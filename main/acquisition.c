#include "acquisition.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "h1.h"
#include "measurement_recorder.h"
#include "sc16is752.h"

static const char *TAG = "DJI_H1_ACQ";

#define FRAME_TIMEOUT_MS 5000
#define REPORT_QUEUE_LENGTH 32
#define SENSOR_COUNT 2
#define RX_STREAM_BUFFER_SIZE 8192
#define RX_SERVICE_PRIORITY 12
#define ACQUISITION_PRIORITY 8
#define LOGGER_PRIORITY 4
#define ACQUISITION_CORE 1
#define LOGGER_CORE 0

typedef struct {
    const char *name;
    sc16_channel_t channel;
    h1_device_t device;
    h1_spectrum_frame_t frame;
    SemaphoreHandle_t done;
    TaskHandle_t task;
    bool task_created;
    bool gate_released; /* Owner-only; never notify a self-deleted worker. */
    bool run; /* Accessed across cores through atomic builtins. */
    uint32_t frames_ok, frame_errors, reports_dropped;
    uint32_t min_interval_us, max_interval_us;
    uint64_t interval_sum_us;
} sensor_context_t;

typedef struct {
    const char *name;
    uint32_t frame_number, receive_us, interval_us, exposure_us;
    int64_t timestamp_us;
    uint8_t exposure_status;
    int16_t spectrum_scale;
    uint16_t sample_count, max_value, max_wavelength_nm;
    uint32_t rx_overruns, software_drops;
    UBaseType_t queue_depth;
} frame_report_t;

static sensor_context_t s_sensors[SENSOR_COUNT] = {
    {.name = "H1-A", .channel = SC16_CHANNEL_A},
    {.name = "H1-B", .channel = SC16_CHANNEL_B},
};
static QueueHandle_t s_report_queue;
static TaskHandle_t s_logger_task;
static SemaphoreHandle_t s_logger_done;
static bool s_logger_created;
static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
static acquisition_status_t s_status;
static bool s_stop_requested;
static bool s_read_failed;

void acquisition_get_status(acquisition_status_t *out)
{
    taskENTER_CRITICAL(&s_status_lock);
    *out = s_status;
    taskEXIT_CRITICAL(&s_status_lock);
}

void acquisition_arm(void)
{
    taskENTER_CRITICAL(&s_status_lock);
    memset(&s_status, 0, sizeof(s_status));
    taskEXIT_CRITICAL(&s_status_lock);
    __atomic_store_n(&s_read_failed, false, __ATOMIC_RELEASE);
    __atomic_store_n(&s_stop_requested, false, __ATOMIC_RELEASE);
}

void acquisition_request_stop(void)
{
    __atomic_store_n(&s_stop_requested, true, __ATOMIC_RELEASE);
}

static const char *status_name(uint8_t status)
{
    switch ((h1_exposure_status_t)status) {
        case H1_EXPOSURE_STATUS_NORMAL: return "NORMAL";
        case H1_EXPOSURE_STATUS_OVER: return "OVER";
        case H1_EXPOSURE_STATUS_UNDER: return "UNDER";
        default: return "UNKNOWN";
    }
}

