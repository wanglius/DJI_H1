#include "ab_protocol_test.h"

#include <string.h>

#include "ab_protocol.h"
#include "esp_log.h"

static const char *TAG = "AB_PROTO_TEST";

#define CHECK(condition, message) do {       \
    if (!(condition)) {                     \
        ESP_LOGE(TAG, "FAIL: %s", message); \
        return ESP_FAIL;                    \
    }                                       \
} while (0)

esp_err_t ab_protocol_self_test(void)
{
    static const uint8_t crc_vector[] = "123456789";
    CHECK(ab_crc16_ccitt_false(crc_vector, sizeof(crc_vector) - 1) == 0x29B1,
          "CRC16 standard vector");

    uint8_t power_payload[AB_POWER_OFF_COMMAND_SIZE];
    CHECK(ab_encode_prepare_power_off(10, power_payload), "power payload");
    uint8_t wire[AB_MAX_FRAME_SIZE];
    size_t wire_length = 0;
    CHECK(ab_frame_encode(AB_CMD_PREPARE_POWER_OFF, 7, power_payload,
                          sizeof(power_payload), wire, sizeof(wire),
                          &wire_length), "sample frame encode");
    static const uint8_t expected[] = {
        0xAA, 0x55, 0x01, 0x30, 0x07, 0x0A, 0x0C, 0x0F,
    };
    CHECK(wire_length == sizeof(expected) &&
              memcmp(wire, expected, sizeof(expected)) == 0,
          "documented complete-frame vector");

    const ab_handshake_request_t handshake = {
        .protocol_version = AB_PROTOCOL_VERSION,
        .drone_link = 1,
        .firmware_version = 0x1234,
        .drone_serial = "12345678901234567890123456789012",
    };
    uint8_t handshake_payload[AB_HANDSHAKE_REQUEST_SIZE];
    ab_handshake_request_t decoded_handshake = {0};
    CHECK(ab_encode_handshake_request(&handshake, handshake_payload) &&
              ab_decode_handshake_request(handshake_payload,
                                          sizeof(handshake_payload),
                                          &decoded_handshake),
          "handshake round trip");
    CHECK(decoded_handshake.firmware_version == handshake.firmware_version &&
              memcmp(decoded_handshake.drone_serial, handshake.drone_serial,
                     sizeof(handshake.drone_serial)) == 0,
          "32-byte non-terminated serial preservation");

    CHECK(!ab_handshake_request_is_valid(NULL), "null handshake rejected");
    /* Exhaust the byte-valued policy fields, including early drone_link=0. */
    for (unsigned version = 0; version <= UINT8_MAX; version++) {
        decoded_handshake.protocol_version = (uint8_t)version;
        CHECK(ab_handshake_request_is_valid(&decoded_handshake) ==
                  (version == AB_PROTOCOL_VERSION), "handshake version gate");
    }
    decoded_handshake.protocol_version = AB_PROTOCOL_VERSION;
    for (unsigned link = 0; link <= UINT8_MAX; link++) {
        decoded_handshake.drone_link = (uint8_t)link;
        CHECK(ab_handshake_request_is_valid(&decoded_handshake) == (link <= 1),
              "handshake drone-link gate");
    }
    decoded_handshake.drone_link = 0;
    memset(decoded_handshake.drone_serial, 0, sizeof(decoded_handshake.drone_serial));
    CHECK(ab_handshake_request_is_valid(&decoded_handshake),
          "early handshake before drone link accepted");
    CHECK(!ab_decode_handshake_request(handshake_payload,
        sizeof(handshake_payload) - 1, &decoded_handshake),
        "truncated handshake rejected");

    const ab_realtime_data_t realtime = {
        .latitude_e7 = 399042000,
        .longitude_e7 = 1164074000,
        .altitude_relative_mm = -1250,
        .utc_seconds = 1767225600,
        .a_monotonic_ms = 0xFFFFFFF0,
        .utc_milliseconds = 999,
        .source_flags = 3,
        .gps_fix = 3,
        .rtk_solution = 50,
        .flight_status = 2,
        .display_mode = 15,
        .battery_percent = 85,
        .a_status = 0x1F,
        .valid_flags = 0x0F,
    };
    uint8_t realtime_payload[AB_REALTIME_DATA_SIZE];
    ab_realtime_data_t decoded_realtime = {0};
    CHECK(ab_encode_realtime_data(&realtime, realtime_payload) &&
              ab_decode_realtime_data(realtime_payload,
                                      sizeof(realtime_payload),
                                      &decoded_realtime),
          "realtime data round trip");
    CHECK(decoded_realtime.latitude_e7 == realtime.latitude_e7 &&
              decoded_realtime.altitude_relative_mm ==
                  realtime.altitude_relative_mm &&
              decoded_realtime.a_monotonic_ms == realtime.a_monotonic_ms &&
              decoded_realtime.utc_milliseconds == realtime.utc_milliseconds &&
              decoded_realtime.rtk_solution == realtime.rtk_solution,
          "signed and wrapping realtime fields");

    const ab_status_report_t status = {
        .b_state = 1,
        .actual_capture = 1,
        .error_code = 0,
        .storage_free_percent = 73,
        .frame_count = 0x12345678,
        .session_id = 0xA1B2C3D4,
        .safe_power_off = 0,
        .reserved = 0,
    };
    uint8_t status_payload[AB_STATUS_REPORT_SIZE];
    CHECK(ab_encode_status_report(&status, status_payload), "status encode");
    CHECK(ab_frame_encode(AB_CMD_STATUS_REPORT, 0xFE, status_payload,
                          sizeof(status_payload), wire, sizeof(wire),
                          &wire_length), "status frame encode");

    ab_parser_t parser;
    ab_parser_init(&parser);
    ab_frame_t frame = {0};
    ab_parse_result_t parse_result = AB_PARSE_NONE;
    for (size_t i = 0; i < wire_length; i++) {
        parse_result = ab_parser_feed(&parser, wire[i], (uint32_t)i, &frame);
    }
    CHECK(parse_result == AB_PARSE_FRAME, "fragmented frame parse");
    CHECK(frame.command == AB_CMD_STATUS_REPORT && frame.sequence == 0xFE &&
              frame.length == AB_STATUS_REPORT_SIZE,
          "parsed frame metadata");
    ab_status_report_t decoded = {0};
    CHECK(ab_decode_status_report(frame.payload, frame.length, &decoded),
          "status decode");
    CHECK(decoded.b_state == status.b_state &&
              decoded.actual_capture == status.actual_capture &&
              decoded.storage_free_percent == status.storage_free_percent &&
              decoded.frame_count == status.frame_count &&
              decoded.session_id == status.session_id,
          "status round trip");

    wire[wire_length - 1] ^= 0x01;
    ab_parser_init(&parser);
    for (size_t i = 0; i < wire_length; i++) {
        parse_result = ab_parser_feed(&parser, wire[i], (uint32_t)i, &frame);
    }
    CHECK(parse_result == AB_PARSE_CRC_ERROR, "bad CRC rejection");

    ab_parser_init(&parser);
    CHECK(ab_parser_feed(&parser, 0xAA, 0, &frame) == AB_PARSE_NONE,
          "timeout setup");
    CHECK(ab_parser_feed(&parser, 0x55, AB_INTERBYTE_TIMEOUT_MS + 1,
                         &frame) == AB_PARSE_TIMEOUT_RESET,
          "inter-byte timeout");

    /* Restore CRC, then test payload corruption rather than only CRC damage. */
    wire[wire_length - 1] ^= 0x01;
    wire[5] ^= 0x01;
    ab_parser_init(&parser);
    for (size_t i = 0; i < wire_length; i++) {
        parse_result = ab_parser_feed(&parser, wire[i], (uint32_t)i, &frame);
    }
    CHECK(parse_result == AB_PARSE_CRC_ERROR, "payload corruption rejected");
    wire[5] ^= 0x01;

    static const uint8_t garbage[] = {0, 0x55, 0x13, 0xAA, 0xAA, 0x42};
    ab_parser_init(&parser);
    for (size_t i = 0; i < sizeof(garbage); i++) {
        CHECK(ab_parser_feed(&parser, garbage[i], (uint32_t)i, &frame) ==
                  AB_PARSE_NONE, "garbage rejection");
    }
    for (size_t i = 0; i < wire_length; i++) {
        parse_result = ab_parser_feed(&parser, wire[i],
                                     (uint32_t)(sizeof(garbage) + i), &frame);
    }
    CHECK(parse_result == AB_PARSE_FRAME && frame.sequence == 0xFE,
          "header resync after garbage");

    /* A timeout abandons the partial frame, but preserves a new AA candidate. */
    ab_parser_init(&parser);
    for (size_t i = 0; i < 6; i++) {
        ab_parser_feed(&parser, wire[i], (uint32_t)i, &frame);
    }
    CHECK(ab_parser_feed(&parser, wire[0], AB_INTERBYTE_TIMEOUT_MS + 10,
                         &frame) == AB_PARSE_TIMEOUT_RESET, "truncation timeout");
    for (size_t i = 1; i < wire_length; i++) {
        parse_result = ab_parser_feed(&parser, wire[i],
            AB_INTERBYTE_TIMEOUT_MS + 10 + (uint32_t)i, &frame);
    }
    CHECK(parse_result == AB_PARSE_FRAME && frame.sequence == 0xFE,
          "valid frame after truncation");

    /* A back-to-back frame header can occupy either one or both missing CRC
     * bytes of a truncated predecessor. Both cases must preserve the valid
     * successor without needing an inter-byte timeout. */
    for (size_t missing_crc = 1; missing_crc <= 2; missing_crc++) {
        ab_parser_init(&parser);
        unsigned crc_errors = 0;
        unsigned recovered_frames = 0;
        for (size_t i = 0; i < wire_length - missing_crc; i++) {
            (void)ab_parser_feed(&parser, wire[i], (uint32_t)i, &frame);
        }
        for (size_t i = 0; i < wire_length; i++) {
            ab_parse_result_t recovered = ab_parser_feed(
                &parser, wire[i], (uint32_t)(wire_length + i), &frame);
            if (recovered == AB_PARSE_CRC_ERROR) crc_errors++;
            if (recovered == AB_PARSE_FRAME) recovered_frames++;
        }
        CHECK(crc_errors == 1 && recovered_frames == 1 &&
                  frame.sequence == 0xFE,
              "CRC-boundary header recovery");
    }

    uint8_t second_wire[AB_MAX_FRAME_SIZE];
    size_t second_length = 0;
    CHECK(ab_frame_encode(AB_CMD_PREPARE_POWER_OFF, 0xFF, power_payload,
        sizeof(power_payload), second_wire, sizeof(second_wire), &second_length),
        "back-to-back frame encode");
    ab_parser_init(&parser);
    unsigned parsed_frames = 0;
    for (size_t i = 0; i < wire_length + second_length; i++) {
        uint8_t byte = i < wire_length ? wire[i] : second_wire[i - wire_length];
        if (ab_parser_feed(&parser, byte, (uint32_t)i, &frame) == AB_PARSE_FRAME) {
            parsed_frames++;
        }
    }
    CHECK(parsed_frames == 2 && frame.command == AB_CMD_PREPARE_POWER_OFF,
          "back-to-back frames");

    ab_parser_init(&parser);
    CHECK(ab_parser_feed(&parser, 0xAA, UINT32_MAX - 50U, &frame) == AB_PARSE_NONE,
          "timestamp rollover setup");
    CHECK(ab_parser_feed(&parser, 0x55, 60, &frame) == AB_PARSE_TIMEOUT_RESET,
          "timestamp rollover timeout");

    ESP_LOGI(TAG, "A-B PROTOCOL SELF-TEST PASSED");
    return ESP_OK;
}
