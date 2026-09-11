#include "telemetry_transport.h"

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

static uint64_t get64(const uint8_t **cursor)
{
    uint64_t value = 0;
    for (unsigned shift = 0; shift < 64; shift += 8) {
        value |= (uint64_t)*(*cursor)++ << shift;
    }
    return value;
}

uint32_t telemetry_crc32(const void *data, size_t length)
{
    if (data == NULL && length != 0) return 0;
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

static uint16_t fragment_count_for(uint32_t message_length)
{
    if (message_length == 0) return 1;
    return (uint16_t)((message_length + TELEMETRY_FRAGMENT_PAYLOAD_MAX - 1U) /
                      TELEMETRY_FRAGMENT_PAYLOAD_MAX);
}

static uint16_t payload_length_for(uint32_t message_length,
                                   uint16_t fragment_index)
{
    uint32_t offset = (uint32_t)fragment_index * TELEMETRY_FRAGMENT_PAYLOAD_MAX;
    uint32_t remaining = message_length - offset;
    return (uint16_t)(remaining > TELEMETRY_FRAGMENT_PAYLOAD_MAX
                          ? TELEMETRY_FRAGMENT_PAYLOAD_MAX : remaining);
}

esp_err_t telemetry_fragment_plan_init(
    telemetry_fragment_plan_t *plan, uint8_t message_type,
    uint64_t source_id, uint64_t mission_id, uint32_t message_sequence,
    uint16_t flags,
    const void *payload, size_t payload_length)
{
    if (plan == NULL || message_type == 0 || source_id == 0 ||
        mission_id == 0 ||
        (payload == NULL && payload_length != 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (payload_length > TELEMETRY_MESSAGE_MAX_SIZE ||
        payload_length > UINT32_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    *plan = (telemetry_fragment_plan_t) {
        .payload = payload,
        .payload_length = (uint32_t)payload_length,
        .payload_crc32 = telemetry_crc32(payload, payload_length),
        .source_id = source_id,
        .mission_id = mission_id,
        .message_sequence = message_sequence,
        .fragment_count = fragment_count_for((uint32_t)payload_length),
        .message_type = message_type,
        .flags = flags,
    };
    return ESP_OK;
}

esp_err_t telemetry_fragment_encode(
    const telemetry_fragment_plan_t *plan, uint16_t fragment_index,
    uint8_t *output, size_t output_capacity, size_t *output_length)
{
    if (plan == NULL || output == NULL || output_length == NULL ||
        plan->message_type == 0 || plan->source_id == 0 ||
        plan->mission_id == 0 ||
        plan->fragment_count == 0 ||
        fragment_index >= plan->fragment_count ||
        (plan->payload == NULL && plan->payload_length != 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (plan->payload_length > TELEMETRY_MESSAGE_MAX_SIZE ||
        plan->fragment_count != fragment_count_for(plan->payload_length)) {
        return ESP_ERR_INVALID_STATE;
    }

    uint16_t payload_length = payload_length_for(
        plan->payload_length, fragment_index);
    size_t wire_length = TELEMETRY_FRAGMENT_HEADER_SIZE + payload_length +
                         TELEMETRY_FRAGMENT_TRAILER_SIZE;
    if (output_capacity < wire_length) return ESP_ERR_INVALID_SIZE;

    uint8_t *cursor = output;
    put32(&cursor, TELEMETRY_FRAGMENT_MAGIC);
    *cursor++ = TELEMETRY_FRAGMENT_VERSION;
    *cursor++ = plan->message_type;
    put16(&cursor, TELEMETRY_FRAGMENT_HEADER_SIZE);
    put64(&cursor, plan->source_id);
    put64(&cursor, plan->mission_id);
    put32(&cursor, plan->message_sequence);
    put32(&cursor, plan->payload_length);
    put32(&cursor, plan->payload_crc32);
    put16(&cursor, fragment_index);
    put16(&cursor, plan->fragment_count);
    put16(&cursor, payload_length);
    put16(&cursor, plan->flags);

    uint32_t offset = (uint32_t)fragment_index * TELEMETRY_FRAGMENT_PAYLOAD_MAX;
    if (payload_length != 0) {
        memcpy(cursor, plan->payload + offset, payload_length);
        cursor += payload_length;
    }
    put32(&cursor, telemetry_crc32(output, (size_t)(cursor - output)));
    *output_length = (size_t)(cursor - output);
    return ESP_OK;
}

esp_err_t telemetry_fragment_emit_all(
    const telemetry_fragment_plan_t *plan,
    uint8_t *scratch, size_t scratch_capacity,
    telemetry_fragment_emit_fn emit, void *context)
{
    if (plan == NULL || scratch == NULL || emit == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (plan->message_type == 0 || plan->source_id == 0 ||
        plan->mission_id == 0 ||
        plan->fragment_count == 0 ||
        (plan->payload == NULL && plan->payload_length != 0) ||
        plan->payload_length > TELEMETRY_MESSAGE_MAX_SIZE ||
        plan->fragment_count != fragment_count_for(plan->payload_length)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (scratch_capacity < TELEMETRY_FRAGMENT_WIRE_MAX_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (uint16_t index = 0; index < plan->fragment_count; index++) {
        size_t length = 0;
        esp_err_t result = telemetry_fragment_encode(
            plan, index, scratch, scratch_capacity, &length);
        if (result != ESP_OK) return result;
        result = emit(scratch, length, index, plan->fragment_count, context);
        if (result != ESP_OK) return result;
    }
    return ESP_OK;
}

esp_err_t telemetry_fragment_decode(
    const uint8_t *fragment, size_t fragment_length,
    telemetry_fragment_view_t *view)
{
    if (fragment == NULL || view == NULL) return ESP_ERR_INVALID_ARG;
    if (fragment_length < TELEMETRY_FRAGMENT_HEADER_SIZE +
                              TELEMETRY_FRAGMENT_TRAILER_SIZE ||
        fragment_length > TELEMETRY_FRAGMENT_WIRE_MAX_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }

    const uint8_t *trailer = fragment + fragment_length - 4;
    const uint8_t *trailer_cursor = trailer;
    uint32_t stored_crc = get32(&trailer_cursor);
    if (telemetry_crc32(fragment, fragment_length - 4) != stored_crc) {
        return ESP_ERR_INVALID_CRC;
    }

    const uint8_t *cursor = fragment;
    uint32_t magic = get32(&cursor);
    uint8_t version = *cursor++;
    uint8_t message_type = *cursor++;
    uint16_t header_size = get16(&cursor);
    uint64_t source_id = get64(&cursor);
    uint64_t mission_id = get64(&cursor);
    uint32_t message_sequence = get32(&cursor);
    uint32_t message_length = get32(&cursor);
    uint32_t message_crc32 = get32(&cursor);
    uint16_t fragment_index = get16(&cursor);
    uint16_t fragment_count = get16(&cursor);
    uint16_t payload_length = get16(&cursor);
    uint16_t flags = get16(&cursor);

    if (magic != TELEMETRY_FRAGMENT_MAGIC ||
        version != TELEMETRY_FRAGMENT_VERSION || message_type == 0 ||
        source_id == 0 || mission_id == 0 ||
        header_size != TELEMETRY_FRAGMENT_HEADER_SIZE ||
        message_length > TELEMETRY_MESSAGE_MAX_SIZE || fragment_count == 0 ||
        fragment_count != fragment_count_for(message_length) ||
        fragment_index >= fragment_count ||
        payload_length != payload_length_for(message_length, fragment_index) ||
        fragment_length != TELEMETRY_FRAGMENT_HEADER_SIZE + payload_length +
                               TELEMETRY_FRAGMENT_TRAILER_SIZE) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    *view = (telemetry_fragment_view_t) {
        .payload = cursor,
        .message_length = message_length,
        .message_crc32 = message_crc32,
        .source_id = source_id,
        .mission_id = mission_id,
        .message_sequence = message_sequence,
        .fragment_index = fragment_index,
        .fragment_count = fragment_count,
        .payload_length = payload_length,
        .flags = flags,
        .message_type = message_type,
    };
    return ESP_OK;
}

esp_err_t telemetry_ack_encode(const telemetry_ack_t *ack,
                               uint8_t output[TELEMETRY_ACK_WIRE_SIZE])
{
    if (ack == NULL || output == NULL || ack->source_id == 0 ||
        ack->mission_id == 0 ||
        ack->message_type == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t *cursor = output;
    put32(&cursor, TELEMETRY_ACK_MAGIC);
    *cursor++ = TELEMETRY_ACK_VERSION;
    *cursor++ = ack->message_type;
    put16(&cursor, TELEMETRY_ACK_WIRE_SIZE);
    put64(&cursor, ack->source_id);
    put64(&cursor, ack->mission_id);
    put32(&cursor, ack->message_sequence);
    put32(&cursor, ack->message_crc32);
    put16(&cursor, ack->status);
    put16(&cursor, 0);
    put32(&cursor, telemetry_crc32(output, TELEMETRY_ACK_WIRE_SIZE - 4U));
    return cursor == output + TELEMETRY_ACK_WIRE_SIZE ? ESP_OK : ESP_FAIL;
}

esp_err_t telemetry_ack_decode(const uint8_t *input, size_t input_length,
                               telemetry_ack_t *ack)
{
    if (input == NULL || ack == NULL) return ESP_ERR_INVALID_ARG;
    if (input_length != TELEMETRY_ACK_WIRE_SIZE) return ESP_ERR_INVALID_SIZE;
    const uint8_t *stored_cursor = input + TELEMETRY_ACK_WIRE_SIZE - 4U;
    uint32_t stored_crc = get32(&stored_cursor);
    if (telemetry_crc32(input, TELEMETRY_ACK_WIRE_SIZE - 4U) != stored_crc) {
        return ESP_ERR_INVALID_CRC;
    }
    const uint8_t *cursor = input;
    uint32_t magic = get32(&cursor);
    uint8_t version = *cursor++;
    uint8_t message_type = *cursor++;
    uint16_t size = get16(&cursor);
    uint64_t source_id = get64(&cursor);
    uint64_t mission_id = get64(&cursor);
    uint32_t sequence = get32(&cursor);
    uint32_t message_crc = get32(&cursor);
    uint16_t status = get16(&cursor);
    uint16_t reserved = get16(&cursor);
    if (magic != TELEMETRY_ACK_MAGIC || version != TELEMETRY_ACK_VERSION ||
        size != TELEMETRY_ACK_WIRE_SIZE || source_id == 0 || mission_id == 0 ||
        message_type == 0 || reserved != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    *ack = (telemetry_ack_t) {
        .source_id = source_id,
        .mission_id = mission_id,
        .message_sequence = sequence,
        .message_crc32 = message_crc,
        .message_type = message_type,
        .status = status,
    };
    return ESP_OK;
}
