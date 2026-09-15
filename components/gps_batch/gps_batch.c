#include "gps_batch.h"

#include <string.h>

#include "data_records.h"
#include "telemetry_transport.h"

#define DHR_RECORD_SEQUENCE_OFFSET 24U

static void put16(uint8_t **cursor, uint16_t value)
{
    *(*cursor)++ = (uint8_t)value;
    *(*cursor)++ = (uint8_t)(value >> 8);
}

static void put32(uint8_t **cursor, uint32_t value)
{
    for (unsigned shift = 0; shift < 32; shift += 8)
        *(*cursor)++ = (uint8_t)(value >> shift);
}

static uint16_t get16(const uint8_t **cursor)
{
    uint16_t value = (uint16_t)(*cursor)[0] |
                     ((uint16_t)(*cursor)[1] << 8);
    *cursor += 2;
    return value;
}

static uint32_t get32(const uint8_t **cursor)
{
    uint32_t value = (uint32_t)(*cursor)[0] |
                     ((uint32_t)(*cursor)[1] << 8) |
                     ((uint32_t)(*cursor)[2] << 16) |
                     ((uint32_t)(*cursor)[3] << 24);
    *cursor += 4;
    return value;
}

static bool valid_record(const gps_record_t *record)
{
    return record != NULL && record->header.magic == DATA_RECORD_MAGIC &&
           record->header.format_version == DATA_RECORD_FORMAT_VERSION &&
           record->header.record_type == DATA_RECORD_GPS &&
           record->header.header_size == DATA_RECORD_WIRE_HEADER_SIZE &&
           record->header.record_size == GPS_RECORD_WIRE_SIZE;
}

static bool common_header_matches(const gps_record_t *left,
                                  const gps_record_t *right)
{
    return left->header.magic == right->header.magic &&
           left->header.format_version == right->header.format_version &&
           left->header.record_type == right->header.record_type &&
           left->header.header_size == right->header.header_size &&
           left->header.record_size == right->header.record_size &&
           left->header.session_id == right->header.session_id &&
           left->header.segment_id == right->header.segment_id &&
           left->header.flags == right->header.flags;
}

static bool sequence_is_forward(uint32_t previous, uint32_t current)
{
    uint32_t distance = current - previous;
    return distance != 0 && distance < UINT32_C(0x80000000);
}

void gps_batch_reset(gps_batch_t *batch)
{
    if (batch != NULL) batch->record_count = 0;
}

esp_err_t gps_batch_append(gps_batch_t *batch, const gps_record_t *record)
{
    if (batch == NULL || !valid_record(record)) return ESP_ERR_INVALID_ARG;
    if (batch->record_count > GPS_BATCH_MAX_RECORDS)
        return ESP_ERR_INVALID_STATE;
    if (batch->record_count == GPS_BATCH_MAX_RECORDS)
        return ESP_ERR_INVALID_SIZE;
    if (batch->record_count != 0) {
        const gps_record_t *previous =
            &batch->records[batch->record_count - 1U];
        if (!common_header_matches(&batch->records[0], record) ||
            !sequence_is_forward(previous->header.record_sequence,
                                 record->header.record_sequence)) {
            return ESP_ERR_INVALID_STATE;
        }
    }
    batch->records[batch->record_count++] = *record;
    return ESP_OK;
}

bool gps_batch_is_full(const gps_batch_t *batch, uint16_t maximum_records)
{
    return batch != NULL && maximum_records > 0 &&
           maximum_records <= GPS_BATCH_MAX_RECORDS &&
           batch->record_count >= maximum_records;
}

uint32_t gps_batch_message_sequence(const gps_batch_t *batch)
{
    return batch != NULL && batch->record_count != 0
        ? batch->records[0].header.record_sequence : 0;
}

uint64_t gps_batch_first_timestamp_us(const gps_batch_t *batch)
{
    return batch != NULL && batch->record_count != 0
        ? batch->records[0].header.timestamp.b_monotonic_us : 0;
}

esp_err_t gps_batch_serialize(const gps_batch_t *batch, void *output,
                              size_t output_capacity, size_t *output_length)
{
    if (batch == NULL || output == NULL || output_length == NULL ||
        batch->record_count == 0 ||
        batch->record_count > GPS_BATCH_MAX_RECORDS) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t wire_size = GPS_BATCH_WIRE_SIZE(batch->record_count);
    if (output_capacity < wire_size) return ESP_ERR_INVALID_SIZE;

    uint8_t *bytes = output;
    uint8_t *cursor = bytes;
    put32(&cursor, GPS_BATCH_MAGIC);
    put16(&cursor, GPS_BATCH_FORMAT_VERSION);
    put16(&cursor, GPS_BATCH_WIRE_HEADER_SIZE);
    put32(&cursor, wire_size);
    put16(&cursor, batch->record_count);
    put16(&cursor, GPS_RECORD_WIRE_SIZE);

    uint8_t record_wire[GPS_RECORD_WIRE_SIZE];
    uint8_t shared_prefix[GPS_BATCH_SHARED_PREFIX_SIZE];
    for (uint16_t index = 0; index < batch->record_count; index++) {
        size_t record_length = 0;
        esp_err_t result = data_record_serialize_gps(
            &batch->records[index], record_wire, sizeof(record_wire),
            &record_length);
        if (result != ESP_OK || record_length != GPS_RECORD_WIRE_SIZE)
            return result != ESP_OK ? result : ESP_ERR_INVALID_SIZE;
        if (index == 0) {
            memcpy(shared_prefix, record_wire, sizeof(shared_prefix));
            memcpy(cursor, shared_prefix, sizeof(shared_prefix));
            cursor += sizeof(shared_prefix);
        } else if (memcmp(record_wire, shared_prefix,
                          sizeof(shared_prefix)) != 0) {
            return ESP_ERR_INVALID_STATE;
        }
        memcpy(cursor, record_wire + GPS_BATCH_SHARED_PREFIX_SIZE,
               GPS_BATCH_SAMPLE_SUFFIX_SIZE);
        cursor += GPS_BATCH_SAMPLE_SUFFIX_SIZE;
    }
    put32(&cursor, telemetry_crc32(bytes, (size_t)(cursor - bytes)));
    if ((size_t)(cursor - bytes) != wire_size) return ESP_ERR_INVALID_SIZE;
    *output_length = wire_size;
    return ESP_OK;
}

