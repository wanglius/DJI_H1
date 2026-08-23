#include <stdio.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"

#include "sc16is752.h"
#include "h1.h"

#include "esp_timer.h"


static const char *TAG = "DJI_H1";


static const char *exposure_status_string(
    h1_exposure_status_t status)
{
    switch (status) {

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


void app_main(void)
{
    printf("\n");
    printf("========================================\n");
    printf(" DJI_H1 - SINGLE SPECTRUM TEST\n");
    printf("========================================\n");


    // --------------------------------------------------------
    // SC16 initialization
    // --------------------------------------------------------

    sc16_config_t sc16_config = {

        .spi_host = SPI2_HOST,

        .pin_mosi = 2,
        .pin_miso = 3,
        .pin_sclk = 5,
        .pin_cs = 1,
        .pin_reset = 6,

        .spi_clock_hz = 1000000,
        .crystal_hz = 1843200
    };


    ESP_ERROR_CHECK(
        sc16_init(&sc16_config)
    );


    ESP_ERROR_CHECK(
        sc16_test_channel(
            SC16_CHANNEL_A)
    );


    ESP_ERROR_CHECK(
        sc16_uart_init(
            SC16_CHANNEL_A)
    );


    // --------------------------------------------------------
    // H1 initialization
    // --------------------------------------------------------

    h1_device_t h1;


    ESP_ERROR_CHECK(
        h1_init(
            &h1,
            SC16_CHANNEL_A
        )
    );


    // --------------------------------------------------------
    // Verify identity
    // --------------------------------------------------------

    esp_err_t ret =
        h1_get_device_info(&h1);

    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Failed to get H1 device info: %s",
            esp_err_to_name(ret)
        );

        /*
        * Stay alive for debugging instead of abort/reboot.
        */
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }


    printf(
        "H1 device: %s\n",
        h1.device_info
    );


    // --------------------------------------------------------
    // Auto exposure
    // --------------------------------------------------------

    ESP_ERROR_CHECK(
        h1_set_exposure_mode(
            &h1,
            H1_EXPOSURE_AUTO
        )
    );


    h1_exposure_mode_t mode;


    ESP_ERROR_CHECK(
        h1_get_exposure_mode(
            &h1,
            &mode
        )
    );


    printf(
        "Exposure mode: %s\n",
        mode == H1_EXPOSURE_AUTO
            ? "AUTO"
            : "MANUAL"
    );


// --------------------------------------------------------
// Continuous streaming test
// --------------------------------------------------------

ESP_LOGI(
    TAG,
    "Starting continuous H1 stream..."
);


sc16_reset_rx_overrun_count();


ret =
    h1_start_stream(&h1);


if (ret != ESP_OK) {

    ESP_LOGE(
        TAG,
        "Start stream failed: %s",
        esp_err_to_name(ret)
    );

    return;
}


static h1_spectrum_frame_t frame;

const uint32_t TEST_FRAMES = 10;


printf("\n");
printf("========================================\n");
printf(" H1 CONTINUOUS STREAM TEST\n");
printf("========================================\n");


for (uint32_t n = 0;
     n < TEST_FRAMES;
     n++) {

    int64_t t0 =
        esp_timer_get_time();


    ret =
        h1_read_stream_frame(
            &h1,
            &frame,
            5000
        );


    int64_t t1 =
        esp_timer_get_time();


    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Frame %lu failed: %s",
            (unsigned long)(n + 1),
            esp_err_to_name(ret)
        );

        break;
    }


    // Find spectral maximum
    uint16_t max_value = 0;
    size_t max_index = 0;


    for (size_t i = 0;
         i < frame.sample_count;
         i++) {

        if (frame.spectrum[i] >
            max_value) {

            max_value =
                frame.spectrum[i];

            max_index =
                i;
        }
    }


    printf(
        "Frame %2lu | "
        "exp=%8lu us | "
        "status=%u | "
        "scale=%d | "
        "N=%u | "
        "max=%u @ %u nm | "
        "interval=%.1f ms\n",

        (unsigned long)(n + 1),

        (unsigned long)
        frame.exposure_us,

        (unsigned)
        frame.exposure_status,

        (int)
        frame.spectrum_scale,

        (unsigned)
        frame.sample_count,

        (unsigned)
        max_value,

        340 + (unsigned)max_index,

        (t1 - t0) / 1000.0
    );
}


// --------------------------------------------------------
// Stop stream
// --------------------------------------------------------

ret =
    h1_stop_stream(&h1);


if (ret != ESP_OK) {

    ESP_LOGE(
        TAG,
        "Stop stream failed: %s",
        esp_err_to_name(ret)
    );
}


printf("\n");

printf(
    "Frames received  : %lu\n",
    (unsigned long)
    h1.stream_frame_count
);

printf(
    "RX overrun count : %lu\n",
    (unsigned long)
    sc16_get_rx_overrun_count()
);

printf("========================================\n");
printf(" STREAM TEST COMPLETE\n");
printf("========================================\n");


    while (1) {

        vTaskDelay(
            pdMS_TO_TICKS(1000)
        );
    }
}