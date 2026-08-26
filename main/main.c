#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"

#include "sc16is752.h"
#include "h1.h"


// ============================================================
// General configuration
// ============================================================

static const char *TAG = "DJI_H1";

/*
 * Duration of the RTOS qualification test.
 *
 * We previously proved 10 synchronous frames.
 * Now we want the H1 acquisition to run concurrently
 * with another FreeRTOS task for a longer period.
 */
#define RTOS_TEST_DURATION_MS       30000

/*
 * h1_read_stream_frame() timeout.
 *
 * Keep this the same as the proven streaming test.
 */
#define H1_FRAME_TIMEOUT_MS          5000

/*
 * Queue depth for lightweight frame reports.
 *
 * The acquisition task NEVER waits for the logger.
 * If the logger somehow falls behind, acquisition
 * continues and the report is simply dropped.
 */
#define FRAME_REPORT_QUEUE_LENGTH      16


// ============================================================
// H1 object
// ============================================================

/*
 * Keep the H1 device at file scope.
 *
 * This avoids lifetime/pointer confusion when a FreeRTOS task
 * accesses the device after app_main() has started the task.
 *
 * In the later dual-H1 version this will naturally become:
 *
 *     g_h1_ground
 *     g_h1_sky
 */
static h1_device_t g_h1;


// ============================================================
// Spectrum frame storage
// ============================================================

/*
 * h1_spectrum_frame_t contains the complete spectrum array,
 * so it is relatively large.
 *
 * Keep it out of the acquisition task's stack.
 *
 * For this ONE-H1 test, one global frame object is sufficient.
 *
 * IMPORTANT:
 * We will NOT share this object between two H1 tasks later.
 */
static h1_spectrum_frame_t g_acquisition_frame;


// ============================================================
// Acquisition task context
// ============================================================

typedef struct
{
    /*
     * H1 device controlled by this task.
     */
    h1_device_t *h1;

    /*
     * Human-readable task/sensor name.
     */
    const char *name;

    /*
     * app_main sets this false when the test should end.
     */
    volatile bool run;

    /*
     * Diagnostic counters.
     */
    uint32_t frames_ok;
    uint32_t frames_error;

    /*
     * Number of frame reports dropped because the
     * logger queue was full.
     *
     * This does NOT mean the spectrum itself was lost.
     * It only means the console report was skipped.
     */
    uint32_t reports_dropped;

} h1_acquisition_context_t;


/*
 * File-scope context so its lifetime is guaranteed
 * for the acquisition task.
 */
static h1_acquisition_context_t g_acq_ctx;


// ============================================================
// Lightweight information sent to logger task
// ============================================================

typedef struct
{
    /*
     * Sequential successfully received frame number.
     */
    uint32_t frame_number;

    /*
     * ESP32 monotonic timestamp immediately after the
     * full H1 packet has been received and decoded.
     */
    int64_t timestamp_us;

    /*
     * H1 metadata.
     */
    uint32_t exposure_us;
    uint8_t exposure_status;
    int16_t spectrum_scale;

    /*
     * Spectrum summary used only for this qualification test.
     */
    uint16_t max_value;
    uint16_t max_wavelength_nm;

    /*
     * Time between completion of this frame
     * and completion of the previous frame.
     *
     * Zero for the first frame.
     */
    uint32_t interval_us;

} h1_frame_report_t;


// ============================================================
// FreeRTOS objects
// ============================================================

/*
 * Acquisition -> logger queue.
 */
static QueueHandle_t g_report_queue = NULL;

/*
 * Used by acquisition task to tell app_main:
 *
 * "I have exited h1_read_stream_frame() and will no longer
 * touch the H1."
 *
 * app_main waits for this before issuing STOP.
 */
static SemaphoreHandle_t g_acquisition_done = NULL;


// ============================================================
// Helper: exposure status string
// ============================================================

static const char *exposure_status_string(
    h1_exposure_status_t status)
{
    switch (status)
    {
        case H1_EXPOSURE_STATUS_NORMAL:
            return "NORMAL";

        case H1_EXPOSURE_STATUS_OVER:
            return "OVEREXPOSED";

        case H1_EXPOSURE_STATUS_UNDER:
            return "UNDEREXPOSED";

        default:
            return "UNKNOWN";
    }
}


