#pragma once

#include <stdint.h>

#include "ab_protocol.h"
#include "clock_sync.h"
#include "h1.h"

#ifdef __cplusplus
extern "C" {
#endif

/* First production on-card/MQTT record schema. Bump this value whenever a
 * serialized field is added, removed, resized, or reinterpreted. */
#define DATA_RECORD_FORMAT_VERSION 0x01U

/* Little-endian byte order produces the readable signatures "DHF1" and
 * "DHR1". Magic values aid framing; CRCs provide corruption detection. */
#define DATA_FILE_MAGIC   UINT32_C(0x31464844)
#define DATA_RECORD_MAGIC UINT32_C(0x31524844)
#define DATA_RECORD_WIRE_HEADER_SIZE 60U
#define RAW_RECORD_WIRE_SIZE(sample_count) \
    (DATA_RECORD_WIRE_HEADER_SIZE + 16U + (uint32_t)(sample_count) * 2U + 4U)
#define REFLECTANCE_RECORD_WIRE_SIZE(sample_count) \
    (DATA_RECORD_WIRE_HEADER_SIZE + 36U + (uint32_t)(sample_count) * 3U + 4U)
#define GPS_RECORD_WIRE_SIZE \
    (DATA_RECORD_WIRE_HEADER_SIZE + 34U + 4U)

typedef enum {
    DATA_RECORD_GPS = 1,
    DATA_RECORD_RAW_SPECTRUM = 2,
    DATA_RECORD_REFLECTANCE = 3,
    DATA_RECORD_OPERATION_LOG = 4,
} data_record_type_t;

typedef enum {
    SPECTROMETER_GROUND = 0,
    SPECTROMETER_SKY = 1,
} spectrometer_role_t;

enum {
    DATA_RECORD_FLAG_NONE = 0,
};

/** Common in-memory header for every mission record.
 *
 * Persistent files and MQTT payloads must serialize every field explicitly in
 * little-endian order. Never write this compiler representation directly. */
typedef struct {
    uint32_t magic;
    uint16_t format_version;
    uint16_t record_type;
    uint32_t header_size;
    uint32_t record_size;
    uint32_t session_id;
    uint16_t segment_id;
    uint16_t flags;
    uint32_t record_sequence;
    record_time_t timestamp;
} measurement_record_header_t;

/* Keep the established name while all producers migrate to the v01 header. */
typedef measurement_record_header_t data_record_header_t;

/* The protocol component remains the only owner of the 30-byte field map. */
typedef ab_realtime_data_t drone_realtime_data_t;

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

enum {
    RAW_QUALITY_VALID = 1U << 0,
    RAW_QUALITY_SATURATED = 1U << 1,
    RAW_QUALITY_UNDEREXPOSED = 1U << 2,
    RAW_QUALITY_AFTER_RECOVERY = 1U << 3,
    RAW_QUALITY_COUNT_MISMATCH = 1U << 4,
};

/** One independently acquired ground or sky frame.
 * Only sample_count samples are serialized to the on-card record. */
typedef struct {
    measurement_record_header_t header;
    uint32_t frame_count;
    uint32_t exposure_us;
    uint16_t sample_count;
    int16_t spectrum_scale;
    uint8_t spectrometer_role;
    uint8_t exposure_status;
    uint8_t frame_quality;
    uint8_t reserved;
    uint16_t samples[H1_MAX_SPECTRUM_SAMPLES];
} raw_spectrum_record_t;

enum {
    REFLECTANCE_INPUT_GROUND_VALID = 1U << 0,
    REFLECTANCE_INPUT_SKY_VALID = 1U << 1,
    REFLECTANCE_INPUT_SKY_FRESH = 1U << 2,
    REFLECTANCE_INPUT_SAMPLE_COUNTS_MATCH = 1U << 3,
};

enum {
    REFLECTANCE_SAMPLE_VALID = 1U << 0,
    REFLECTANCE_SAMPLE_CLAMPED_LOW = 1U << 1,
    REFLECTANCE_SAMPLE_CLAMPED_HIGH = 1U << 2,
    REFLECTANCE_SAMPLE_SKY_TOO_SMALL = 1U << 3,
    REFLECTANCE_SAMPLE_GROUND_OVER = 1U << 4,
    REFLECTANCE_SAMPLE_SKY_OVER = 1U << 5,
    REFLECTANCE_SAMPLE_INTERPOLATED = 1U << 6,
};

/** Derived spectrum driven by a ground frame and the latest acceptable sky
 * frame. The header timestamp is always copied from the ground frame. */
typedef struct {
    measurement_record_header_t header;
    uint32_t calculation_count;
    uint32_t ground_frame_count;
    uint32_t sky_frame_count;
    uint64_t sky_b_monotonic_us;
    uint32_t sky_age_us;
    uint16_t sample_count;
    uint16_t valid_sample_count;
    uint16_t clamped_low_count;
    uint16_t clamped_high_count;
    uint16_t invalid_denominator_count;
    uint16_t input_quality_flags;
    /* 0..10000 represents 0.00%..100.00%. */
    uint16_t reflectance_0p01_percent[H1_MAX_SPECTRUM_SAMPLES];
    uint8_t sample_flags[H1_MAX_SPECTRUM_SAMPLES];
} reflectance_record_t;

_Static_assert(sizeof(record_time_t) == 32,
               "v01 timestamp must remain exactly 32 bytes");
_Static_assert(sizeof(measurement_record_header_t) == 64,
               "v01 in-memory common header layout changed");

#ifdef __cplusplus
}
#endif
