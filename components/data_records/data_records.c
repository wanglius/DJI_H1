#include "data_records.h"

#include <stdbool.h>
#include <string.h>

static void put16(uint8_t **cursor, uint16_t value)
{
    *(*cursor)++ = (uint8_t)value;
    *(*cursor)++ = (uint8_t)(value >> 8);
}

static void put32(uint8_t **cursor, uint32_t value)
{
    for (unsigned shift = 0; shift < 32; shift += 8) {
        *(*cursor)++ = (uint8_t)(value >> shift);
    }
}

static void put64(uint8_t **cursor, uint64_t value)
{
    for (unsigned shift = 0; shift < 64; shift += 8) {
        *(*cursor)++ = (uint8_t)(value >> shift);
    }
}

static uint32_t crc32(const void *data, size_t length)
{
    const uint8_t *bytes = data;
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < length; i++) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; bit++) {
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1) ^ (UINT32_C(0xEDB88320) & mask);
        }
    }
    return ~crc;
}

static void serialize_time(uint8_t **cursor, const record_time_t *time)
{
    put64(cursor, time->b_monotonic_us);
    put64(cursor, time->a_monotonic_ms);
    put64(cursor, time->utc_ms);
    put32(cursor, time->sync_age_ms);
    put16(cursor, time->sync_generation);
    *(*cursor)++ = time->sync_state;
    *(*cursor)++ = time->valid_flags;
}

static void serialize_header(uint8_t **cursor,
                             const measurement_record_header_t *header,
                             uint32_t wire_size)
{
    put32(cursor, DATA_RECORD_MAGIC);
    put16(cursor, DATA_RECORD_FORMAT_VERSION);
    put16(cursor, header->record_type);
    put32(cursor, DATA_RECORD_WIRE_HEADER_SIZE);
    put32(cursor, wire_size);
    put32(cursor, header->session_id);
    put16(cursor, header->segment_id);
    put16(cursor, header->flags);
    put32(cursor, header->record_sequence);
    serialize_time(cursor, &header->timestamp);
}

static bool valid_header(const measurement_record_header_t *header,
                         data_record_type_t type, uint32_t wire_size)
{
    return header->magic == DATA_RECORD_MAGIC &&
           header->format_version == DATA_RECORD_FORMAT_VERSION &&
           header->record_type == (uint16_t)type &&
           header->header_size == DATA_RECORD_WIRE_HEADER_SIZE &&
           header->record_size == wire_size;
}