// ============================================================
// H1 acquisition task
// ============================================================

static void h1_acquisition_task(void *arg)
{
    h1_acquisition_context_t *ctx =
        (h1_acquisition_context_t *)arg;

    /*
     * Completion timestamp of previous successful frame.
     */
    int64_t previous_frame_us = 0;


    ESP_LOGI(
        TAG,
        "%s acquisition task started",
        ctx->name
    );


    // --------------------------------------------------------
    // Main acquisition loop
    // --------------------------------------------------------

    while (ctx->run)
    {
        /*
         * This is the SAME proven H1 streaming API used by
         * the synchronous 10-frame test.
         *
         * h1_receive_packet() internally uses taskYIELD()
         * while no UART data are available.
         */
        esp_err_t ret =
            h1_read_stream_frame(
                ctx->h1,
                &g_acquisition_frame,
                H1_FRAME_TIMEOUT_MS
            );


        /*
         * Timestamp immediately after the complete H1 frame
         * has returned from the driver.
         *
         * Later this timestamp will be useful for pairing
         * ground and sky measurements.
         */
        int64_t now_us =
            esp_timer_get_time();


        // ----------------------------------------------------
        // Receive/decode failure
        // ----------------------------------------------------

        if (ret != ESP_OK)
        {
            ctx->frames_error++;

            ESP_LOGE(
                TAG,
                "%s frame error: %s",
                ctx->name,
                esp_err_to_name(ret)
            );

            /*
             * Don't introduce a millisecond-scale delay here,
             * because we already learned that sleeping can be
             * dangerous for the 64-byte SC16 RX FIFO.
             *
             * Just offer the scheduler another opportunity.
             */
            taskYIELD();

            continue;
        }


        // ----------------------------------------------------
        // Successful frame
        // ----------------------------------------------------

        ctx->frames_ok++;


        // ----------------------------------------------------
        // Find maximum raw spectral sample
        //
        // This is only for easy visual confirmation.
        // It is NOT part of the eventual scientific pipeline.
        // ----------------------------------------------------

        uint16_t max_value = 0;
        size_t max_index = 0;


        for (size_t i = 0;
             i < g_acquisition_frame.sample_count;
             i++)
        {
            if (g_acquisition_frame.spectrum[i] >
                max_value)
            {
                max_value =
                    g_acquisition_frame.spectrum[i];

                max_index = i;
            }
        }


        // ----------------------------------------------------
        // Build a SMALL report for the logger.
        //
        // We deliberately do NOT send 711 spectral samples
        // through this queue.
        //
        // The acquisition task should remain fast.
        // ----------------------------------------------------

        h1_frame_report_t report =
        {
            .frame_number =
                ctx->frames_ok,

            .timestamp_us =
                now_us,

            .exposure_us =
                g_acquisition_frame.exposure_us,

            .exposure_status =
                (uint8_t)
                g_acquisition_frame.exposure_status,

            .spectrum_scale =
                g_acquisition_frame.spectrum_scale,

            .max_value =
                max_value,

            /*
             * H1 currently gives 340...1050 nm,
             * one sample per nm.
             */
            .max_wavelength_nm =
                (uint16_t)(340 + max_index),

            .interval_us =
                (previous_frame_us == 0)
                ? 0
                : (uint32_t)
                  (now_us - previous_frame_us)
        };


        /*
         * Save timestamp for next interval measurement.
         */
        previous_frame_us =
            now_us;


        // ----------------------------------------------------
        // Send report WITHOUT BLOCKING.
        //
        // Timeout = 0 is deliberate.
        //
        // Acquisition must never wait for printf().
        // ----------------------------------------------------

        if (xQueueSend(
                g_report_queue,
                &report,
                0) != pdTRUE)
        {
            ctx->reports_dropped++;
        }
    }


    // --------------------------------------------------------
    // We have left the acquisition loop.
    //
    // From this point onward this task will no longer call
    // h1_read_stream_frame().
    // --------------------------------------------------------

    ESP_LOGI(
        TAG,
        "%s acquisition task stopped",
        ctx->name
    );


    /*
     * Signal app_main that it is now safe to send the H1
     * STOP command.
     */
    xSemaphoreGive(
        g_acquisition_done
    );


    /*
     * Delete this task.
     */
    vTaskDelete(NULL);
}


