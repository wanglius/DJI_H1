#include "ab_link.h"
#include "mission_control.h"
#include "drone_data.h"
#include "clock_sync.h"
#include "measurement_recorder.h"

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

static const char *TAG = "AB_LINK";

/* UART owns protocol state; mission_control owns blocking hardware work. */
#define AB_RX_BUFFER_SIZE 1024
#define AB_LINK_TASK_STACK 4096
#define AB_LINK_TASK_PRIORITY 8
#define AB_B_FIRMWARE_VERSION 0x0001

typedef struct {
    bool linked;
    uint8_t tx_sequence;
    uint32_t realtime_count;
    int64_t next_heartbeat_us;
} ab_link_state_t;

static ab_link_state_t s_state;
static TaskHandle_t s_task;

/* This endpoint has one owner task. Cache bounded retry bursts, not an
 * indefinitely remembered 8-bit SEQ (which eventually wraps). Session checks
 * in mission_control independently prevent repeated starts from resetting acquisition. */
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
            .b_ready = mission_control_ready() ? 1 : 0,
            .firmware_version = AB_B_FIRMWARE_VERSION,
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
        /* Per section 5.1, the link is established only by a ready=1
         * response. A will retry at 1 Hz while asynchronous B-board startup
         * is still in progress. Once linked, a later readiness fault is
         * reported by heartbeat rather than silently destroying the session. */
        if (response.b_ready != 0) {
            if (!s_state.linked) {
                s_state.linked = true;
                s_state.next_heartbeat_us = esp_timer_get_time();
            }
            measurement_recorder_note_handshake(request.drone_serial,
                                                request.firmware_version,
                                                request.drone_link);
        }
        ESP_LOGI(TAG,
                 "Handshake response: seq=%u ready=%u drone_link=%u "
                 "A-fw=0x%04X",
                 frame->sequence, response.b_ready, request.drone_link,
                 request.firmware_version);
        break;
    }
    case AB_CMD_REALTIME_DATA: {
        ab_realtime_data_t data;
        if (!ab_decode_realtime_data(frame->payload, frame->length, &data)) {
            ESP_LOGW(TAG, "Rejected malformed realtime frame");
            return;
        }
        if (!s_state.linked) return;
        int64_t b_receive_us = esp_timer_get_time();
        if (drone_data_update_payload(frame->payload, frame->sequence,
                                      b_receive_us) != ESP_OK) return;
        if (clock_sync_submit(&data, b_receive_us) != ESP_OK) {
            ESP_LOGW(TAG, "Clock observation dropped at GPS #%lu",
                     (unsigned long)(s_state.realtime_count + 1));
            (void)measurement_recorder_log_event(
                MEASUREMENT_EVENT_CLOCK_OBSERVATION_DROP,
                s_state.realtime_count + 1, 0);
        }
        gps_record_t gps;
        if (drone_data_get_latest(&gps) == ESP_OK) {
            (void)clock_sync_timestamp(b_receive_us, &gps.header.timestamp);
            (void)measurement_recorder_submit_gps(&gps);
        }
        s_state.realtime_count++;
        /* Keep RX-path logging throttled; per-frame printing delays heartbeat
         * and parsing when the navigation stream grows faster. */
        if (s_state.realtime_count == 1 || s_state.realtime_count % 25 == 0) {
            record_time_t synchronized;
            clock_sync_timestamp(b_receive_us, &synchronized);
            int64_t utc_delta_ms = 0;
            if ((synchronized.valid_flags & RECORD_TIME_VALID_UTC) &&
                (data.valid_flags & (1U << 2))) {
                uint64_t source_utc_ms = (uint64_t)data.utc_seconds * 1000ULL +
                                         data.utc_milliseconds;
                utc_delta_ms = (int64_t)synchronized.utc_ms -
                               (int64_t)source_utc_ms;
            }
            ESP_LOGI(TAG,
                     "GPS #%lu seq=%u lat=%.7f lon=%.7f alt=%.3fm mono=%lums "
                     "sync=%s gen=%u age=%lums utc_delta=%lldms valid=0x%02X",
                     (unsigned long)s_state.realtime_count, frame->sequence,
                     data.latitude_e7 / 10000000.0,
                     data.longitude_e7 / 10000000.0,
                     data.altitude_relative_mm / 1000.0,
                     (unsigned long)data.a_monotonic_ms,
                     clock_sync_state_name(synchronized.sync_state),
                     synchronized.sync_generation,
                     (unsigned long)synchronized.sync_age_ms,
                     (long long)utc_delta_ms, synchronized.valid_flags);
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
        cache_and_send_ack(frame, s_state.linked ?
            mission_control_start(command.session_id) : 4);
        break;
    }
    case AB_CMD_STOP_CAPTURE: {
        ab_stop_capture_t command;
        if (!ab_decode_stop_capture(frame->payload, frame->length, &command) ||
            command.reserved != 0 || command.reason < 1 || command.reason > 8) {
            cache_and_send_ack(frame, 3);
            return;
        }
        uint8_t result = s_state.linked ?
            mission_control_stop(command.session_id) : 4;
        cache_and_send_ack(frame, result);
        if (result == 0)
            (void)measurement_recorder_log_event(
                MEASUREMENT_EVENT_STOP_REQUEST, command.session_id,
                command.reason);
        break;
    }
    case AB_CMD_PREPARE_POWER_OFF: {
        uint8_t grace_seconds;
        if (!ab_decode_prepare_power_off(frame->payload, frame->length,
                                         &grace_seconds)) {
            cache_and_send_ack(frame, 3);
            return;
        }
        /* Queue the durable event before waking shutdown. The recorder closes
         * auxiliary admission as soon as mission_control handles this request. */
        uint8_t result = 4;
        if (s_state.linked) {
            (void)measurement_recorder_log_event(
                MEASUREMENT_EVENT_POWER_OFF_REQUEST, grace_seconds, 0);
            result = mission_control_power_off(grace_seconds);
        }
        cache_and_send_ack(frame, result);
        ESP_LOGI(TAG, "Power-off request: grace=%us (A owns deadline)", grace_seconds);
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
    ab_status_report_t status;
    mission_control_get_status(&status);
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
                (void)measurement_recorder_log_event(
                    MEASUREMENT_EVENT_PROTOCOL_CRC_ERROR, 0, 0);
            } else if (result == AB_PARSE_TIMEOUT_RESET) {
                ESP_LOGW(TAG, "Discarded interrupted frame (>100ms gap)");
                (void)measurement_recorder_log_event(
                    MEASUREMENT_EVENT_PROTOCOL_TIMEOUT, 0, 0);
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

esp_err_t ab_link_start(void)
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
    if (xTaskCreate(ab_link_task, "ab_link", AB_LINK_TASK_STACK, NULL,
                    AB_LINK_TASK_PRIORITY, &s_task) != pdPASS) {
        uart_driver_delete(DJI_AB_UART_PORT);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "A-B control endpoint: UART%d TX=GPIO%d RX=GPIO%d 115200 8N1",
             DJI_AB_UART_PORT, DJI_AB_UART_TX_GPIO, DJI_AB_UART_RX_GPIO);
    return ESP_OK;
}