static esp_err_t finish_record(uint8_t *output, uint8_t *cursor,
                               size_t output_capacity, uint32_t wire_size,
                               size_t *output_length)
{
    size_t body_length = (size_t)(cursor - output);
    if (body_length + 4U != wire_size || output_capacity < wire_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    put32(&cursor, crc32(output, body_length));
    *output_length = (size_t)(cursor - output);
    return ESP_OK;
}

void data_record_header_init(data_record_header_t *header,
                             data_record_type_t type,
                             uint32_t record_size,
                             uint32_t sequence,
                             uint32_t session_id,
                             uint16_t segment_id,
                             const record_time_t *timestamp)
{
    if (header == NULL) return;
    /* timestamp may legally refer to header->timestamp during in-place header
     * renewal. Preserve it before clearing the destination. */
    record_time_t preserved_time = {0};
    if (timestamp != NULL) preserved_time = *timestamp;
    memset(header, 0, sizeof(*header));
    header->magic = DATA_RECORD_MAGIC;
    header->format_version = DATA_RECORD_FORMAT_VERSION;
    header->record_type = (uint16_t)type;
    header->header_size = DATA_RECORD_WIRE_HEADER_SIZE;
    header->record_size = record_size;
    header->record_sequence = sequence;
    header->session_id = session_id;
    header->segment_id = segment_id;
    if (timestamp != NULL) header->timestamp = preserved_time;
}

esp_err_t data_record_serialize_raw(const raw_spectrum_record_t *record,
                                    void *output, size_t output_capacity,
                                    size_t *output_length)
{
    if (record == NULL || output == NULL || output_length == NULL ||
        record->sample_count == 0 ||
        record->sample_count > H1_MAX_SPECTRUM_SAMPLES) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t wire_size = RAW_RECORD_WIRE_SIZE(record->sample_count);
    if (!valid_header(&record->header, DATA_RECORD_RAW_SPECTRUM, wire_size)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (output_capacity < wire_size) return ESP_ERR_INVALID_SIZE;

    uint8_t *bytes = output;
    uint8_t *cursor = bytes;
    serialize_header(&cursor, &record->header, wire_size);
    put32(&cursor, record->frame_count);
    put32(&cursor, record->exposure_us);
    put16(&cursor, record->sample_count);
    put16(&cursor, (uint16_t)record->spectrum_scale);
    *cursor++ = record->spectrometer_role;
    *cursor++ = record->exposure_status;
    *cursor++ = record->frame_quality;
    *cursor++ = 0;
    for (uint16_t i = 0; i < record->sample_count; i++) {
        put16(&cursor, record->samples[i]);
    }
    return finish_record(bytes, cursor, output_capacity, wire_size,
                         output_length);
}

esp_err_t data_record_serialize_reflectance(
    const reflectance_record_t *record, void *output, size_t output_capacity,
    size_t *output_length)
{
    if (record == NULL || output == NULL || output_length == NULL ||
        record->sample_count == 0 ||
        record->sample_count > H1_MAX_SPECTRUM_SAMPLES) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t wire_size = REFLECTANCE_RECORD_WIRE_SIZE(record->sample_count);
    if (!valid_header(&record->header, DATA_RECORD_REFLECTANCE, wire_size)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (output_capacity < wire_size) return ESP_ERR_INVALID_SIZE;

    uint8_t *bytes = output;
    uint8_t *cursor = bytes;
    serialize_header(&cursor, &record->header, wire_size);
    put32(&cursor, record->calculation_count);
    put32(&cursor, record->ground_frame_count);
    put32(&cursor, record->sky_frame_count);
    put64(&cursor, record->sky_b_monotonic_us);
    put32(&cursor, record->sky_age_us);
    put16(&cursor, record->sample_count);
    put16(&cursor, record->valid_sample_count);
    put16(&cursor, record->clamped_low_count);
    put16(&cursor, record->clamped_high_count);
    put16(&cursor, record->invalid_denominator_count);
    put16(&cursor, record->input_quality_flags);
    for (uint16_t i = 0; i < record->sample_count; i++) {
        put16(&cursor, record->reflectance_0p01_percent[i]);
    }
    memcpy(cursor, record->sample_flags, record->sample_count);
    cursor += record->sample_count;
    return finish_record(bytes, cursor, output_capacity, wire_size,
                         output_length);
}

esp_err_t data_record_serialize_gps(const gps_record_t *record,
                                    void *output, size_t output_capacity,
                                    size_t *output_length)
{
    if (record == NULL || output == NULL || output_length == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!valid_header(&record->header, DATA_RECORD_GPS,
                      GPS_RECORD_WIRE_SIZE)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (output_capacity < GPS_RECORD_WIRE_SIZE) return ESP_ERR_INVALID_SIZE;

    uint8_t *bytes = output;
    uint8_t *cursor = bytes;
    serialize_header(&cursor, &record->header, GPS_RECORD_WIRE_SIZE);
    *cursor++ = record->protocol_sequence;
    *cursor++ = 0;
    *cursor++ = 0;
    *cursor++ = 0;
    put32(&cursor, (uint32_t)record->data.latitude_e7);
    put32(&cursor, (uint32_t)record->data.longitude_e7);
    put32(&cursor, (uint32_t)record->data.altitude_relative_mm);
    put32(&cursor, record->data.utc_seconds);
    put32(&cursor, record->data.a_monotonic_ms);
    put16(&cursor, record->data.utc_milliseconds);
    *cursor++ = record->data.source_flags;
    *cursor++ = record->data.gps_fix;
    *cursor++ = record->data.rtk_solution;
    *cursor++ = record->data.flight_status;
    *cursor++ = record->data.display_mode;
    *cursor++ = record->data.battery_percent;
    *cursor++ = record->data.a_status;
    *cursor++ = record->data.valid_flags;
    return finish_record(bytes, cursor, output_capacity, GPS_RECORD_WIRE_SIZE,
                         output_length);
}
