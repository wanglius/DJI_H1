#include "mission_control.h"

#include "acquisition.h"
#include "calculation.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "measurement_recorder.h"
#include "sc16is752.h"
#include "sd_card.h"
#include "telemetry.h"

static const char *TAG = "MISSION";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_task;
static bool s_initialized, s_busy, s_pending, s_power_off, s_safe;
static int64_t s_shutdown_deadline_us = INT64_MAX;
static uint32_t s_session;
static uint16_t s_segment;
static uint8_t s_error, s_free_percent;
#define SHUTDOWN_ACQUISITION_RESERVE_US 2000000LL
#define SHUTDOWN_FINAL_IO_RESERVE_US     500000LL
/* Recent completed IDs suppress delayed stale commands. The ring never blocks
 * a healthy flight after an arbitrary number of capture intervals. */
#define SESSION_LIMIT 64
static uint32_t s_sessions[SESSION_LIMIT];
static size_t s_session_count;

static int64_t reserve_shutdown_time(int64_t deadline_us, int64_t reserve_us)
{
    int64_t now_us = esp_timer_get_time();
    return deadline_us - now_us > reserve_us ? deadline_us - reserve_us
                                              : deadline_us;
}

/* Error mapping is B-defined: 1 SD, 2 initialization, 3 acquisition/lifecycle,
 * 4 shutdown, 5 measurement-data degradation (decode failures, recorder
 * pressure drops, rejected calculations, telemetry delivery degradation or
 * infrastructure failure, or conflicting A identities). */
static bool known_session(uint32_t session)
{
    size_t count = s_session_count < SESSION_LIMIT ? s_session_count : SESSION_LIMIT;
    for (size_t i = 0; i < count; i++) {
        if (s_sessions[i] == session) return true;
    }
    return false;
}

