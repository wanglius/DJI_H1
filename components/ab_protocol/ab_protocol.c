#include "ab_protocol.h"

#include <string.h>

enum {
    PARSER_WAIT_AA = 0,
    PARSER_WAIT_55,
    PARSER_LENGTH,
    PARSER_COMMAND,
    PARSER_SEQUENCE,
    PARSER_PAYLOAD,
    PARSER_CRC_LOW,
    PARSER_CRC_HIGH,
};

static uint16_t read_u16_le(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t read_u32_le(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static void write_u16_le(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
}

static void write_u32_le(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
    data[2] = (uint8_t)(value >> 16);
    data[3] = (uint8_t)(value >> 24);
}

uint16_t ab_crc16_ccitt_false(const uint8_t *data, size_t length)
{
    uint16_t crc = 0xFFFF;
    if (data == NULL && length != 0) return crc;
    while (length-- != 0) {
        crc ^= (uint16_t)(*data++) << 8;
        for (uint8_t bit = 0; bit < 8; bit++) {
            crc = (crc & 0x8000U) != 0
                ? (uint16_t)((crc << 1) ^ 0x1021U)
                : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

bool ab_frame_encode(uint8_t command, uint8_t sequence,
                     const uint8_t *payload, uint8_t payload_length,
                     uint8_t *output, size_t output_capacity,
                     size_t *output_length)
{
    size_t frame_length = (size_t)payload_length + 7U;
    if (output == NULL || output_length == NULL ||
        (payload == NULL && payload_length != 0) ||
        payload_length > AB_MAX_PAYLOAD_SIZE || output_capacity < frame_length) {
        return false;
    }

    output[0] = AB_FRAME_HEADER_0;
    output[1] = AB_FRAME_HEADER_1;
    output[2] = payload_length;
    output[3] = command;
    output[4] = sequence;
    if (payload_length != 0) memcpy(&output[5], payload, payload_length);
    uint16_t crc = ab_crc16_ccitt_false(&output[2], payload_length + 3U);
    write_u16_le(&output[5 + payload_length], crc);
    *output_length = frame_length;
    return true;
}

void ab_parser_init(ab_parser_t *parser)
{
    if (parser == NULL) return;
    memset(parser, 0, sizeof(*parser));
    parser->state = PARSER_WAIT_AA;
}

static void parser_reset(ab_parser_t *parser)
{
    parser->state = PARSER_WAIT_AA;
    parser->payload_index = 0;
    parser->have_last_byte = false;
}

static void parser_accept_header_candidate(ab_parser_t *parser, uint8_t byte,
                                           uint32_t now_ms)
{
    if (byte == AB_FRAME_HEADER_0) {
        parser->state = PARSER_WAIT_55;
        parser->last_byte_ms = now_ms;
        parser->have_last_byte = true;
    }
}

ab_parse_result_t ab_parser_feed(ab_parser_t *parser, uint8_t byte,
                                 uint32_t now_ms, ab_frame_t *out_frame)
{
    if (parser == NULL) return AB_PARSE_NONE;
    bool timed_out = parser->state != PARSER_WAIT_AA && parser->have_last_byte &&
                     (uint32_t)(now_ms - parser->last_byte_ms) >
                         AB_INTERBYTE_TIMEOUT_MS;
    if (timed_out) {
        parser_reset(parser);
        parser_accept_header_candidate(parser, byte, now_ms);
        return AB_PARSE_TIMEOUT_RESET;
    }

    parser->last_byte_ms = now_ms;
    parser->have_last_byte = true;
    switch (parser->state) {
    case PARSER_WAIT_AA:
        if (byte == AB_FRAME_HEADER_0) parser->state = PARSER_WAIT_55;
        break;
    case PARSER_WAIT_55:
        if (byte == AB_FRAME_HEADER_1) {
            parser->state = PARSER_LENGTH;
        } else if (byte != AB_FRAME_HEADER_0) {
            parser_reset(parser);
        }
        break;
    case PARSER_LENGTH:
        if (byte > AB_MAX_PAYLOAD_SIZE) {
            parser_reset(parser);
            break;
        }
        parser->frame.length = byte;
        parser->payload_index = 0;
        parser->state = PARSER_COMMAND;
        break;
    case PARSER_COMMAND:
        parser->frame.command = byte;
        parser->state = PARSER_SEQUENCE;
        break;
    case PARSER_SEQUENCE:
        parser->frame.sequence = byte;
        parser->state = parser->frame.length == 0
            ? PARSER_CRC_LOW : PARSER_PAYLOAD;
        break;
    case PARSER_PAYLOAD:
        parser->frame.payload[parser->payload_index++] = byte;
        if (parser->payload_index == parser->frame.length) {
            parser->state = PARSER_CRC_LOW;
        }
        break;
    case PARSER_CRC_LOW:
        parser->crc_low = byte;
        parser->state = PARSER_CRC_HIGH;
        break;
    case PARSER_CRC_HIGH: {
        uint8_t crc_input[AB_MAX_PAYLOAD_SIZE + 3U];
        crc_input[0] = parser->frame.length;
        crc_input[1] = parser->frame.command;
        crc_input[2] = parser->frame.sequence;
        if (parser->frame.length != 0) {
            memcpy(&crc_input[3], parser->frame.payload, parser->frame.length);
        }
        uint16_t expected = ab_crc16_ccitt_false(
            crc_input, (size_t)parser->frame.length + 3U);
        uint16_t received = (uint16_t)parser->crc_low | ((uint16_t)byte << 8);
        ab_parse_result_t result = AB_PARSE_CRC_ERROR;
        if (expected == received) {
            if (out_frame != NULL) *out_frame = parser->frame;
            result = AB_PARSE_FRAME;
        }
        /* A damaged/truncated frame may consume the beginning of the next
         * back-to-back frame as its CRC. Preserve AA or AA 55 at that boundary
         * so one bad frame does not force the following valid frame to be
         * discarded as well. */
        bool have_complete_header = result == AB_PARSE_CRC_ERROR &&
            parser->crc_low == AB_FRAME_HEADER_0 && byte == AB_FRAME_HEADER_1;
        bool have_header_candidate = result == AB_PARSE_CRC_ERROR &&
            byte == AB_FRAME_HEADER_0;
        parser_reset(parser);
        if (have_complete_header) {
            parser->state = PARSER_LENGTH;
            parser->last_byte_ms = now_ms;
            parser->have_last_byte = true;
        } else if (have_header_candidate) {
            parser_accept_header_candidate(parser, byte, now_ms);
        }
        return result;
    }
    default:
        parser_reset(parser);
        break;
    }
    return AB_PARSE_NONE;
}

bool ab_encode_handshake_request(const ab_handshake_request_t *value,
                                 uint8_t payload[AB_HANDSHAKE_REQUEST_SIZE])
{
    if (value == NULL || payload == NULL) return false;
    payload[0] = value->protocol_version;
    payload[1] = value->drone_link;
    write_u16_le(&payload[2], value->firmware_version);
    memcpy(&payload[4], value->drone_serial, sizeof(value->drone_serial));
    return true;
}

bool ab_decode_handshake_request(const uint8_t *payload, size_t length,
                                 ab_handshake_request_t *value)
{
    if (payload == NULL || value == NULL || length != AB_HANDSHAKE_REQUEST_SIZE)
        return false;
    value->protocol_version = payload[0];
    value->drone_link = payload[1];
    value->firmware_version = read_u16_le(&payload[2]);
    memcpy(value->drone_serial, &payload[4], sizeof(value->drone_serial));
    return true;
}

bool ab_handshake_request_is_valid(const ab_handshake_request_t *request)
{
    return request != NULL && request->protocol_version == AB_PROTOCOL_VERSION &&
           request->drone_link <= 1;
}

bool ab_encode_handshake_response(const ab_handshake_response_t *value,
                                  uint8_t payload[AB_HANDSHAKE_RESPONSE_SIZE])
{
    if (value == NULL || payload == NULL) return false;
    payload[0] = value->protocol_version;
    payload[1] = value->b_ready;
    write_u16_le(&payload[2], value->firmware_version);
    payload[4] = value->request_sequence;
    return true;
}

bool ab_decode_handshake_response(const uint8_t *payload, size_t length,
                                  ab_handshake_response_t *value)
{
    if (payload == NULL || value == NULL || length != AB_HANDSHAKE_RESPONSE_SIZE)
        return false;
    value->protocol_version = payload[0];
    value->b_ready = payload[1];
    value->firmware_version = read_u16_le(&payload[2]);
    value->request_sequence = payload[4];
    return true;
}

bool ab_encode_realtime_data(const ab_realtime_data_t *value,
                             uint8_t payload[AB_REALTIME_DATA_SIZE])
{
    if (value == NULL || payload == NULL) return false;
    write_u32_le(&payload[0], (uint32_t)value->latitude_e7);
    write_u32_le(&payload[4], (uint32_t)value->longitude_e7);
    write_u32_le(&payload[8], (uint32_t)value->altitude_relative_mm);
    write_u32_le(&payload[12], value->utc_seconds);
    write_u32_le(&payload[16], value->a_monotonic_ms);
    write_u16_le(&payload[20], value->utc_milliseconds);
    payload[22] = value->source_flags;
    payload[23] = value->gps_fix;
    payload[24] = value->rtk_solution;
    payload[25] = value->flight_status;
    payload[26] = value->display_mode;
    payload[27] = value->battery_percent;
    payload[28] = value->a_status;
    payload[29] = value->valid_flags;
    return true;
}

bool ab_decode_realtime_data(const uint8_t *payload, size_t length,
                             ab_realtime_data_t *value)
{
    if (payload == NULL || value == NULL || length != AB_REALTIME_DATA_SIZE)
        return false;
    value->latitude_e7 = (int32_t)read_u32_le(&payload[0]);
    value->longitude_e7 = (int32_t)read_u32_le(&payload[4]);
    value->altitude_relative_mm = (int32_t)read_u32_le(&payload[8]);
    value->utc_seconds = read_u32_le(&payload[12]);
    value->a_monotonic_ms = read_u32_le(&payload[16]);
    value->utc_milliseconds = read_u16_le(&payload[20]);
    value->source_flags = payload[22];
    value->gps_fix = payload[23];
    value->rtk_solution = payload[24];
    value->flight_status = payload[25];
    value->display_mode = payload[26];
    value->battery_percent = payload[27];
    value->a_status = payload[28];
    value->valid_flags = payload[29];
    return true;
}

static bool encode_capture(uint8_t first, uint8_t reserved, uint32_t session_id,
                           uint8_t payload[AB_CAPTURE_COMMAND_SIZE])
{
    if (payload == NULL) return false;
    payload[0] = first;
    payload[1] = reserved;
    write_u32_le(&payload[2], session_id);
    return true;
}

bool ab_encode_start_capture(const ab_start_capture_t *value,
                             uint8_t payload[AB_CAPTURE_COMMAND_SIZE])
{
    return value != NULL && encode_capture(value->trigger_source,
                                           value->reserved,
                                           value->session_id, payload);
}

bool ab_decode_start_capture(const uint8_t *payload, size_t length,
                             ab_start_capture_t *value)
{
    if (payload == NULL || value == NULL || length != AB_CAPTURE_COMMAND_SIZE)
        return false;
    value->trigger_source = payload[0];
    value->reserved = payload[1];
    value->session_id = read_u32_le(&payload[2]);
    return true;
}

bool ab_encode_stop_capture(const ab_stop_capture_t *value,
                            uint8_t payload[AB_CAPTURE_COMMAND_SIZE])
{
    return value != NULL && encode_capture(value->reason, value->reserved,
                                           value->session_id, payload);
}

bool ab_decode_stop_capture(const uint8_t *payload, size_t length,
                            ab_stop_capture_t *value)
{
    if (payload == NULL || value == NULL || length != AB_CAPTURE_COMMAND_SIZE)
        return false;
    value->reason = payload[0];
    value->reserved = payload[1];
    value->session_id = read_u32_le(&payload[2]);
    return true;
}

bool ab_encode_prepare_power_off(uint8_t grace_seconds,
                                 uint8_t payload[AB_POWER_OFF_COMMAND_SIZE])
{
    if (payload == NULL) return false;
    payload[0] = grace_seconds;
    return true;
}

bool ab_decode_prepare_power_off(const uint8_t *payload, size_t length,
                                 uint8_t *grace_seconds)
{
    if (payload == NULL || grace_seconds == NULL ||
        length != AB_POWER_OFF_COMMAND_SIZE) return false;
    *grace_seconds = payload[0];
    return true;
}

bool ab_encode_status_report(const ab_status_report_t *value,
                             uint8_t payload[AB_STATUS_REPORT_SIZE])
{
    if (value == NULL || payload == NULL) return false;
    payload[0] = value->b_state;
    payload[1] = value->actual_capture;
    payload[2] = value->error_code;
    payload[3] = value->storage_free_percent;
    write_u32_le(&payload[4], value->frame_count);
    write_u32_le(&payload[8], value->session_id);
    payload[12] = value->safe_power_off;
    payload[13] = value->reserved;
    return true;
}

bool ab_decode_status_report(const uint8_t *payload, size_t length,
                             ab_status_report_t *value)
{
    if (payload == NULL || value == NULL || length != AB_STATUS_REPORT_SIZE)
        return false;
    value->b_state = payload[0];
    value->actual_capture = payload[1];
    value->error_code = payload[2];
    value->storage_free_percent = payload[3];
    value->frame_count = read_u32_le(&payload[4]);
    value->session_id = read_u32_le(&payload[8]);
    value->safe_power_off = payload[12];
    value->reserved = payload[13];
    return true;
}

bool ab_encode_ack(const ab_ack_t *value, uint8_t payload[AB_ACK_SIZE])
{
    if (value == NULL || payload == NULL) return false;
    payload[0] = value->acknowledged_command;
    payload[1] = value->acknowledged_sequence;
    payload[2] = value->result;
    return true;
}

bool ab_decode_ack(const uint8_t *payload, size_t length, ab_ack_t *value)
{
    if (payload == NULL || value == NULL || length != AB_ACK_SIZE) return false;
    value->acknowledged_command = payload[0];
    value->acknowledged_sequence = payload[1];
    value->result = payload[2];
    return true;
}
