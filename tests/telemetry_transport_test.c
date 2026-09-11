#include "telemetry_transport_test.h"

#include <string.h>

#include "esp_log.h"
#include "telemetry_transport.h"

static const char *TAG = "TELEM_TEST";

#define CHECK(condition, message) do {       \
    if (!(condition)) {                     \
        ESP_LOGE(TAG, "FAIL: %s", message); \
        return ESP_FAIL;                    \
    }                                       \
} while (0)

static esp_err_t discard_fragment(const uint8_t *fragment, size_t length,
                                  uint16_t index, uint16_t count, void *context)
{
    (void)fragment;
    (void)length;
    (void)index;
    (void)count;
    (void)context;
    return ESP_OK;
}

esp_err_t telemetry_transport_self_test(void)
{
    static uint8_t payload[3172];
    static uint8_t reassembled[sizeof(payload)];
    uint8_t fragment[TELEMETRY_FRAGMENT_WIRE_MAX_SIZE];
    for (size_t i = 0; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)(i * 37U + 11U);
    }

    telemetry_fragment_plan_t plan;
    CHECK(telemetry_fragment_plan_init(
              &plan, TELEMETRY_MESSAGE_REFLECTANCE,
              UINT64_C(0x123456789ABC),
              42, 99, 0,
              payload, sizeof(payload)) == ESP_OK,
          "reflectance plan");
    CHECK(plan.fragment_count == 4, "reflectance fragment count");
    CHECK(plan.payload_crc32 == UINT32_C(0xB2816957),
          "CRC compatibility vector");

    telemetry_fragment_plan_t empty_plan = {0};
    CHECK(telemetry_fragment_emit_all(
              &empty_plan, fragment, sizeof(fragment),
              discard_fragment, NULL) == ESP_ERR_INVALID_STATE,
          "invalid plan rejection");

    static const size_t expected_wire_lengths[] = {1024, 1024, 1024, 292};
    for (uint16_t index = 0; index < plan.fragment_count; index++) {
        size_t wire_length = 0;
        CHECK(telemetry_fragment_encode(
                  &plan, index, fragment, sizeof(fragment), &wire_length) ==
                  ESP_OK,
              "fragment encode");
        CHECK(wire_length == expected_wire_lengths[index],
              "fragment wire length");

        if (index == 0) {
            static const uint8_t python_reference_header[44] = {
                0x44, 0x54, 0x46, 0x32, 0x02, 0x03, 0x2C, 0x00,
                0xBC, 0x9A, 0x78, 0x56, 0x34, 0x12, 0x00, 0x00,
                0x2A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x63, 0x00, 0x00, 0x00, 0x64, 0x0C, 0x00, 0x00,
                0x57, 0x69, 0x81, 0xB2, 0x00, 0x00, 0x04, 0x00,
                0xD0, 0x03, 0x00, 0x00,
            };
            static const uint8_t python_reference_crc[4] = {
                0x89, 0x5B, 0x21, 0x64,
            };
            CHECK(memcmp(fragment, python_reference_header,
                         sizeof(python_reference_header)) == 0,
                  "Python-compatible fragment header");
            CHECK(memcmp(fragment + wire_length - 4, python_reference_crc,
                         sizeof(python_reference_crc)) == 0,
                  "Python-compatible fragment CRC");
        }

        telemetry_fragment_view_t view;
        CHECK(telemetry_fragment_decode(fragment, wire_length, &view) == ESP_OK,
              "fragment decode");
        CHECK(view.source_id == UINT64_C(0x123456789ABC) &&
                  view.mission_id == 42 && view.message_sequence == 99 &&
                  view.message_type == TELEMETRY_MESSAGE_REFLECTANCE &&
                  view.fragment_index == index && view.fragment_count == 4 &&
                  view.message_length == sizeof(payload) &&
                  view.message_crc32 == plan.payload_crc32,
              "fragment metadata");
        memcpy(reassembled + (size_t)index * TELEMETRY_FRAGMENT_PAYLOAD_MAX,
               view.payload, view.payload_length);

        if (index == 0) {
            fragment[wire_length - 1] ^= 0x80;
            CHECK(telemetry_fragment_decode(fragment, wire_length, &view) ==
                      ESP_ERR_INVALID_CRC,
                  "fragment CRC rejection");
        }
    }
    CHECK(memcmp(payload, reassembled, sizeof(payload)) == 0,
          "reflectance reassembly");
    CHECK(telemetry_crc32(reassembled, sizeof(reassembled)) ==
              plan.payload_crc32,
          "message CRC");

    uint8_t gps[98] = {0};
    CHECK(telemetry_fragment_plan_init(
              &plan, TELEMETRY_MESSAGE_GPS, UINT64_C(0x123456789ABC),
              42, 100, 0,
              gps, sizeof(gps)) == ESP_OK && plan.fragment_count == 1,
          "single-fragment GPS plan");
    size_t gps_wire_length = 0;
    CHECK(telemetry_fragment_encode(
              &plan, 0, fragment, sizeof(fragment), &gps_wire_length) == ESP_OK &&
              gps_wire_length == TELEMETRY_FRAGMENT_HEADER_SIZE + sizeof(gps) +
                                     TELEMETRY_FRAGMENT_TRAILER_SIZE,
          "single-fragment GPS encode");
    CHECK(telemetry_fragment_encode(
              &plan, 0, fragment, gps_wire_length - 1, &gps_wire_length) ==
              ESP_ERR_INVALID_SIZE,
          "small output rejection");

    telemetry_ack_t expected_ack = {
        .source_id = UINT64_C(0x123456789ABC),
        .mission_id = UINT64_C(0xFEDCBA9876543210),
        .message_sequence = 100,
        .message_crc32 = plan.payload_crc32,
        .message_type = TELEMETRY_MESSAGE_GPS,
    };
    uint8_t encoded_ack[TELEMETRY_ACK_WIRE_SIZE];
    telemetry_ack_t decoded_ack;
    CHECK(telemetry_ack_encode(&expected_ack, encoded_ack) == ESP_OK &&
              telemetry_ack_decode(encoded_ack, sizeof(encoded_ack),
                                   &decoded_ack) == ESP_OK &&
              decoded_ack.source_id == expected_ack.source_id &&
              decoded_ack.mission_id == expected_ack.mission_id &&
              decoded_ack.message_sequence == expected_ack.message_sequence &&
              decoded_ack.message_crc32 == expected_ack.message_crc32 &&
              decoded_ack.message_type == expected_ack.message_type &&
              decoded_ack.status == 0,
          "cloud acknowledgement round trip");
    encoded_ack[TELEMETRY_ACK_WIRE_SIZE - 1] ^= 1;
    CHECK(telemetry_ack_decode(encoded_ack, sizeof(encoded_ack),
                               &decoded_ack) == ESP_ERR_INVALID_CRC,
          "cloud acknowledgement CRC rejection");

    ESP_LOGI(TAG, "TELEMETRY FRAGMENTATION SELF-TEST PASSED");
    return ESP_OK;
}