uint8_t mission_control_start(uint32_t session)
{
    uint8_t result = 0;
    acquisition_status_t acquisition;
    acquisition_get_status(&acquisition);
    bool wake_cleanup = false;
    taskENTER_CRITICAL(&s_lock);
    if (s_power_off) result = 4;
    else if (known_session(session)) result = 0;
    /* Storage/init/shutdown faults remain latched. A safely terminated
     * acquisition fault may be retried with a new session ID. ACK result 1 is
     * the protocol's generic failure; the heartbeat carries the exact cause. */
    else if (s_error && s_error != 3) result = 1;
    else if (!s_initialized || s_busy) {
        result = 2;
        wake_cleanup = acquisition.cleanup_pending;
    }
    else {
        /* Arm before exposing the pending run; a subsequent STOP cannot be
         * overwritten by the worker when it starts preparing the sensors. */
        if (acquisition_arm() != ESP_OK) {
            result = 2;
            wake_cleanup = true;
        } else {
            s_error = 0;
            s_sessions[s_session_count % SESSION_LIMIT] = session;
            s_session_count++;
            s_session = session;
            s_busy = s_pending = true;
        }
    }
    taskEXIT_CRITICAL(&s_lock);
    if (result == 0 || wake_cleanup) xTaskNotifyGive(s_task);
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

uint8_t mission_control_power_off(uint8_t grace_seconds)
{
    int64_t requested_deadline = esp_timer_get_time() +
        (int64_t)grace_seconds * 1000000LL;
    /* This call is deliberately nonblocking. It closes telemetry admission,
     * purges queued cloud work and makes any ACK wait/retry exit promptly. */
    esp_err_t telemetry_result = telemetry_abort_mission();
    if (telemetry_result != ESP_OK) {
        ESP_LOGW(TAG, "Could not abort telemetry at power-off: %s",
                 esp_err_to_name(telemetry_result));
    }
    /* The power-off event was queued by ab_link before this call. Refuse new
     * GPS/external events now so the SD queue becomes finite while the last
     * in-flight spectra complete. */
    measurement_recorder_prepare_shutdown();
    taskENTER_CRITICAL(&s_lock);
    /* A repeated request must never extend the original physical power-cut
     * deadline. Prepare-power-off remains terminal until reset. */
    if (!s_power_off || requested_deadline < s_shutdown_deadline_us)
        s_shutdown_deadline_us = requested_deadline;
    s_power_off = true;
    acquisition_request_stop_before(reserve_shutdown_time(
        s_shutdown_deadline_us, SHUTDOWN_ACQUISITION_RESERVE_US));
    taskEXIT_CRITICAL(&s_lock);
    xTaskNotifyGive(s_task);
    return 0;
}

bool mission_control_ready(void)
{
    taskENTER_CRITICAL(&s_lock);
    bool ready = s_initialized && (!s_error || s_error == 3) && !s_power_off;
    taskEXIT_CRITICAL(&s_lock);
    return ready;
}

void mission_control_get_status(ab_status_report_t *out)
{
    acquisition_status_t acquisition;
    measurement_recorder_status_t recorder;
    telemetry_status_t telemetry;
    measurement_recorder_get_status(&recorder);
    telemetry_get_status(&telemetry);
    taskENTER_CRITICAL(&s_lock);
    acquisition_get_status(&acquisition);
    uint8_t error = s_error;
    if (!error && !recorder.healthy) error = 1;
    if (!error && (recorder.raw_dropped || recorder.gps_dropped ||
                   recorder.events_dropped || recorder.calculation_rejected ||
                   recorder.identity_mismatches))
        error = 5;
    if (!error && (acquisition.errors[0] || acquisition.errors[1])) error = 5;
    /* Delivery loss is mission degradation, not a recorder stop condition.
     * Keep it visible to A while later SD and telemetry submissions continue. */
    if (!error && telemetry.initialized &&
        (!telemetry.healthy || telemetry.messages_failed ||
         telemetry.reflectance_queue_overflows))
        error = 5;
    *out = (ab_status_report_t) {
        .b_state = error ? 2 : (s_initialized ? 1 : 0),
        .actual_capture = acquisition.capturing,
        .error_code = error,
        .storage_free_percent = s_free_percent,
        /* Ground frames are measurement events and potential reflectance rows. */
        .frame_count = acquisition.frames[0],
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
    if (result == ESP_OK) result = calculation_self_test();
    if (result == ESP_OK) result = measurement_recorder_init();
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
        acquisition_status_t retained;
        acquisition_get_status(&retained);
        if (retained.cleanup_pending) {
            esp_err_t cleanup_result = acquisition_retry_cleanup();
            acquisition_get_status(&retained);
            taskENTER_CRITICAL(&s_lock);
            s_busy = retained.capturing || retained.cleanup_pending;
            if (cleanup_result != ESP_OK) s_error = 3;
            taskEXIT_CRITICAL(&s_lock);
            if (retained.cleanup_pending) {
                /* Retry cooperatively without accepting another session or
                 * replacing retained handles. Heartbeats remain independent. */
                vTaskDelay(pdMS_TO_TICKS(250));
                xTaskNotifyGive(s_task);
                continue;
            }
            ESP_LOGI(TAG, "Retained acquisition cleanup completed");
        }
        taskENTER_CRITICAL(&s_lock);
        bool run = s_pending;
        uint32_t session = s_session;
        s_pending = false;
        taskEXIT_CRITICAL(&s_lock);
        if (run) {
            ESP_LOGI(TAG, "Session %lu preparing", (unsigned long)session);
            uint16_t segment;
            taskENTER_CRITICAL(&s_lock);
            segment = ++s_segment;
            taskEXIT_CRITICAL(&s_lock);
            result = measurement_recorder_begin(session, segment);
            bool storage_failure = result != ESP_OK;
            if (result == ESP_OK) result = acquisition_run_dual(0);
            taskENTER_CRITICAL(&s_lock);
            bool power_off = s_power_off;
            taskEXIT_CRITICAL(&s_lock);
            /* On prepare-power-off, the terminal shutdown path below drains
             * the segment and finalizes the mission under one shared deadline. */
            esp_err_t recorder_result = power_off ? ESP_OK :
                measurement_recorder_end();
            if (recorder_result != ESP_OK) storage_failure = true;
            if (result == ESP_OK) result = recorder_result;
            if (result == ESP_OK && !power_off) {
                result = refresh_storage();
                if (result != ESP_OK) storage_failure = true;
            }
            (void)measurement_recorder_log_event(
                MEASUREMENT_EVENT_CAPTURE_RESULT, session, (int32_t)result);
            acquisition_status_t acquisition;
            acquisition_get_status(&acquisition);
            taskENTER_CRITICAL(&s_lock);
            if (result != ESP_OK) {
                s_error = storage_failure ? 1 : 3;
            }
            /* Retain session ownership if failed cleanup leaves a stream
             * potentially active. Faults block reuse; never advertise idle. */
            s_busy = acquisition.capturing || acquisition.cleanup_pending;
            taskEXIT_CRITICAL(&s_lock);
            if (acquisition.cleanup_pending) xTaskNotifyGive(s_task);
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
        int64_t shutdown_deadline_us = s_shutdown_deadline_us;
        taskEXIT_CRITICAL(&s_lock);
        if (shutdown) {
            int64_t recorder_wait_deadline = reserve_shutdown_time(
                shutdown_deadline_us, SHUTDOWN_FINAL_IO_RESERVE_US);
            result = measurement_recorder_shutdown(recorder_wait_deadline);
            if (result == ESP_OK && sd_card_is_mounted())
                result = sd_card_unmount();
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
    esp_err_t result = acquisition_arm();
    if (result != ESP_OK) return result;
    return xTaskCreate(control_task, "mission_control", 6144, NULL, 5, &s_task)
        == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
