#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"
#include "sc16is752.h"
#include "h1.h"

static const char *TAG = "DJI_H1_DUAL";
#define TEST_DURATION_MS 30000
#define FRAME_TIMEOUT_MS 5000
#define REPORT_QUEUE_LENGTH 32
#define SENSOR_COUNT 2

typedef struct {
    const char *name;
    sc16_channel_t channel;
    h1_device_t device;
    h1_spectrum_frame_t frame;
    SemaphoreHandle_t done;
    TaskHandle_t task;
    /* Accessed by tasks on different cores; use atomic builtins below. */
    bool run;
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
    uint32_t rx_overruns;
    uint32_t software_drops;
    UBaseType_t queue_depth;
} frame_report_t;

static sensor_context_t g_sensors[SENSOR_COUNT] = {
    {.name = "H1-A", .channel = SC16_CHANNEL_A},
    {.name = "H1-B", .channel = SC16_CHANNEL_B},
};
static QueueHandle_t g_report_queue;

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
    ESP_LOGI(TAG, "%s task ready on UART-%c; waiting for start gate",
             ctx->name, ctx->channel == SC16_CHANNEL_A ? 'A' : 'B');
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "%s acquisition released at %lld us",
             ctx->name, (long long)esp_timer_get_time());

    while (__atomic_load_n(&ctx->run, __ATOMIC_ACQUIRE)) {
        int64_t receive_start_us = esp_timer_get_time();
        esp_err_t ret = h1_read_stream_frame(&ctx->device, &ctx->frame,
                                              FRAME_TIMEOUT_MS);
        int64_t now_us = esp_timer_get_time();
        if (ret != ESP_OK) {
            ctx->frame_errors++;
            if (ctx->frame_errors <= 3 || (ctx->frame_errors % 100) == 0) {
                ESP_LOGE(TAG, "%s error #%lu after %.1f ms: %s; overruns=%lu",
                         ctx->name, (unsigned long)ctx->frame_errors,
                         (now_us - receive_start_us) / 1000.0,
                         esp_err_to_name(ret),
                         (unsigned long)sc16_get_channel_rx_overrun_count(ctx->channel));
            }
            taskYIELD();
            continue;
        }

        ctx->frames_ok++;
        uint32_t interval_us = previous_us == 0 ? 0 : (uint32_t)(now_us - previous_us);
        previous_us = now_us;
        if (interval_us != 0) {
            if (ctx->min_interval_us == 0 || interval_us < ctx->min_interval_us)
                ctx->min_interval_us = interval_us;
            if (interval_us > ctx->max_interval_us) ctx->max_interval_us = interval_us;
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
            .max_value = max_value, .max_wavelength_nm = (uint16_t)(340 + max_index),
            .rx_overruns = sc16_get_channel_rx_overrun_count(ctx->channel),
            .software_drops = sc16_get_software_rx_drop_count(ctx->channel),
            .queue_depth = uxQueueMessagesWaiting(g_report_queue),
        };
        if (xQueueSend(g_report_queue, &report, 0) != pdTRUE) {
            ctx->reports_dropped++;
            ESP_LOGW(TAG, "%s report dropped at frame %lu",
                     ctx->name, (unsigned long)ctx->frames_ok);
        }
    }

    ESP_LOGI(TAG, "%s stopped: ok=%lu errors=%lu dropped=%lu", ctx->name,
             (unsigned long)ctx->frames_ok, (unsigned long)ctx->frame_errors,
             (unsigned long)ctx->reports_dropped);
    xSemaphoreGive(ctx->done);
    vTaskDelete(NULL);
}

