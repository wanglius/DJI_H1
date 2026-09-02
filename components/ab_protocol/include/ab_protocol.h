#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AB_PROTOCOL_VERSION             0x01U
#define AB_FRAME_HEADER_0               0xAAU
#define AB_FRAME_HEADER_1               0x55U
#define AB_MAX_PAYLOAD_SIZE             247U
#define AB_MAX_FRAME_SIZE               (AB_MAX_PAYLOAD_SIZE + 7U)
#define AB_INTERBYTE_TIMEOUT_MS         100U

#define AB_HANDSHAKE_REQUEST_SIZE       36U
#define AB_HANDSHAKE_RESPONSE_SIZE      5U
#define AB_REALTIME_DATA_SIZE           30U
#define AB_CAPTURE_COMMAND_SIZE         6U
#define AB_POWER_OFF_COMMAND_SIZE       1U
#define AB_STATUS_REPORT_SIZE           14U
#define AB_ACK_SIZE                     3U

typedef enum {
    AB_CMD_REALTIME_DATA = 0x01,
    AB_CMD_START_CAPTURE = 0x10,
    AB_CMD_STOP_CAPTURE = 0x11,
    AB_CMD_HANDSHAKE = 0x20,
    AB_CMD_PREPARE_POWER_OFF = 0x30,
    AB_CMD_STATUS_REPORT = 0x81,
    AB_CMD_ACK = 0x90,
    AB_CMD_HANDSHAKE_RESPONSE = 0xA0,
} ab_command_t;

typedef enum {
    AB_PARSE_NONE = 0,
    AB_PARSE_FRAME,
    AB_PARSE_CRC_ERROR,
    AB_PARSE_TIMEOUT_RESET,
} ab_parse_result_t;

typedef struct {
    uint8_t length;
    uint8_t command;
    uint8_t sequence;
    uint8_t payload[AB_MAX_PAYLOAD_SIZE];
} ab_frame_t;

typedef struct {
    uint8_t state;
    uint8_t payload_index;
    uint8_t crc_low;
    bool have_last_byte;
    uint32_t last_byte_ms;
    ab_frame_t frame;
} ab_parser_t;

typedef struct {
    uint8_t protocol_version;
    uint8_t drone_link;
    uint16_t firmware_version;
    uint8_t drone_serial[32];
} ab_handshake_request_t;

typedef struct {
    uint8_t protocol_version;
    uint8_t b_ready;
    uint16_t firmware_version;
    uint8_t request_sequence;
} ab_handshake_response_t;

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
} ab_realtime_data_t;

typedef struct {
    uint8_t trigger_source;
    uint8_t reserved;
    uint32_t session_id;
} ab_start_capture_t;

typedef struct {
    uint8_t reason;
    uint8_t reserved;
    uint32_t session_id;
} ab_stop_capture_t;

typedef struct {
    uint8_t b_state;
    uint8_t actual_capture;
    uint8_t error_code;
    uint8_t storage_free_percent;
    uint32_t frame_count;
    uint32_t session_id;
    uint8_t safe_power_off;
    uint8_t reserved;
} ab_status_report_t;

typedef struct {
    uint8_t acknowledged_command;
    uint8_t acknowledged_sequence;
    uint8_t result;
} ab_ack_t;

uint16_t ab_crc16_ccitt_false(const uint8_t *data, size_t length);

/** Encode a complete wire frame. CRC bytes are emitted low byte first. */
bool ab_frame_encode(uint8_t command, uint8_t sequence,
                     const uint8_t *payload, uint8_t payload_length,
                     uint8_t *output, size_t output_capacity,
                     size_t *output_length);

void ab_parser_init(ab_parser_t *parser);

/**
 * Feed one received byte with a monotonic millisecond timestamp.
 * On AB_PARSE_FRAME, out_frame receives a complete CRC-verified copy.
 */
ab_parse_result_t ab_parser_feed(ab_parser_t *parser, uint8_t byte,
                                 uint32_t now_ms, ab_frame_t *out_frame);

bool ab_encode_handshake_request(const ab_handshake_request_t *value,
                                 uint8_t payload[AB_HANDSHAKE_REQUEST_SIZE]);
bool ab_decode_handshake_request(const uint8_t *payload, size_t length,
                                 ab_handshake_request_t *value);
/** Validate decoded version and drone_link (0 or 1). A disconnected drone
 * and empty serial are allowed during early A-board startup. */
bool ab_handshake_request_is_valid(const ab_handshake_request_t *request);
bool ab_encode_handshake_response(const ab_handshake_response_t *value,
                                  uint8_t payload[AB_HANDSHAKE_RESPONSE_SIZE]);
bool ab_decode_handshake_response(const uint8_t *payload, size_t length,
                                  ab_handshake_response_t *value);
bool ab_encode_realtime_data(const ab_realtime_data_t *value,
                             uint8_t payload[AB_REALTIME_DATA_SIZE]);
bool ab_decode_realtime_data(const uint8_t *payload, size_t length,
                             ab_realtime_data_t *value);
bool ab_encode_start_capture(const ab_start_capture_t *value,
                             uint8_t payload[AB_CAPTURE_COMMAND_SIZE]);
bool ab_decode_start_capture(const uint8_t *payload, size_t length,
                             ab_start_capture_t *value);
bool ab_encode_stop_capture(const ab_stop_capture_t *value,
                            uint8_t payload[AB_CAPTURE_COMMAND_SIZE]);
bool ab_decode_stop_capture(const uint8_t *payload, size_t length,
                            ab_stop_capture_t *value);
bool ab_encode_prepare_power_off(uint8_t grace_seconds,
                                 uint8_t payload[AB_POWER_OFF_COMMAND_SIZE]);
bool ab_decode_prepare_power_off(const uint8_t *payload, size_t length,
                                 uint8_t *grace_seconds);
bool ab_encode_status_report(const ab_status_report_t *value,
                             uint8_t payload[AB_STATUS_REPORT_SIZE]);
bool ab_decode_status_report(const uint8_t *payload, size_t length,
                             ab_status_report_t *value);
bool ab_encode_ack(const ab_ack_t *value, uint8_t payload[AB_ACK_SIZE]);
bool ab_decode_ack(const uint8_t *payload, size_t length, ab_ack_t *value);

#ifdef __cplusplus
}
#endif
