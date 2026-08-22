#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "sc16is752.h"

#ifdef __cplusplus
extern "C" {
#endif


typedef enum {
    H1_EXPOSURE_MANUAL = 0,
    H1_EXPOSURE_AUTO   = 1
} h1_exposure_mode_t;


typedef enum {
    H1_EXPOSURE_STATUS_NORMAL = 0x00,
    H1_EXPOSURE_STATUS_OVER   = 0x01,
    H1_EXPOSURE_STATUS_UNDER  = 0x02
} h1_exposure_status_t;


/*
 * Current H1 documentation gives 711 samples for 340-1050 nm.
 * Give ourselves some spare capacity.
 */
#define H1_MAX_SPECTRUM_SAMPLES  1024


typedef struct {
    sc16_channel_t channel;

    char device_info[25];

    bool initialized;
} h1_device_t;


typedef struct {
    h1_exposure_status_t exposure_status;

    uint32_t exposure_us;

    int16_t spectrum_scale;

    size_t sample_count;

    /*
     * Raw uint16 values returned by H1.
     *
     * Physical value:
     *
     * actual = raw / 10^(spectrum_scale)
     */
    uint16_t spectrum[H1_MAX_SPECTRUM_SAMPLES];

} h1_spectrum_frame_t;


esp_err_t h1_init(
    h1_device_t *dev,
    sc16_channel_t channel
);


esp_err_t h1_get_device_info(
    h1_device_t *dev
);


esp_err_t h1_set_exposure_mode(
    h1_device_t *dev,
    h1_exposure_mode_t mode
);


esp_err_t h1_get_exposure_mode(
    h1_device_t *dev,
    h1_exposure_mode_t *mode
);


/**
 * Acquire one spectrum using H1 command 0x32.
 *
 * This is a blocking request-response operation.
 */
esp_err_t h1_get_single_spectrum(
    h1_device_t *dev,
    h1_spectrum_frame_t *frame
);


#ifdef __cplusplus
}
#endif