static void acquisition_task(void *arg)
{
    sensor_context_t *ctx = (sensor_context_t *)arg;
    int64_t previous_us = 0;
    unsigned consecutive_errors = 0;
    size_t sensor_index = (size_t)(ctx - s_sensors);
    ESP_LOGI(TAG, "%s task ready on UART-%c; waiting for start gate",
             ctx->name, ctx->channel == SC16_CHANNEL_A ? 'A' : 'B');
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "%s acquisition released at %lld us",
             ctx->name, (long long)esp_timer_get_time());

    while (__atomic_load_n(&ctx->run, __ATOMIC_ACQUIRE)) {
        int64_t receive_start_us = esp_timer_get_time();
        esp_err_t ret = h1_read_stream_frame(
            &ctx->device, &ctx->frame, FRAME_TIMEOUT_MS
        );
        int64_t now_us = esp_timer_get_time();
        if (ret != ESP_OK) {
            ctx->frame_errors++;
            taskENTER_CRITICAL(&s_status_lock);
            s_status.errors[sensor_index] = ctx->frame_errors;
            taskEXIT_CRITICAL(&s_status_lock);
            /* Bound a broken stream; the owner stops both channels safely. */
            if (++consecutive_errors >= 3) {
                __atomic_store_n(&s_read_failed, true, __ATOMIC_RELEASE);
                acquisition_request_stop();
                break;
            }
            if (ctx->frame_errors <= 3 || (ctx->frame_errors % 100) == 0) {
                ESP_LOGE(TAG, "%s error #%lu after %.1f ms: %s; overruns=%lu",
                         ctx->name, (unsigned long)ctx->frame_errors,
                         (now_us - receive_start_us) / 1000.0,
                         esp_err_to_name(ret),
                         (unsigned long)sc16_get_channel_rx_overrun_count(
                             ctx->channel));
            }
            vTaskDelay(1);
            continue;
        }

        ctx->frames_ok++;
        consecutive_errors = 0;
        taskENTER_CRITICAL(&s_status_lock);
        s_status.frames[sensor_index] = ctx->frames_ok;
        taskEXIT_CRITICAL(&s_status_lock);
        esp_err_t record_result = measurement_recorder_submit(
            sensor_index == 0 ? SPECTROMETER_GROUND : SPECTROMETER_SKY,
            ctx->frames_ok, &ctx->frame, now_us,
            !__atomic_load_n(&s_stop_requested, __ATOMIC_ACQUIRE));
        if (record_result == ESP_ERR_NO_MEM) {
            /* Preserve the flight: frame_count makes this explicit gap
             * detectable, and recorder status exposes accumulated pressure. */
            if (ctx->frames_ok <= 3 || (ctx->frames_ok % 100) == 0) {
                ESP_LOGW(TAG, "%s raw frame %lu dropped by recorder pressure",
                         ctx->name, (unsigned long)ctx->frames_ok);
            }
        } else if (record_result != ESP_OK) {
            ESP_LOGE(TAG, "%s raw frame %lu could not be queued: %s",
                     ctx->name, (unsigned long)ctx->frames_ok,
                     esp_err_to_name(record_result));
            __atomic_store_n(&s_read_failed, true, __ATOMIC_RELEASE);
            acquisition_request_stop();
            break;
        }
        uint32_t interval_us = previous_us == 0
            ? 0 : (uint32_t)(now_us - previous_us);
        previous_us = now_us;
        if (interval_us != 0) {
            if (ctx->min_interval_us == 0 ||
                interval_us < ctx->min_interval_us) {
                ctx->min_interval_us = interval_us;
            }
            if (interval_us > ctx->max_interval_us) {
                ctx->max_interval_us = interval_us;
            }
            ctx->interval_sum_us += interval_us;
        }

        uint16_t max_value = 0;
        size_t max_index = 0;
        for (size_t i = 0; i < ctx->frame.sample_count; i++) {
            if (ctx->frame.spectrum[i] > max_value) {
                max_value = ctx->frame.spectrum[i];
                max_index = i;
            }
        }

        frame_report_t report = {
            .name = ctx->name, .frame_number = ctx->frames_ok,
            .timestamp_us = now_us,
            .receive_us = (uint32_t)(now_us - receive_start_us),
            .interval_us = interval_us, .exposure_us = ctx->frame.exposure_us,
            .exposure_status = (uint8_t)ctx->frame.exposure_status,
            .spectrum_scale = ctx->frame.spectrum_scale,
            .sample_count = (uint16_t)ctx->frame.sample_count,
            .max_value = max_value,
            .max_wavelength_nm = (uint16_t)(340 + max_index),
            .rx_overruns = sc16_get_channel_rx_overrun_count(ctx->channel),
            .software_drops = sc16_get_software_rx_drop_count(ctx->channel),
            .queue_depth = uxQueueMessagesWaiting(s_report_queue),
        };
        if (xQueueSend(s_report_queue, &report, 0) != pdTRUE) {
            ctx->reports_dropped++;
            ESP_LOGW(TAG, "%s report dropped at frame %lu",
                     ctx->name, (unsigned long)ctx->frames_ok);
        }
    }

    ESP_LOGI(TAG, "%s stopped: ok=%lu errors=%lu dropped=%lu", ctx->name,
             (unsigned long)ctx->frames_ok, (unsigned long)ctx->frame_errors,
             (unsigned long)ctx->reports_dropped);
    /* Owner retains the handle until it has received done; never race a
     * concurrent stop notification with a worker clearing its own handle. */
    xSemaphoreGive(ctx->done);
    vTaskDelete(NULL);
}

