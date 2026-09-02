#include "ab_link_test.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "ab_protocol.h"
#include "board_config.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "AB_LINK_TEST";

/* Protocol exercise only: commands below update synthetic state, not the
 * acquisition tasks or SD writer running in the coexistence test. */
#define AB_RX_BUFFER_SIZE 1024
#define AB_LINK_TASK_STACK 4096
#define AB_LINK_TASK_PRIORITY 8
#define AB_TEST_B_FIRMWARE_VERSION 0x0001

typedef struct {
    bool linked;
    bool capturing;
    bool safe_power_off;
    uint8_t tx_sequence;
    uint32_t session_id;
    uint32_t last_completed_session_id;
    /* Zero is an opaque session value, not an "uninitialized" sentinel. */
    bool has_completed_session;
    uint32_t frame_count;
    uint32_t realtime_count;
    int64_t next_heartbeat_us;
} ab_link_test_state_t;

static ab_link_test_state_t s_state;
static TaskHandle_t s_task;

/* This test endpoint has one owner task. Cache bounded retry bursts, not an
 * indefinitely remembered 8-bit SEQ (which eventually wraps). Session checks
 * below independently prevent a repeated start from resetting acquisition. */
typedef struct {
    bool valid;
    uint8_t sequence;
    uint8_t length;
    uint8_t result;
    uint8_t payload[AB_CAPTURE_COMMAND_SIZE];
    int64_t accepted_us;
} action_cache_t;
static action_cache_t s_action_cache[3];
/* Covers the documented 200 ms retry burst, measured from initial handling.
 * Replays do not extend this window indefinitely. */
#define AB_ACTION_RETRY_WINDOW_US 1000000LL

static esp_err_t send_frame(uint8_t command, uint8_t sequence,
                            const uint8_t *payload, uint8_t payload_length)
{
    uint8_t wire[AB_MAX_FRAME_SIZE];
    size_t wire_length = 0;
    if (!ab_frame_encode(command, sequence, payload, payload_length,
                         wire, sizeof(wire), &wire_length)) {
        return ESP_ERR_INVALID_ARG;
    }
    int written = uart_write_bytes(DJI_AB_UART_PORT, wire, wire_length);
    return written == (int)wire_length ? ESP_OK : ESP_FAIL;
}

static void send_ack(const ab_frame_t *request, uint8_t result)
{
    const ab_ack_t ack = {
        .acknowledged_command = request->command,
        .acknowledged_sequence = request->sequence,
        .result = result,
    };
    uint8_t payload[AB_ACK_SIZE];
    if (ab_encode_ack(&ack, payload)) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(send_frame(
            AB_CMD_ACK, request->sequence, payload, sizeof(payload)));
    }
}

static int action_cache_index(uint8_t command)
{
    switch (command) {
    case AB_CMD_START_CAPTURE: return 0;
    case AB_CMD_STOP_CAPTURE: return 1;
    case AB_CMD_PREPARE_POWER_OFF: return 2;
    default: return -1;
    }
}

static bool replay_duplicate_ack(const ab_frame_t *request)
{
    int index = action_cache_index(request->command);
    if (index < 0) return false;
    const action_cache_t *cached = &s_action_cache[index];
    if (!cached->valid || request->length > sizeof(cached->payload) ||
        cached->sequence != request->sequence || cached->length != request->length ||
        esp_timer_get_time() - cached->accepted_us > AB_ACTION_RETRY_WINDOW_US ||
        memcmp(cached->payload, request->payload, request->length) != 0) {
        return false;
    }
    send_ack(request, cached->result);
    ESP_LOGI(TAG, "Replayed ACK for duplicate cmd=0x%02X seq=%u",
             request->command, request->sequence);
    return true;
}

static void cache_and_send_ack(const ab_frame_t *request, uint8_t result)
{
    /* Save the outcome before transmission: a failed/lost ACK must not cause
     * the action to execute again on retry. Negative outcomes are replayed too. */
    int index = action_cache_index(request->command);
    if (index >= 0 && request->length <= sizeof(s_action_cache[index].payload)) {
        action_cache_t *cached = &s_action_cache[index];
        cached->valid = true;
        cached->sequence = request->sequence;
        cached->length = request->length;
        cached->result = result;
        cached->accepted_us = esp_timer_get_time();
        memcpy(cached->payload, request->payload, request->length);
    }
    send_ack(request, result);
}