esp_err_t gps_batch_decode(const void *input, size_t input_length,
                           gps_batch_view_t *view)
{
    if (input == NULL || view == NULL) return ESP_ERR_INVALID_ARG;
    if (input_length < GPS_BATCH_WIRE_SIZE(1) ||
        input_length > GPS_BATCH_WIRE_SIZE(GPS_BATCH_MAX_RECORDS)) {
        return ESP_ERR_INVALID_SIZE;
    }
    const uint8_t *bytes = input;
    const uint8_t *stored_cursor = bytes + input_length - 4U;
    uint32_t stored_crc = get32(&stored_cursor);
    if (telemetry_crc32(bytes, input_length - 4U) != stored_crc)
        return ESP_ERR_INVALID_CRC;

    const uint8_t *cursor = bytes;
    uint32_t magic = get32(&cursor);
    uint16_t version = get16(&cursor);
    uint16_t header_size = get16(&cursor);
    uint32_t wire_size = get32(&cursor);
    uint16_t count = get16(&cursor);
    uint16_t record_size = get16(&cursor);
    if (magic != GPS_BATCH_MAGIC || version != GPS_BATCH_FORMAT_VERSION ||
        header_size != GPS_BATCH_WIRE_HEADER_SIZE || count == 0 ||
        count > GPS_BATCH_MAX_RECORDS || record_size != GPS_RECORD_WIRE_SIZE ||
        wire_size != input_length || wire_size != GPS_BATCH_WIRE_SIZE(count)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const uint8_t *prefix = cursor;
    const uint8_t *prefix_cursor = prefix;
    uint32_t record_magic = get32(&prefix_cursor);
    uint16_t record_version = get16(&prefix_cursor);
    uint16_t record_type = get16(&prefix_cursor);
    uint32_t record_header_size = get32(&prefix_cursor);
    uint32_t declared_record_size = get32(&prefix_cursor);
    (void)get32(&prefix_cursor); /* session_id */
    (void)get16(&prefix_cursor); /* segment_id */
    (void)get16(&prefix_cursor); /* flags */
    if (record_magic != DATA_RECORD_MAGIC ||
        record_version != DATA_RECORD_FORMAT_VERSION ||
        record_type != DATA_RECORD_GPS ||
        record_header_size != DATA_RECORD_WIRE_HEADER_SIZE ||
        declared_record_size != GPS_RECORD_WIRE_SIZE) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const uint8_t *suffixes = prefix + GPS_BATCH_SHARED_PREFIX_SIZE;
    uint32_t previous_sequence = 0;
    for (uint16_t index = 0; index < count; index++) {
        const uint8_t *sequence_cursor = suffixes +
            (size_t)index * GPS_BATCH_SAMPLE_SUFFIX_SIZE;
        uint32_t sequence = get32(&sequence_cursor);
        if (index != 0 && !sequence_is_forward(previous_sequence, sequence))
            return ESP_ERR_INVALID_RESPONSE;
        previous_sequence = sequence;
    }

    *view = (gps_batch_view_t) {
        .shared_prefix = prefix,
        .sample_suffixes = suffixes,
        .record_count = count,
    };
    return ESP_OK;
}

esp_err_t gps_batch_decode_record(const gps_batch_view_t *view, uint16_t index,
                                  uint8_t output[GPS_RECORD_WIRE_SIZE])
{
    if (view == NULL || output == NULL || view->shared_prefix == NULL ||
        view->sample_suffixes == NULL || index >= view->record_count ||
        view->record_count > GPS_BATCH_MAX_RECORDS) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(output, view->shared_prefix, GPS_BATCH_SHARED_PREFIX_SIZE);
    memcpy(output + GPS_BATCH_SHARED_PREFIX_SIZE,
           view->sample_suffixes + (size_t)index * GPS_BATCH_SAMPLE_SUFFIX_SIZE,
           GPS_BATCH_SAMPLE_SUFFIX_SIZE);
    uint8_t *crc_cursor = output + GPS_RECORD_WIRE_SIZE - 4U;
    put32(&crc_cursor, telemetry_crc32(output, GPS_RECORD_WIRE_SIZE - 4U));
    return ESP_OK;
}