static void logger_task(void *arg)
{
    (void)arg;
    frame_report_t r;
    ESP_LOGI(TAG, "Detailed dual-channel logger started");
    while (true) {
        if (xQueueReceive(s_report_queue, &r, portMAX_DELAY) == pdTRUE) {
            if (r.name == NULL) {
                break;
            }
            printf("%s frame=%4lu t=%10.3fms rx=%7.1fms dt=%7.1fms "
                   "exp=%8luus status=%-6s n=%3u scale=%d max=%5u@%4unm "
                   "hwov=%lu swdrop=%lu q=%u\n",
                   r.name, (unsigned long)r.frame_number, r.timestamp_us / 1000.0,
                   r.receive_us / 1000.0, r.interval_us / 1000.0,
                   (unsigned long)r.exposure_us, status_name(r.exposure_status),
                   (unsigned)r.sample_count, (int)r.spectrum_scale,
                   (unsigned)r.max_value, (unsigned)r.max_wavelength_nm,
                   (unsigned long)r.rx_overruns,
                   (unsigned long)r.software_drops, (unsigned)r.queue_depth);
        }
    }

    s_logger_task = NULL;
    xSemaphoreGive(s_logger_done);
    vTaskDelete(NULL);
}

static void reset_sensor_run_state(sensor_context_t *ctx)
{
    memset(&ctx->device, 0, sizeof(ctx->device));
    memset(&ctx->frame, 0, sizeof(ctx->frame));
    ctx->done = NULL;
    ctx->task = NULL;
    ctx->task_created = false;
    ctx->gate_released = false;
    __atomic_store_n(&ctx->run, false, __ATOMIC_RELEASE);
    ctx->frames_ok = 0;
    ctx->frame_errors = 0;
    ctx->reports_dropped = 0;
    ctx->min_interval_us = 0;
    ctx->max_interval_us = 0;
    ctx->interval_sum_us = 0;
}

static esp_err_t prepare_sensor(sensor_context_t *ctx)
{
    ESP_LOGI(TAG, "Preparing %s on UART-%c", ctx->name,
             ctx->channel == SC16_CHANNEL_A ? 'A' : 'B');
    esp_err_t ret = sc16_test_channel(ctx->channel);
    if (ret != ESP_OK) return ret;
    ret = sc16_uart_init(ctx->channel);
    if (ret != ESP_OK) return ret;
    ret = h1_init(&ctx->device, ctx->channel);
    if (ret != ESP_OK) return ret;
    ret = h1_get_device_info(&ctx->device);
    if (ret != ESP_OK) return ret;
    ESP_LOGI(TAG, "%s identity: %s", ctx->name, ctx->device.device_info);
    ret = h1_set_exposure_mode(&ctx->device, H1_EXPOSURE_AUTO);
    if (ret != ESP_OK) return ret;
    h1_exposure_mode_t mode = H1_EXPOSURE_MANUAL;
    ret = h1_get_exposure_mode(&ctx->device, &mode);
    if (ret != ESP_OK) return ret;
    ESP_LOGI(TAG, "%s exposure mode: %s", ctx->name,
             mode == H1_EXPOSURE_AUTO ? "AUTO" : "MANUAL");
    return mode == H1_EXPOSURE_AUTO ? ESP_OK : ESP_FAIL;
}

