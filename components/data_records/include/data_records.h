#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "h1.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DATA_RECORD_FORMAT_VERSION 1U
#define DRONE_REALTIME_PAYLOAD_SIZE 30U

typedef enum {
    DATA_RECORD_GPS = 1,
    DATA_RECORD_RAW_SPECTRAL_PAIR = 2,
    DATA_RECORD_REFLECTANCE = 3,
} data_record_type_t;

typedef struct {
    uint16_t format_version;
    uint16_t record_type;
    uint32_t record_size;
    uint32_t sequence;
    uint32_t session_id;
    int64_t b_timestamp_us;
} data_record_header_t;

typedef struct {
    int32_t latitude_e7;
    int32_t longitude_e7;
    int32_t altitude_relative_mm;
    uint32_t utc_seconds;
    uint32_t a_monotonic_ms;
    uint16_t utc_milliseconds;
    uint8_t source_flags;
    uint8_t gps_fix;
    uint8_t rtk_solution;
    uint8_t flight_status;
    uint8_t display_mode;
    uint8_t battery_percent;
    uint8_t a_status;
    uint8_t valid_flags;
} drone_realtime_data_t;

enum {
    DRONE_SOURCE_POSITION_RTK = 1U << 0,
    DRONE_SOURCE_ALTITUDE_RTK = 1U << 1,
};

enum {
    DRONE_VALID_POSITION = 1U << 0,
    DRONE_VALID_ALTITUDE = 1U << 1,
    DRONE_VALID_UTC = 1U << 2,
    DRONE_VALID_BATTERY = 1U << 3,
};

typedef struct {
    data_record_header_t header;
    uint8_t protocol_sequence;
    uint8_t reserved[3];
    drone_realtime_data_t data;
} gps_record_t;

typedef struct {
    data_record_header_t header;
    uint32_t pair_count;
    uint32_t frame_a_count;
    uint32_t frame_b_count;
    int64_t frame_a_timestamp_us;
    int64_t frame_b_timestamp_us;
    h1_spectrum_frame_t frame_a;
    h1_spectrum_frame_t frame_b;
} raw_spectral_record_t;

typedef struct {
    data_record_header_t header;
    uint32_t calculation_count;
    uint32_t source_pair_count;
    uint16_t sample_count;
    uint16_t reserved;
    float reflectance[H1_MAX_SPECTRUM_SAMPLES];
} reflectance_record_t;

/*
 * These are in-memory queue records. Do not fwrite() the structures as a
 * persistent wire format: compiler padding and h1_spectrum_frame_t layout are
 * not version-stable. The logging component must serialize fields explicitly.
 */

/** Initialize the common header of any record type. */
void data_record_header_init(data_record_header_t *header,
                             data_record_type_t type,
                             uint32_t record_size,
                             uint32_t sequence,
                             uint32_t session_id,
                             int64_t b_timestamp_us);

#ifdef __cplusplus
}
#endif