static void logger_task(void *arg)
{
    (void)arg;
    frame_report_t r;
    ESP_LOGI(TAG, "Detailed dual-channel logger started");
    while (true) {
        if (xQueueReceive(g_report_queue, &r, portMAX_DELAY) == pdTRUE) {
            printf("%s frame=%4lu t=%10.3fms rx=%7.1fms dt=%7.1fms "
                   "exp=%8luus status=%-6s n=%3u scale=%d max=%5u@%4unm "
                   "hwov=%lu swdrop=%lu q=%u\n",
                   r.name, (unsigned long)r.frame_number, r.timestamp_us / 1000.0,
                   r.receive_us / 1000.0, r.interval_us / 1000.0,
                   (unsigned long)r.exposure_us, status_name(r.exposure_status),
                   (unsigned)r.sample_count, (int)r.spectrum_scale,
                   (unsigned)r.max_value, (unsigned)r.max_wavelength_nm,
                   (unsigned long)r.rx_overruns,
                   (unsigned long)r.software_drops,
                   (unsigned)r.queue_depth);
        }
    }
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

static void print_summary(
    const sensor_context_t *ctx,
    uint32_t acquisition_overruns,
    uint32_t shutdown_overruns,
    uint32_t acquisition_drops,
    uint32_t shutdown_drops)
{
    uint32_t intervals = ctx->frames_ok > 0 ? ctx->frames_ok - 1 : 0;
    double average_ms = intervals == 0 ? 0.0 :
        (double)ctx->interval_sum_us / intervals / 1000.0;
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

void app_main(void)
{
    printf("\n============================================================\n");
    printf(" DJI_H1 - DUAL UART 30-SECOND DIAGNOSTIC STREAMING TEST\n");
    printf("============================================================\n");
    ESP_LOGI(TAG, "Free heap at boot: %lu bytes", (unsigned long)esp_get_free_heap_size());

    const sc16_config_t config = {
        .spi_host = SPI2_HOST, .pin_mosi = 2, .pin_miso = 3,
        .pin_sclk = 5, .pin_cs = 1, .pin_reset = 6,
        /* Dual-channel diagnostic: 4 MHz gives the shared SPI bus
         * enough service margin for two simultaneous 115200-baud UARTs. */
        .spi_clock_hz = 4000000, .crystal_hz = 1843200,
    };
    ESP_ERROR_CHECK(sc16_init(&config));
    for (size_t i = 0; i < SENSOR_COUNT; i++) {
        esp_err_t ret = prepare_sensor(&g_sensors[i]);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "%s preparation failed: %s", g_sensors[i].name,
                     esp_err_to_name(ret));
            return;
        }
    }

    ESP_ERROR_CHECK(sc16_start_dual_rx_service(8192, 12, 1));
    ESP_LOGI(TAG, "SC16 service active: 8192-byte software buffer per UART");

    g_report_queue = xQueueCreate(REPORT_QUEUE_LENGTH, sizeof(frame_report_t));
    if (g_report_queue == NULL) { ESP_LOGE(TAG, "Report queue allocation failed"); return; }
    for (size_t i = 0; i < SENSOR_COUNT; i++) {
        g_sensors[i].done = xSemaphoreCreateBinary();
        __atomic_store_n(&g_sensors[i].run, true, __ATOMIC_RELEASE);
        if (g_sensors[i].done == NULL) { ESP_LOGE(TAG, "Semaphore allocation failed"); return; }
    }
    if (xTaskCreatePinnedToCore(logger_task, "h1_dual_log", 4096, NULL, 4,
                                NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "Logger task creation failed"); return;
    }
    for (size_t i = 0; i < SENSOR_COUNT; i++) {
        const char *task_name = i == 0 ? "h1_acq_A" : "h1_acq_B";
        if (xTaskCreatePinnedToCore(acquisition_task, task_name, 6144,
                                    &g_sensors[i], 8, &g_sensors[i].task,
                                    1) != pdPASS) {
            ESP_LOGE(TAG, "%s task creation failed", g_sensors[i].name); return;
        }
    }

    /*
     * The RX service intentionally occupies CPU1 whenever bytes are pending.
     * Monitoring IDLE1 would generate diagnostic dumps that are themselves
     * long enough to overflow the SC16 hardware FIFOs. Keep CPU0 idle-task
     * monitoring enabled so the watchdog still covers the rest of the app.
     */
    const esp_task_wdt_config_t streaming_wdt = {
        .timeout_ms = CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000U,
        .idle_core_mask = 1U << 0,
        .trigger_panic = false,
    };
    esp_err_t idle1_wdt = esp_task_wdt_reconfigure(&streaming_wdt);
    if (idle1_wdt == ESP_OK) {
        ESP_LOGI(TAG, "TWDT reconfigured: monitor IDLE0, exclude IDLE1");
    } else {
        ESP_LOGW(TAG, "Could not reconfigure TWDT for streaming: %s",
                 esp_err_to_name(idle1_wdt));
    }

    sc16_reset_rx_overrun_count();
    int64_t start_a_us = esp_timer_get_time();
    ESP_ERROR_CHECK(h1_start_stream(&g_sensors[0].device));
    int64_t start_b_us = esp_timer_get_time();
    ESP_ERROR_CHECK(h1_start_stream(&g_sensors[1].device));
    ESP_LOGI(TAG, "Streams commanded A=%lldus B=%lldus separation=%.3fms",
             (long long)start_a_us, (long long)start_b_us,
             (start_b_us - start_a_us) / 1000.0);
    xTaskNotifyGive(g_sensors[0].task);
    xTaskNotifyGive(g_sensors[1].task);

    printf("\n============================================================\n");
    printf(" BOTH CHANNELS ACQUIRING FOR 30 SECONDS\n");
    printf("============================================================\n");
    vTaskDelay(pdMS_TO_TICKS(TEST_DURATION_MS));

    ESP_LOGI(TAG, "Requesting both acquisition tasks to stop");
    for (size_t i = 0; i < SENSOR_COUNT; i++) {
        __atomic_store_n(&g_sensors[i].run, false, __ATOMIC_RELEASE);
    }
    for (size_t i = 0; i < SENSOR_COUNT; i++) {
        if (xSemaphoreTake(g_sensors[i].done, pdMS_TO_TICKS(6000)) != pdTRUE) {
            ESP_LOGE(TAG, "%s did not stop within 6 seconds", g_sensors[i].name);
            if (g_sensors[i].task != NULL) {
                vTaskDelete(g_sensors[i].task);
                g_sensors[i].task = NULL;
            }
        }
    }

    uint32_t acquisition_overruns[SENSOR_COUNT], shutdown_overruns[SENSOR_COUNT];
    uint32_t acquisition_drops[SENSOR_COUNT], shutdown_drops[SENSOR_COUNT];
    for (size_t i = 0; i < SENSOR_COUNT; i++) {
        acquisition_overruns[i] = sc16_get_channel_rx_overrun_count(g_sensors[i].channel);
        acquisition_drops[i] = sc16_get_software_rx_drop_count(g_sensors[i].channel);
    }
    for (size_t i = 0; i < SENSOR_COUNT; i++) {
        ESP_LOGI(TAG, "Stopping %s stream", g_sensors[i].name);
        esp_err_t ret = h1_stop_stream(&g_sensors[i].device);
        if (ret != ESP_OK) ESP_LOGE(TAG, "%s stop failed: %s", g_sensors[i].name,
                                    esp_err_to_name(ret));
    }
    for (size_t i = 0; i < SENSOR_COUNT; i++) {
        uint32_t total = sc16_get_channel_rx_overrun_count(g_sensors[i].channel);
        shutdown_overruns[i] = total - acquisition_overruns[i];
        total = sc16_get_software_rx_drop_count(g_sensors[i].channel);
        shutdown_drops[i] = total - acquisition_drops[i];
    }

    vTaskDelay(pdMS_TO_TICKS(200));
    printf("\n============================================================\n");
    printf(" DUAL UART STREAMING TEST RESULT\n");
    printf("============================================================\n");
    for (size_t i = 0; i < SENSOR_COUNT; i++) {
        print_summary(&g_sensors[i], acquisition_overruns[i], shutdown_overruns[i],
                      acquisition_drops[i], shutdown_drops[i]);
        printf("------------------------------------------------------------\n");
    }
    printf("Start-command separation    : %.3f ms\n", (start_b_us - start_a_us) / 1000.0);
    printf("Free heap after test        : %lu bytes\n", (unsigned long)esp_get_free_heap_size());
    printf("============================================================\n");
    while (true) vTaskDelay(pdMS_TO_TICKS(1000));
}