static void print_summary(const sensor_context_t *ctx,
                          uint32_t acquisition_overruns,
                          uint32_t shutdown_overruns,
                          uint32_t acquisition_drops,
                          uint32_t shutdown_drops)
{
    uint32_t intervals = ctx->frames_ok > 0 ? ctx->frames_ok - 1 : 0;
    double average_ms = intervals == 0 ? 0.0
        : (double)ctx->interval_sum_us / intervals / 1000.0;
    printf("%-4s device                 : %s\n", ctx->name, ctx->device.device_info);
    printf("%-4s frames OK              : %lu\n", ctx->name, (unsigned long)ctx->frames_ok);
    printf("%-4s frame errors           : %lu\n", ctx->name, (unsigned long)ctx->frame_errors);
    printf("%-4s reports dropped        : %lu\n", ctx->name, (unsigned long)ctx->reports_dropped);
    printf("%-4s interval ms            : min=%.1f avg=%.1f max=%.1f\n",
           ctx->name, ctx->min_interval_us / 1000.0, average_ms,
           ctx->max_interval_us / 1000.0);
    printf("%-4s acquisition overruns   : %lu\n", ctx->name,
           (unsigned long)acquisition_overruns);
    printf("%-4s shutdown overruns      : %lu\n", ctx->name,
           (unsigned long)shutdown_overruns);
    printf("%-4s acquisition SW drops   : %lu\n", ctx->name,
           (unsigned long)acquisition_drops);
    printf("%-4s shutdown SW drops      : %lu\n", ctx->name,
           (unsigned long)shutdown_drops);
}

