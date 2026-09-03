#include "mission_control.h"

#include "acquisition.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sc16is752.h"
#include "sd_card.h"

static const char *TAG = "MISSION";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_task;
static bool s_initialized, s_busy, s_pending, s_power_off, s_safe;
static uint32_t s_session;
static uint8_t s_error, s_free_percent;
/* Bounded boot-local history. Refuse further new sessions when full rather
 * than evicting an old ID and accidentally restarting a completed mission. */
#define SESSION_LIMIT 64
static uint32_t s_sessions[SESSION_LIMIT];
static size_t s_session_count;

/* Error mapping is B-defined: 1 SD, 2 initialization, 3 acquisition/lifecycle,
 * 4 shutdown, 5 decoded-frame errors (latched for the current session). */
static bool known_session(uint32_t session)
{
    for (size_t i = 0; i < s_session_count; i++) {
        if (s_sessions[i] == session) return true;
    }
    return false;
}

uint8_t mission_control_start(uint32_t session)
{
    uint8_t result = 0;
    taskENTER_CRITICAL(&s_lock);
    if (s_power_off) result = 4;
    else if (known_session(session)) result = 0;
    else if (s_error) result = 1;
    else if (!s_initialized || s_busy || s_session_count == SESSION_LIMIT) result = 2;
    else {
        /* Arm before exposing the pending run; a subsequent STOP cannot be
         * overwritten by the worker when it starts preparing the sensors. */
        acquisition_arm();
        s_sessions[s_session_count++] = session;
        s_session = session;
        s_busy = s_pending = true;
    }
    taskEXIT_CRITICAL(&s_lock);
    if (result == 0) xTaskNotifyGive(s_task);
    return result;
}

uint8_t mission_control_stop(uint32_t session)
{
    taskENTER_CRITICAL(&s_lock);
    uint8_t result = known_session(session) ? 0 : 4;
    if (s_busy && session == s_session) acquisition_request_stop();
    taskEXIT_CRITICAL(&s_lock);
    return result;
}

uint8_t mission_control_power_off(void)
{
    taskENTER_CRITICAL(&s_lock);
    s_power_off = true; /* Terminal until reset; no new starts after shutdown. */
    acquisition_request_stop();
    taskEXIT_CRITICAL(&s_lock);
    xTaskNotifyGive(s_task);
    return 0;
}

bool mission_control_ready(void)
{
    taskENTER_CRITICAL(&s_lock);
    bool ready = s_initialized && !s_error && !s_power_off;
    taskEXIT_CRITICAL(&s_lock);
    return ready;
}

void mission_control_get_status(ab_status_report_t *out)
{
    acquisition_status_t acquisition;
    taskENTER_CRITICAL(&s_lock);
    acquisition_get_status(&acquisition);
    uint8_t error = s_error;
    if (!error && (acquisition.errors[0] || acquisition.errors[1])) error = 5;
    *out = (ab_status_report_t) {
        .b_state = error ? 2 : (s_initialized ? 1 : 0),
        .actual_capture = acquisition.capturing,
        .error_code = error,
        .storage_free_percent = s_free_percent,
        /* Total decoded spectra, A+B; NOT synchronized pairs or saved frames. */
        .frame_count = acquisition.frames[0] + acquisition.frames[1],
        .session_id = s_busy ? s_session : 0,
        .safe_power_off = s_safe,
    };
    taskEXIT_CRITICAL(&s_lock);
}

static esp_err_t refresh_storage(void)
{
    uint64_t total = 0, free_bytes = 0;
    esp_err_t result = sd_card_get_space(&total, &free_bytes);
    if (result == ESP_OK && (total == 0 || free_bytes > total)) result = ESP_FAIL;
    if (result == ESP_OK) {
        taskENTER_CRITICAL(&s_lock);
        s_free_percent = (uint8_t)(free_bytes * 100 / total);
        taskEXIT_CRITICAL(&s_lock);
    }
    return result;
}

static void control_task(void *unused)
{
    (void)unused;
    const sd_card_config_t sd = SD_CARD_LILYGO_T8_S3_DEFAULT_CONFIG();
    esp_err_t result = sd_card_mount(&sd);
    if (result == ESP_OK) result = refresh_storage();
    uint8_t error = result == ESP_OK ? 0 : 1;
    if (!error) {
        const sc16_config_t bridge = {
            .spi_host = SPI3_HOST, .pin_mosi = 2, .pin_miso = 3,
            .pin_sclk = 5, .pin_cs = 1, .pin_reset = 6,
            .spi_clock_hz = 4000000, .crystal_hz = 1843200,
        };
        result = sc16_init(&bridge);
        if (result == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(500));
            result = acquisition_prepare_dual();
        }
        if (result != ESP_OK) error = 2;
    }
    taskENTER_CRITICAL(&s_lock);
    s_error = error;
    s_initialized = !error;
    taskEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "Initialization: %s; awaiting A-board commands", esp_err_to_name(result));

    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        taskENTER_CRITICAL(&s_lock);
        bool run = s_pending;
        uint32_t session = s_session;
        s_pending = false;
        taskEXIT_CRITICAL(&s_lock);
        if (run) {
            ESP_LOGI(TAG, "Session %lu preparing", (unsigned long)session);
            result = acquisition_run_dual(0);
            if (result == ESP_OK) result = refresh_storage();
            acquisition_status_t acquisition;
            acquisition_get_status(&acquisition);
            taskENTER_CRITICAL(&s_lock);
            if (result != ESP_OK) s_error = 3;
            /* Retain session ownership if failed cleanup leaves a stream
             * potentially active. Faults block reuse; never advertise idle. */
            s_busy = acquisition.capturing;
            taskEXIT_CRITICAL(&s_lock);
            ESP_LOGI(TAG, "Session %lu finished: %s", (unsigned long)session,
                     esp_err_to_name(result));
        }
        taskENTER_CRITICAL(&s_lock);
        /* A new run may have been admitted after the previous one published
         * idle. Consume that pending (now cancelled) run before unmounting;
         * otherwise its capacity refresh would touch an already unmounted SD.
         * Retained active resources on failure must never qualify as safe. */
        bool shutdown = s_power_off && !s_pending && !s_busy && !s_safe;
        bool healthy = s_error == 0;
        taskEXIT_CRITICAL(&s_lock);
        if (shutdown) {
            /* There is no recorder yet and this mode opens no files. When
             * adding one, drain/flush/close it HERE before unmount and safe=1.
             * Never advertise safety on a failed or retained-resource path. */
            result = sd_card_is_mounted() ? sd_card_unmount() : ESP_OK;
            taskENTER_CRITICAL(&s_lock);
            s_safe = healthy && result == ESP_OK;
            if (result != ESP_OK) s_error = 4;
            taskEXIT_CRITICAL(&s_lock);
            ESP_LOGI(TAG, "Shutdown complete: safe=%u result=%s", s_safe,
                     esp_err_to_name(result));
        }
    }
}

esp_err_t mission_control_init(void)
{
    if (s_task != NULL) return ESP_ERR_INVALID_STATE;
    acquisition_arm();
    return xTaskCreate(control_task, "mission_control", 6144, NULL, 5, &s_task)
        == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