static void handle_frame(const ab_frame_t *frame)
{
    if (replay_duplicate_ack(frame)) return;
    switch (frame->command) {
    case AB_CMD_HANDSHAKE: {
        ab_handshake_request_t request;
        if (!ab_decode_handshake_request(frame->payload, frame->length,
                                         &request)) {
            ESP_LOGW(TAG, "Rejected malformed handshake");
            return;
        }
        if (!ab_handshake_request_is_valid(&request)) {
            /* No rejection code is defined for handshakes. Drop unsupported
             * requests without claiming readiness or altering a live link. */
            ESP_LOGW(TAG, "Rejected handshake: version=%u drone_link=%u",
                     request.protocol_version, request.drone_link);
            return;
        }
        const ab_handshake_response_t response = {
            .protocol_version = AB_PROTOCOL_VERSION,
            .b_ready = 1,
            .firmware_version = AB_TEST_B_FIRMWARE_VERSION,
            .request_sequence = frame->sequence,
        };
        uint8_t payload[AB_HANDSHAKE_RESPONSE_SIZE];
        ab_encode_handshake_response(&response, payload);
        esp_err_t result = send_frame(AB_CMD_HANDSHAKE_RESPONSE, frame->sequence,
                                      payload, sizeof(payload));
        if (result != ESP_OK) {
            ESP_LOGW(TAG, "Handshake response failed: %s", esp_err_to_name(result));
            return;
        }
        /* Retries and serial-number updates must preserve cadence and session. */
        if (!s_state.linked) {
            s_state.linked = true;
            s_state.next_heartbeat_us = esp_timer_get_time();
        }
        ESP_LOGI(TAG, "Handshake accepted: seq=%u drone_link=%u A-fw=0x%04X",
                 frame->sequence, request.drone_link, request.firmware_version);
        break;
    }
    case AB_CMD_REALTIME_DATA: {
        ab_realtime_data_t data;
        if (!ab_decode_realtime_data(frame->payload, frame->length, &data)) {
            ESP_LOGW(TAG, "Rejected malformed realtime frame");
            return;
        }
        s_state.realtime_count++;
        /* Keep RX-path logging throttled; per-frame printing delays heartbeat
         * and parsing when the navigation stream grows faster. */
        if (s_state.realtime_count == 1 || s_state.realtime_count % 5 == 0) {
            ESP_LOGI(TAG,
                     "GPS #%lu seq=%u lat=%.7f lon=%.7f alt=%.3fm mono=%lums",
                     (unsigned long)s_state.realtime_count, frame->sequence,
                     data.latitude_e7 / 10000000.0,
                     data.longitude_e7 / 10000000.0,
                     data.altitude_relative_mm / 1000.0,
                     (unsigned long)data.a_monotonic_ms);
        }
        break;
    }
    case AB_CMD_START_CAPTURE: {
        ab_start_capture_t command;
        if (!ab_decode_start_capture(frame->payload, frame->length, &command) ||
            command.trigger_source != 1 || command.reserved != 0) {
            cache_and_send_ack(frame, 3);
            return;
        }
        if (s_state.capturing) {
            /* Reconnection may resend this session with a NEW sequence.
             * Session idempotency therefore cannot rely on the retry cache. */
            cache_and_send_ack(frame, command.session_id == s_state.session_id ? 0 : 2);
            return;
        }
        if (s_state.has_completed_session &&
            command.session_id == s_state.last_completed_session_id) {
            /* Remember only the most recently completed task, not a persistent
             * mission history. An ESP32 reset also loses this information. */
            cache_and_send_ack(frame, 0);
            return;
        }
        s_state.session_id = command.session_id;
        s_state.frame_count = 0;
        s_state.safe_power_off = false;
        s_state.capturing = true;
        cache_and_send_ack(frame, 0);
        ESP_LOGI(TAG, "Simulated capture started: session=%lu",
                 (unsigned long)s_state.session_id);
        break;
    }
    case AB_CMD_STOP_CAPTURE: {
        ab_stop_capture_t command;
        if (!ab_decode_stop_capture(frame->payload, frame->length, &command) ||
            command.reserved != 0) {
            cache_and_send_ack(frame, 3);
            return;
        }
        if (!s_state.capturing) {
            cache_and_send_ack(frame, s_state.has_completed_session &&
                command.session_id == s_state.last_completed_session_id ? 0 : 4);
            return;
        }
        if (command.session_id != s_state.session_id) {
            cache_and_send_ack(frame, 4);
            return;
        }
        s_state.capturing = false;
        s_state.last_completed_session_id = s_state.session_id;
        s_state.has_completed_session = true;
        s_state.session_id = 0;
        cache_and_send_ack(frame, 0);
        ESP_LOGI(TAG, "Simulated capture stopped: session=%lu reason=%u",
                 (unsigned long)command.session_id, command.reason);
        break;
    }
    case AB_CMD_PREPARE_POWER_OFF: {
        uint8_t grace_seconds;
        if (!ab_decode_prepare_power_off(frame->payload, frame->length,
                                         &grace_seconds)) {
            cache_and_send_ack(frame, 3);
            return;
        }
        if (s_state.capturing) {
            s_state.last_completed_session_id = s_state.session_id;
            s_state.has_completed_session = true;
        }
        s_state.capturing = false;
        s_state.session_id = 0;
        /* Synthetic readiness only. Production must wait for acquisition stop
         * and successful recorder flush/close before advertising safe=1. */
        s_state.safe_power_off = true;
        cache_and_send_ack(frame, 0);
        ESP_LOGI(TAG, "Simulated data flush complete; safe power-off within %us",
                 grace_seconds);
        break;
    }
    default:
        ESP_LOGW(TAG, "Ignored command 0x%02X seq=%u len=%u",
                 frame->command, frame->sequence, frame->length);
        break;
    }
}