static esp_err_t create_tasks(void)
{
    s_report_queue = xQueueCreate(REPORT_QUEUE_LENGTH, sizeof(frame_report_t));
    if (s_report_queue == NULL) return ESP_ERR_NO_MEM;

    s_logger_done = xSemaphoreCreateBinary();
    if (s_logger_done == NULL) return ESP_ERR_NO_MEM;
    for (size_t i = 0; i < SENSOR_COUNT; i++) {
        s_sensors[i].done = xSemaphoreCreateBinary();
        __atomic_store_n(&s_sensors[i].run, true, __ATOMIC_RELEASE);
        if (s_sensors[i].done == NULL) return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCore(logger_task, "h1_dual_log", 4096, NULL,
                                LOGGER_PRIORITY, &s_logger_task,
                                LOGGER_CORE) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_logger_created = true;
    for (size_t i = 0; i < SENSOR_COUNT; i++) {
        const char *name = i == 0 ? "h1_acq_A" : "h1_acq_B";
        if (xTaskCreatePinnedToCore(acquisition_task, name, 6144,
                                    &s_sensors[i], ACQUISITION_PRIORITY,
                                    &s_sensors[i].task,
                                    ACQUISITION_CORE) != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
        s_sensors[i].task_created = true;
    }
    return ESP_OK;
}

static esp_err_t stop_reader_stream(sensor_context_t *ctx,
                                    bool *stream_started)
{
    if (!*stream_started) return ESP_OK;
    ESP_LOGI(TAG, "Stopping %s stream after its reader exited", ctx->name);
    esp_err_t result = h1_stop_stream(&ctx->device);
    if (result == ESP_OK) {
        *stream_started = false;
    } else {
        ESP_LOGE(TAG, "%s stop failed: %s", ctx->name,
                 esp_err_to_name(result));
    }
    return result;
}

static esp_err_t stop_acquisition_tasks(bool stream_started[SENSOR_COUNT],
                                        bool *all_tasks_stopped)
{
    *all_tasks_stopped = false;
    for (size_t i = 0; i < SENSOR_COUNT; i++) {
        __atomic_store_n(&s_sensors[i].run, false, __ATOMIC_RELEASE);
        if (s_sensors[i].task_created && !s_sensors[i].gate_released) {
            xTaskNotifyGive(s_sensors[i].task);
            s_sensors[i].gate_released = true;
        }
    }

    bool waiting[SENSOR_COUNT] = {false, false};
    size_t pending = 0;
    for (size_t i = 0; i < SENSOR_COUNT; i++) {
        waiting[i] = s_sensors[i].task_created;
        if (waiting[i]) pending++;
    }

    esp_err_t result = ESP_OK;
    int64_t deadline_us = esp_timer_get_time() + 6000000;
    while (pending != 0) {
        for (size_t i = 0; i < SENSOR_COUNT; i++) {
            if (!waiting[i] ||
                xSemaphoreTake(s_sensors[i].done, 0) != pdTRUE) {
                continue;
            }
            waiting[i] = false;
            pending--;
            s_sensors[i].task = NULL;
            s_sensors[i].task_created = false;

            /* Stop a completed reader's device immediately. With asymmetric
             * exposures, waiting for the other task can otherwise fill this
             * channel's abandoned software buffer. */
            esp_err_t stop_result = stop_reader_stream(
                &s_sensors[i], &stream_started[i]);
            if (stop_result != ESP_OK && result == ESP_OK) {
                result = stop_result;
            }
        }
        if (pending == 0) break;
        if (esp_timer_get_time() >= deadline_us) {
            for (size_t i = 0; i < SENSOR_COUNT; i++) {
                if (waiting[i]) {
                    ESP_LOGE(TAG,
                             "%s did not stop within 6 seconds; task retained",
                             s_sensors[i].name);
                }
            }
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }

    /* Any remaining stream has no reader: either task setup was partial or
     * the first stop attempt failed after its reader exited. Retry safely. */
    for (size_t i = 0; i < SENSOR_COUNT; i++) {
        if (!stream_started[i]) continue;
        esp_err_t stop_result = stop_reader_stream(
            &s_sensors[i], &stream_started[i]);
        if (stop_result != ESP_OK && result == ESP_OK) {
            result = stop_result;
        }
    }
    *all_tasks_stopped = true;
    return result;
}

static esp_err_t stop_logger_task(void)
{
    if (!s_logger_created) return ESP_OK;

    const frame_report_t stop_report = {0};
    if (xQueueSend(s_report_queue, &stop_report,
                   pdMS_TO_TICKS(1000)) != pdTRUE ||
        xSemaphoreTake(s_logger_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Logger task did not stop; task retained");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void delete_runtime_objects(void)
{
    for (size_t i = 0; i < SENSOR_COUNT; i++) {
        if (s_sensors[i].done != NULL) {
            vSemaphoreDelete(s_sensors[i].done);
            s_sensors[i].done = NULL;
        }
        s_sensors[i].task_created = false;
    }
    if (s_logger_done != NULL) {
        vSemaphoreDelete(s_logger_done);
        s_logger_done = NULL;
    }
    if (s_report_queue != NULL) {
        vQueueDelete(s_report_queue);
        s_report_queue = NULL;
    }
    s_logger_created = false;
}

static void configure_watchdog(void)
{
    const esp_task_wdt_config_t config = {
        .timeout_ms = CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000U,
        .idle_core_mask = (1U << 0) | (1U << 1),
        .trigger_panic = false,
    };
    esp_err_t ret = esp_task_wdt_reconfigure(&config);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "TWDT reconfigured: monitor IDLE0 and IDLE1");
    } else {
        ESP_LOGW(TAG, "Could not reconfigure TWDT for streaming: %s",
                 esp_err_to_name(ret));
    }
}

esp_err_t acquisition_prepare_dual(void)
{
    for (size_t i = 0; i < SENSOR_COUNT; i++) {
        reset_sensor_run_state(&s_sensors[i]);
        esp_err_t result = prepare_sensor(&s_sensors[i]);
        if (result != ESP_OK) return result;
    }
    return ESP_OK;
}

esp_err_t acquisition_run_dual(uint32_t duration_ms)
{
    bool service_started = false;
    bool stream_started[SENSOR_COUNT] = {false, false};
    bool tasks_stopped = false;
    esp_err_t result = ESP_OK;

    /* Cancellation is armed by the owner before publishing STARTING, not
     * here: a stop received before this task wakes must never be lost. */

    for (size_t i = 0; i < SENSOR_COUNT; i++) {
        reset_sensor_run_state(&s_sensors[i]);
        if (__atomic_load_n(&s_stop_requested, __ATOMIC_ACQUIRE)) goto cleanup;
        result = prepare_sensor(&s_sensors[i]);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "%s preparation failed: %s",
                     s_sensors[i].name, esp_err_to_name(result));
            goto cleanup;
        }
    }

    if (__atomic_load_n(&s_stop_requested, __ATOMIC_ACQUIRE)) goto cleanup;

    result = sc16_start_dual_rx_service(
        RX_STREAM_BUFFER_SIZE, RX_SERVICE_PRIORITY, ACQUISITION_CORE);
    if (result != ESP_OK) goto cleanup;
    service_started = true;
    ESP_LOGI(TAG, "SC16 service active: %u-byte software buffer per UART",
             RX_STREAM_BUFFER_SIZE);
    result = create_tasks();
    if (result != ESP_OK) goto cleanup;
    configure_watchdog();

    sc16_reset_rx_overrun_count();
    /* Sky is the denominator for every ground-derived reflectance row. Prime
     * its stream first so a short capture segment cannot lose its initial
     * ground frames merely because no causal sky reference exists yet. */
    int64_t start_b_us = esp_timer_get_time();
    result = h1_start_stream(&s_sensors[1].device);
    if (result != ESP_OK) goto cleanup;
    stream_started[1] = true;
    /* A partial dual-start failure still leaves real hardware streaming.
     * Keep capture asserted until cleanup has stopped every started channel. */
    taskENTER_CRITICAL(&s_status_lock);
    s_status.capturing = true;
    taskEXIT_CRITICAL(&s_status_lock);
    xTaskNotifyGive(s_sensors[1].task);
    s_sensors[1].gate_released = true;

    int64_t prime_deadline = esp_timer_get_time() + FRAME_TIMEOUT_MS * 1000LL;
    while (!__atomic_load_n(&s_stop_requested, __ATOMIC_ACQUIRE)) {
        acquisition_status_t status;
        acquisition_get_status(&status);
        if (status.frames[1] != 0) break;
        if (esp_timer_get_time() >= prime_deadline) {
            ESP_LOGE(TAG, "Sky stream did not produce its priming frame");
            result = ESP_ERR_TIMEOUT;
            goto cleanup;
        }
        vTaskDelay(1);
    }
    if (__atomic_load_n(&s_stop_requested, __ATOMIC_ACQUIRE)) goto cleanup;

    int64_t start_a_us = esp_timer_get_time();
    result = h1_start_stream(&s_sensors[0].device);
    if (result != ESP_OK) goto cleanup;
    stream_started[0] = true;
    ESP_LOGI(TAG, "Sky primed; streams commanded B=%lldus A=%lldus separation=%.3fms",
             (long long)start_b_us, (long long)start_a_us,
             (start_a_us - start_b_us) / 1000.0);
    xTaskNotifyGive(s_sensors[0].task);
    s_sensors[0].gate_released = true;

    printf("\n============================================================\n");
    if (duration_ms) {
        printf(" BOTH CHANNELS ACQUIRING FOR %.1f SECONDS\n", duration_ms / 1000.0);
    } else {
        printf(" BOTH CHANNELS ACQUIRING UNTIL A-BOARD STOP\n");
    }
    printf("============================================================\n");
    int64_t deadline = duration_ms == 0 ? INT64_MAX :
        esp_timer_get_time() + (int64_t)duration_ms * 1000;
    while (!__atomic_load_n(&s_stop_requested, __ATOMIC_ACQUIRE) &&
           esp_timer_get_time() < deadline) {
        vTaskDelay(1);
    }

    uint32_t acquisition_overruns[SENSOR_COUNT], shutdown_overruns[SENSOR_COUNT];
    uint32_t acquisition_drops[SENSOR_COUNT], shutdown_drops[SENSOR_COUNT];
    /* Snapshot while both reader tasks still own their live streams. Bytes
     * received after this point are shutdown traffic, not acquisition loss. */
    for (size_t i = 0; i < SENSOR_COUNT; i++) {
        acquisition_overruns[i] = sc16_get_channel_rx_overrun_count(s_sensors[i].channel);
        acquisition_drops[i] = sc16_get_software_rx_drop_count(s_sensors[i].channel);
    }
    ESP_LOGI(TAG, "Requesting both acquisition tasks to stop");
    /* Also covers duration-based test stops, ensuring any frame that completes
     * during coordinated teardown is recorded raw without reflectance pairing. */
    acquisition_request_stop();
    result = stop_acquisition_tasks(stream_started, &tasks_stopped);
    if (result != ESP_OK) goto cleanup;
    for (size_t i = 0; i < SENSOR_COUNT; i++) {
        uint32_t total = sc16_get_channel_rx_overrun_count(s_sensors[i].channel);
        shutdown_overruns[i] = total - acquisition_overruns[i];
        total = sc16_get_software_rx_drop_count(s_sensors[i].channel);
        shutdown_drops[i] = total - acquisition_drops[i];
    }

    vTaskDelay(pdMS_TO_TICKS(200));
    printf("\n============================================================\n");
    printf(" DUAL UART STREAMING TEST RESULT\n");
    printf("============================================================\n");
    for (size_t i = 0; i < SENSOR_COUNT; i++) {
        print_summary(&s_sensors[i], acquisition_overruns[i],
                      shutdown_overruns[i], acquisition_drops[i],
                      shutdown_drops[i]);
        printf("------------------------------------------------------------\n");
    }
    printf("Start-command separation    : %.3f ms\n",
           (start_a_us - start_b_us) / 1000.0);
    printf("Free heap after test        : %lu bytes\n",
           (unsigned long)esp_get_free_heap_size());
    printf("============================================================\n");

cleanup:
    if (!tasks_stopped) {
        esp_err_t stop_ret = stop_acquisition_tasks(stream_started,
                                                    &tasks_stopped);
        if (stop_ret != ESP_OK && result == ESP_OK) {
            result = stop_ret;
        }
    }

    if (tasks_stopped) {
        for (size_t i = 0; i < SENSOR_COUNT; i++) {
            if (!stream_started[i]) continue;
            ESP_LOGI(TAG, "Rollback: stopping %s stream", s_sensors[i].name);
            esp_err_t stop_ret = h1_stop_stream(&s_sensors[i].device);
            if (stop_ret != ESP_OK) {
                ESP_LOGE(TAG, "%s rollback stop failed: %s",
                         s_sensors[i].name, esp_err_to_name(stop_ret));
                if (result == ESP_OK) result = stop_ret;
            }
            if (stop_ret == ESP_OK) stream_started[i] = false;
        }

        esp_err_t logger_ret = stop_logger_task();
        if (logger_ret != ESP_OK && result == ESP_OK) result = logger_ret;
        if (logger_ret == ESP_OK) delete_runtime_objects();
    }

    if (service_started && tasks_stopped) {
        esp_err_t service_ret = sc16_stop_dual_rx_service(1000);
        if (service_ret != ESP_OK) {
            ESP_LOGE(TAG, "RX service stop failed: %s",
                     esp_err_to_name(service_ret));
            if (result == ESP_OK) result = service_ret;
        }
    }
    if (__atomic_load_n(&s_read_failed, __ATOMIC_ACQUIRE) && result == ESP_OK) {
        result = ESP_FAIL;
    }
    if (tasks_stopped && !stream_started[0] && !stream_started[1]) {
        taskENTER_CRITICAL(&s_status_lock);
        s_status.capturing = false;
        taskEXIT_CRITICAL(&s_status_lock);
    }
    return result;
}