// ============================================================
// Logger task
// ============================================================

static void h1_logger_task(void *arg)
{
    (void)arg;

    h1_frame_report_t report;


    ESP_LOGI(
        TAG,
        "Logger task started"
    );


    while (1)
    {
        /*
         * This task can block indefinitely because it has
         * nothing time-critical to do.
         *
         * It wakes only when the acquisition task puts
         * a report into the queue.
         */
        if (xQueueReceive(
                g_report_queue,
                &report,
                portMAX_DELAY) == pdTRUE)
        {
            if (report.interval_us == 0)
            {
                printf(
                    "Frame %5lu | "
                    "exp=%8lu us | "
                    "status=%u | "
                    "scale=%d | "
                    "max=%5u @ %4u nm | "
                    "first frame\n",

                    (unsigned long)
                    report.frame_number,

                    (unsigned long)
                    report.exposure_us,

                    (unsigned)
                    report.exposure_status,

                    (int)
                    report.spectrum_scale,

                    (unsigned)
                    report.max_value,

                    (unsigned)
                    report.max_wavelength_nm
                );
            }
            else
            {
                printf(
                    "Frame %5lu | "
                    "exp=%8lu us | "
                    "status=%u | "
                    "scale=%d | "
                    "max=%5u @ %4u nm | "
                    "dt=%7.1f ms\n",

                    (unsigned long)
                    report.frame_number,

                    (unsigned long)
                    report.exposure_us,

                    (unsigned)
                    report.exposure_status,

                    (int)
                    report.spectrum_scale,

                    (unsigned)
                    report.max_value,

                    (unsigned)
                    report.max_wavelength_nm,

                    report.interval_us / 1000.0
                );
            }
        }
    }
}


// ============================================================
// app_main
// ============================================================

