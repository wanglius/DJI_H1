#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "measurement_records.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Little-endian bytes read "DGB1". This is a telemetry-only container; the
 * on-card GPS_TRACK.BIN format remains a stream of individual DHR1 records. */
#define GPS_BATCH_MAGIC UINT32_C(0x31424744)
#define GPS_BATCH_FORMAT_VERSION 0x01U
#define GPS_BATCH_MAX_RECORDS 10U
#define GPS_BATCH_WIRE_HEADER_SIZE 16U
#define GPS_BATCH_SHARED_PREFIX_SIZE 24U
#define GPS_BATCH_SAMPLE_SUFFIX_SIZE \
    (GPS_RECORD_WIRE_SIZE - GPS_BATCH_SHARED_PREFIX_SIZE - 4U)
#define GPS_BATCH_WIRE_TRAILER_SIZE 4U
#define GPS_BATCH_WIRE_SIZE(record_count) \
    (GPS_BATCH_WIRE_HEADER_SIZE + GPS_BATCH_SHARED_PREFIX_SIZE + \
     (uint32_t)(record_count) * GPS_BATCH_SAMPLE_SUFFIX_SIZE + \
     GPS_BATCH_WIRE_TRAILER_SIZE)

typedef struct {
    uint16_t record_count;
    gps_record_t records[GPS_BATCH_MAX_RECORDS];
} gps_batch_t;

/** Validated zero-copy view of one serialized DGB1 payload. */
typedef struct {
    const uint8_t *shared_prefix;
    const uint8_t *sample_suffixes;
    uint16_t record_count;
} gps_batch_view_t;

void gps_batch_reset(gps_batch_t *batch);

/** Add one record while preserving its complete v01 serialized meaning.
 * Records in one batch must share the DHR1 prefix through header.flags and
 * have strictly forward record sequences. A caller should seal the current
 * partial batch and start another when ESP_ERR_INVALID_STATE is returned. */
esp_err_t gps_batch_append(gps_batch_t *batch, const gps_record_t *record);

bool gps_batch_is_full(const gps_batch_t *batch, uint16_t maximum_records);
uint32_t gps_batch_message_sequence(const gps_batch_t *batch);
uint64_t gps_batch_first_timestamp_us(const gps_batch_t *batch);

/** Serialize a DGB1 payload. The existing DHR1 GPS serializer remains the only
 * owner of the GPS field map; this codec removes repeated prefix bytes only
 * after producing each canonical 98-byte record. */
esp_err_t gps_batch_serialize(const gps_batch_t *batch, void *output,
                              size_t output_capacity, size_t *output_length);

/** Validate a complete DGB1 payload, including its batch CRC. */
esp_err_t gps_batch_decode(const void *input, size_t input_length,
                           gps_batch_view_t *view);

/** Reconstruct one canonical, individually CRC-protected DHR1 GPS record. */
esp_err_t gps_batch_decode_record(const gps_batch_view_t *view, uint16_t index,
                                  uint8_t output[GPS_RECORD_WIRE_SIZE]);

#ifdef __cplusplus
}
#endif
