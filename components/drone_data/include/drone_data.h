#pragma once

#include <stdint.h>

#include "data_records.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Fixed simulation sample used until the A-board UART source is available. */
extern const drone_realtime_data_t DRONE_DATA_FAKE_FIXED;

/** Initialize the latest-sample store with DRONE_DATA_FAKE_FIXED. */
esp_err_t drone_data_init_fake(void);

/**
 * Decode one documented 30-byte CMD 0x01 payload and publish it as latest.
 * Multi-byte fields are decoded explicitly as little-endian values.
 * Caller must validate the received length is AB_REALTIME_DATA_SIZE first:
 * a C array parameter decays to a pointer and cannot enforce buffer length.
 */
esp_err_t drone_data_update_payload(
    const uint8_t payload[AB_REALTIME_DATA_SIZE],
    uint8_t protocol_sequence,
    int64_t b_receive_timestamp_us);

/** Copy the most recently published real or simulated GPS record. */
esp_err_t drone_data_get_latest(gps_record_t *out_record);

#ifdef __cplusplus
}
#endif