static void send_heartbeat(void)
{
    /* One synthetic frame per heartbeat; this is not the H1 frame count. */
    if (s_state.capturing) s_state.frame_count++;
    const ab_status_report_t status = {
        .b_state = 1,
        .actual_capture = s_state.capturing ? 1 : 0,
        .error_code = 0,
        .storage_free_percent = 75,
        .frame_count = s_state.frame_count,
        .session_id = s_state.capturing ? s_state.session_id : 0,
        .safe_power_off = s_state.safe_power_off ? 1 : 0,
        .reserved = 0,
    };
    uint8_t payload[AB_STATUS_REPORT_SIZE];
    ab_encode_status_report(&status, payload);
    ESP_ERROR_CHECK_WITHOUT_ABORT(send_frame(
        AB_CMD_STATUS_REPORT, s_state.tx_sequence++, payload, sizeof(payload)));
}

static void ab_link_task(void *argument)
{
    (void)argument;
    ab_parser_t parser;
    ab_parser_init(&parser);
    uint8_t bytes[128];
    while (true) {
        int count = uart_read_bytes(DJI_AB_UART_PORT, bytes, sizeof(bytes),
                                    pdMS_TO_TICKS(10));
        int64_t now_us = esp_timer_get_time();
        uint32_t now_ms = (uint32_t)(now_us / 1000);
        for (int i = 0; i < count; i++) {
            ab_frame_t frame;
            ab_parse_result_t result = ab_parser_feed(
                &parser, bytes[i], now_ms, &frame);
            if (result == AB_PARSE_FRAME) {
                handle_frame(&frame);
            } else if (result == AB_PARSE_CRC_ERROR) {
                ESP_LOGW(TAG, "Discarded frame with invalid CRC");
            } else if (result == AB_PARSE_TIMEOUT_RESET) {
                ESP_LOGW(TAG, "Discarded interrupted frame (>100ms gap)");
            }
        }
        now_us = esp_timer_get_time();
        if (s_state.linked && now_us >= s_state.next_heartbeat_us) {
            send_heartbeat();
            /* Skip missed deadlines instead of sending a catch-up burst.
             * RX handling and synchronous logging still share this task. */
            do {
                s_state.next_heartbeat_us += 1000000;
            } while (s_state.next_heartbeat_us <= now_us);
        }
    }
}

esp_err_t ab_link_test_start(void)
{
    if (s_task != NULL) return ESP_ERR_INVALID_STATE;
    const uart_config_t config = {
        .baud_rate = DJI_AB_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(uart_driver_install(
                            DJI_AB_UART_PORT, AB_RX_BUFFER_SIZE, 0, 0, NULL, 0),
                        TAG, "UART driver install failed");
    esp_err_t result = uart_param_config(DJI_AB_UART_PORT, &config);
    if (result == ESP_OK) {
        result = uart_set_pin(DJI_AB_UART_PORT, DJI_AB_UART_TX_GPIO,
                              DJI_AB_UART_RX_GPIO,
                              UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (result != ESP_OK) {
        uart_driver_delete(DJI_AB_UART_PORT);
        return result;
    }
    if (xTaskCreate(ab_link_task, "ab_link_test", AB_LINK_TASK_STACK, NULL,
                    AB_LINK_TASK_PRIORITY, &s_task) != pdPASS) {
        uart_driver_delete(DJI_AB_UART_PORT);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "A-B emulator endpoint: UART%d TX=GPIO%d RX=GPIO%d 115200 8N1",
             DJI_AB_UART_PORT, DJI_AB_UART_TX_GPIO, DJI_AB_UART_RX_GPIO);
    return ESP_OK;
}
