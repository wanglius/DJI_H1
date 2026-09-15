#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "sc16is752.h"

#define H1_RX_PACKET_BUFFER_SIZE 2048


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


/**
 * Streaming state of H1 device, with a streaming frame count.
 */
typedef struct
{
    sc16_channel_t channel;

    char device_info[25];

    bool initialized;
    bool streaming;

    uint32_t stream_frame_count;

    /*
     * Private receive buffer for this H1 instance.
     *
     * Ground H1 and sky H1 will each have their own copy.
     */
    uint8_t rx_packet[H1_RX_PACKET_BUFFER_SIZE];

} h1_device_t;


/*
 * Current H1 documentation gives 711 samples for 340-1050 nm.
 * Give ourselves some spare capacity.
 */
#define H1_MAX_SPECTRUM_SAMPLES  1024


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

/** Cooperative cancellation predicate for a blocking stream-frame receive.
 * It is called from the reader task and must be nonblocking. */
typedef bool (*h1_cancel_requested_fn)(void *context);


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

/**
 * stream controls
 */
esp_err_t h1_start_stream(
    h1_device_t *dev
);

esp_err_t h1_read_stream_frame(
    h1_device_t *dev,
    h1_spectrum_frame_t *frame,
    uint32_t timeout_ms
);

/** Read one stream frame while allowing the lifecycle owner to cancel the
 * in-progress receive. Cancellation preserves the partial packet only until
 * this call returns and reports ESP_ERR_INVALID_STATE; the owner must then
 * stop the stream through the normal ordered cleanup path. */
esp_err_t h1_read_stream_frame_interruptible(
    h1_device_t *dev,
    h1_spectrum_frame_t *frame,
    uint32_t timeout_ms,
    h1_cancel_requested_fn cancel_requested,
    void *cancel_context
);

esp_err_t h1_stop_stream(
    h1_device_t *dev
);

#ifdef __cplusplus
}
#endif