void app_main(void)
{
    printf("\n");
    printf("========================================\n");
    printf(" DJI_H1 - RTOS STREAMING TEST\n");
    printf("========================================\n");


    // ========================================================
    // 1. SC16IS752 INITIALIZATION
    // ========================================================

    sc16_config_t sc16_config =
    {
        .spi_host = SPI2_HOST,

        .pin_mosi = 2,
        .pin_miso = 3,
        .pin_sclk = 5,
        .pin_cs = 1,
        .pin_reset = 6,

        /*
         * Keep exactly the proven SPI setting.
         */
        .spi_clock_hz = 1000000,

        /*
         * SC16 board crystal frequency.
         */
        .crystal_hz = 1843200
    };


    ESP_ERROR_CHECK(
        sc16_init(
            &sc16_config
        )
    );


    /*
     * Verify UART-A register access exactly as before.
     */
    ESP_ERROR_CHECK(
        sc16_test_channel(
            SC16_CHANNEL_A
        )
    );


    /*
     * Configure UART-A for the production H1 setting:
     *
     * 115200 baud
     * 8 data bits
     * no parity
     * 1 stop bit
     */
    ESP_ERROR_CHECK(
        sc16_uart_init(
            SC16_CHANNEL_A
        )
    );


    // ========================================================
    // 2. H1 INITIALIZATION
    // ========================================================

    ESP_ERROR_CHECK(
        h1_init(
            &g_h1,
            SC16_CHANNEL_A
        )
    );


    // ========================================================
    // 3. READ / VERIFY H1 IDENTITY
    // ========================================================

    esp_err_t ret =
        h1_get_device_info(
            &g_h1
        );


    if (ret != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to get H1 device info: %s",
            esp_err_to_name(ret)
        );

        /*
         * Stay alive instead of ESP_ERROR_CHECK -> abort/reboot.
         *
         * Communication failures are useful diagnostics,
         * not firmware-fatal conditions.
         */
        while (1)
        {
            vTaskDelay(
                pdMS_TO_TICKS(1000)
            );
        }
    }


    printf(
        "H1 device: %s\n",
        g_h1.device_info
    );


    // ========================================================
    // 4. ENABLE AUTO EXPOSURE
    // ========================================================

    ret =
        h1_set_exposure_mode(
            &g_h1,
            H1_EXPOSURE_AUTO
        );


    if (ret != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to set AUTO exposure: %s",
            esp_err_to_name(ret)
        );

        return;
    }


    // ========================================================
    // 5. VERIFY AUTO EXPOSURE MODE
    // ========================================================

    h1_exposure_mode_t mode;


    ret =
        h1_get_exposure_mode(
            &g_h1,
            &mode
        );


    if (ret != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to read exposure mode: %s",
            esp_err_to_name(ret)
        );

        return;
    }


    printf(
        "Exposure mode: %s\n",
        mode == H1_EXPOSURE_AUTO
            ? "AUTO"
            : "MANUAL"
    );


    // ========================================================
    // 6. CREATE THE RTOS REPORT QUEUE
    // ========================================================

    g_report_queue =
        xQueueCreate(
            FRAME_REPORT_QUEUE_LENGTH,
            sizeof(h1_frame_report_t)
        );


    if (g_report_queue == NULL)
    {
        ESP_LOGE(
            TAG,
            "Failed to create frame report queue"
        );

        return;
    }


    // ========================================================
    // 7. CREATE ACQUISITION-DONE SEMAPHORE
    // ========================================================

    g_acquisition_done =
        xSemaphoreCreateBinary();


    if (g_acquisition_done == NULL)
    {
        ESP_LOGE(
            TAG,
            "Failed to create acquisition semaphore"
        );

        return;
    }


    // ========================================================
    // 8. INITIALIZE ACQUISITION CONTEXT
    // ========================================================

    g_acq_ctx.h1 =
        &g_h1;

    g_acq_ctx.name =
        "H1-A";

    g_acq_ctx.run =
        true;

    g_acq_ctx.frames_ok =
        0;

    g_acq_ctx.frames_error =
        0;

    g_acq_ctx.reports_dropped =
        0;


    // ========================================================
    // 9. RESET SC16 RX DIAGNOSTIC COUNTER
    // ========================================================

    sc16_reset_rx_overrun_count();


    // ========================================================
    // 10. START H1 CONTINUOUS STREAM
    //
    // This is the same proven 0x33 operation.
    // ========================================================

    ESP_LOGI(
        TAG,
        "Starting continuous H1 stream..."
    );


    ret =
        h1_start_stream(
            &g_h1
        );


    if (ret != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to start stream: %s",
            esp_err_to_name(ret)
        );

        return;
    }


    // ========================================================
    // 11. START LOGGER TASK FIRST
    //
    // Lower priority than acquisition.
    // ========================================================

    TaskHandle_t logger_task_handle = NULL;


    BaseType_t task_result =
        xTaskCreate(
            h1_logger_task,           // task function
            "h1_logger",              // task name
            4096,                     // stack size
            NULL,                     // argument
            4,                        // priority
            &logger_task_handle       // task handle
        );


    if (task_result != pdPASS)
    {
        ESP_LOGE(
            TAG,
            "Failed to create logger task"
        );

        /*
         * H1 is already streaming, so stop it
         * before leaving.
         */
        h1_stop_stream(
            &g_h1
        );

        return;
    }


    // ========================================================
    // 12. START ACQUISITION TASK
    //
    // Higher priority than logger.
    // ========================================================

    task_result =
        xTaskCreate(
            h1_acquisition_task,      // task function
            "h1_acq_A",               // task name
            6144,                     // stack size
            &g_acq_ctx,               // argument
            8,                        // priority
            NULL                      // no handle needed
        );


    if (task_result != pdPASS)
    {
        ESP_LOGE(
            TAG,
            "Failed to create acquisition task"
        );

        h1_stop_stream(
            &g_h1
        );

        return;
    }


    printf("\n");
    printf("========================================\n");
    printf(" H1 RTOS STREAMING TEST\n");
    printf("========================================\n");


    ESP_LOGI(
        TAG,
        "RTOS streaming test running for %u seconds",
        RTOS_TEST_DURATION_MS / 1000
    );


    // ========================================================
    // 13. LET BOTH TASKS RUN CONCURRENTLY
    // ========================================================

    vTaskDelay(
        pdMS_TO_TICKS(
            RTOS_TEST_DURATION_MS
        )
    );


    // ========================================================
    // 14. ASK ACQUISITION TASK TO EXIT
    //
    // IMPORTANT:
    // Do NOT send H1 STOP yet.
    //
    // The acquisition task may currently be receiving a frame.
    // ========================================================

    ESP_LOGI(
        TAG,
        "Requesting acquisition task shutdown..."
    );


    g_acq_ctx.run =
        false;


    // ========================================================
    // 15. WAIT UNTIL ACQUISITION TASK IS DEFINITELY FINISHED
    //
    // h1_read_stream_frame() uses a 5-second timeout,
    // so allow slightly more than that.
    // ========================================================

    if (xSemaphoreTake(
            g_acquisition_done,
            pdMS_TO_TICKS(6000)
        ) != pdTRUE)
    {
        ESP_LOGE(
            TAG,
            "Acquisition task did not stop within 6 seconds"
        );

        /*
         * Don't blindly interfere with the H1 receive operation.
         * Stay here for diagnosis.
         */
        while (1)
        {
            vTaskDelay(
                pdMS_TO_TICKS(1000)
            );
        }
    }


    /*
     * Snapshot the diagnostic before stopping the H1. At short
     * exposure times the H1 can transmit frames back-to-back.
     * Once the acquisition task exits, bytes received while the
     * STOP command settles are deliberately discarded and may
     * overflow the 64-byte SC16 FIFO. Those shutdown overruns do
     * not describe the integrity of the frames qualified above.
     */
    uint32_t acquisition_rx_overruns =
        sc16_get_rx_overrun_count();


    // ========================================================
    // 16. NOW STOP THE H1 STREAM
    //
    // No acquisition task is touching the UART any more.
    // ========================================================

    ret =
        h1_stop_stream(
            &g_h1
        );


    if (ret != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to stop stream: %s",
            esp_err_to_name(ret)
        );
    }


    uint32_t total_rx_overruns =
        sc16_get_rx_overrun_count();


    uint32_t shutdown_rx_overruns =
        total_rx_overruns - acquisition_rx_overruns;


    /*
     * Give logger a short opportunity to print the final
     * queued frame report before we print the summary.
     *
     * This delay occurs AFTER H1 streaming has stopped,
     * so it cannot hurt the SC16 RX FIFO.
     */
    vTaskDelay(
        pdMS_TO_TICKS(100)
    );


    // ========================================================
    // 17. PRINT FINAL QUALIFICATION RESULT
    // ========================================================

    printf("\n");
    printf("========================================\n");
    printf(" RTOS STREAMING TEST RESULT\n");
    printf("========================================\n");


    printf(
        "Frames OK        : %lu\n",
        (unsigned long)
        g_acq_ctx.frames_ok
    );


    printf(
        "Frame errors     : %lu\n",
        (unsigned long)
        g_acq_ctx.frames_error
    );


    printf(
        "Reports dropped  : %lu\n",
        (unsigned long)
        g_acq_ctx.reports_dropped
    );


    printf(
        "Acquisition RX overruns : %lu\n",
        (unsigned long)
        acquisition_rx_overruns
    );


    printf(
        "Shutdown RX overruns    : %lu\n",
        (unsigned long)
        shutdown_rx_overruns
    );


    printf("========================================\n");


    // ========================================================
    // 18. TEST COMPLETE
    //
    // Leave ESP32 alive so the serial monitor remains readable.
    // ========================================================

    while (1)
    {
        vTaskDelay(
            pdMS_TO_TICKS(1000)
        );
    }
}
