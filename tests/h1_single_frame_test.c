#include <stdio.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"

#include "sc16is752.h"
#include "h1.h"


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
    // Acquire one spectrum
    // --------------------------------------------------------

    ESP_LOGI(
        TAG,
        "Requesting one spectrum..."
    );


    /*
     * Static so the ~2 kB frame structure does not consume
     * app_main task stack space.
     */

    static h1_spectrum_frame_t frame;

    /**
     * reset the fifo overrun counter before acquiring a spectrum
     */
    sc16_reset_rx_overrun_count();


    ret =
        h1_get_single_spectrum(
            &h1,
            &frame
        );

    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Spectrum acquisition failed: %s",
            esp_err_to_name(ret)
        );

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }


    // --------------------------------------------------------
    // Display result
    // --------------------------------------------------------

    printf("\n");
    printf("========================================\n");
    printf(" H1 SINGLE SPECTRUM\n");
    printf("========================================\n");

    printf(
        "Exposure status : %s\n",
        exposure_status_string(
            frame.exposure_status
        )
    );

    printf(
        "Exposure time   : %lu us\n",
        (unsigned long)frame.exposure_us
    );

    printf(
        "Spectrum scale  : %d\n",
        (int)frame.spectrum_scale
    );

    printf(
        "Sample count    : %u\n",
        (unsigned)frame.sample_count
    );


    // --------------------------------------------------------
    // First five samples
    // --------------------------------------------------------

    printf("\nFirst samples:\n");


    size_t first_count =
        frame.sample_count < 5
        ? frame.sample_count
        : 5;


    for (size_t i = 0;
         i < first_count;
         i++) {

        /*
         * For this current H1 configuration we expect
         * 340 nm as the first sample.
         */

        unsigned wavelength =
            340 + (unsigned)i;

        printf(
            "  %u nm : raw=%u\n",
            wavelength,
            (unsigned)frame.spectrum[i]
        );
    }


    // --------------------------------------------------------
    // Last five samples
    // --------------------------------------------------------

    if (frame.sample_count > 5) {

        printf("\nLast samples:\n");


        size_t start =
            frame.sample_count - 5;


        for (size_t i = start;
             i < frame.sample_count;
             i++) {

            unsigned wavelength =
                340 + (unsigned)i;

            printf(
                "  %u nm : raw=%u\n",
                wavelength,
                (unsigned)frame.spectrum[i]
            );
        }
    }


    // --------------------------------------------------------
    // Find maximum raw value
    // --------------------------------------------------------

    uint16_t max_value = 0;
    size_t max_index = 0;


    for (size_t i = 0;
         i < frame.sample_count;
         i++) {

        if (frame.spectrum[i] > max_value) {

            max_value =
                frame.spectrum[i];

            max_index =
                i;
        }
    }


    printf(
        "\nMaximum raw value: %u at ~%u nm\n",
        (unsigned)max_value,
        340 + (unsigned)max_index
    );


    printf("\n");
    printf("========================================\n");
    printf(" SINGLE SPECTRUM TEST PASSED\n");
    printf("========================================\n");


    while (1) {

        vTaskDelay(
            pdMS_TO_TICKS(1000)
        );
    }
}