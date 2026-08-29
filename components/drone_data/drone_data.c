#include "drone_data.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

const drone_realtime_data_t DRONE_DATA_FAKE_FIXED = {
    .latitude_e7 = 399042000,
    .longitude_e7 = 1164074000,
    .altitude_relative_mm = 120000,
    .utc_seconds = 1767225600,
    .a_monotonic_ms = 123456,
    .utc_milliseconds = 500,
    .source_flags = 0,
    .gps_fix = 3,
    .rtk_solution = 0,
    .flight_status = 2,
    .display_mode = 0,
    .battery_percent = 85,
    .a_status = 0x0F,
    .valid_flags = DRONE_VALID_POSITION | DRONE_VALID_ALTITUDE |
                   DRONE_VALID_UTC | DRONE_VALID_BATTERY,
};

static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_storage;
static portMUX_TYPE s_init_guard = portMUX_INITIALIZER_UNLOCKED;
static gps_record_t s_latest;
static uint32_t s_record_sequence;

static uint16_t read_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t read_le32(const uint8_t *data)
{
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static esp_err_t ensure_initialized(void)
{
    taskENTER_CRITICAL(&s_init_guard);
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
    }
    taskEXIT_CRITICAL(&s_init_guard);
    return s_lock != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static void publish(const drone_realtime_data_t *data,
                    uint8_t protocol_sequence,
                    int64_t b_timestamp_us)
{
    data_record_header_init(&s_latest.header, DATA_RECORD_GPS,
                            sizeof(s_latest), ++s_record_sequence,
                            0, b_timestamp_us);
    s_latest.protocol_sequence = protocol_sequence;
    memset(s_latest.reserved, 0, sizeof(s_latest.reserved));
    s_latest.data = *data;
}

esp_err_t drone_data_init_fake(void)
{
    esp_err_t result = ensure_initialized();
    if (result != ESP_OK) return result;
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return ESP_FAIL;
    s_record_sequence = 0;
    publish(&DRONE_DATA_FAKE_FIXED, 0, 0);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t drone_data_update_payload(
    const uint8_t payload[DRONE_REALTIME_PAYLOAD_SIZE],
    uint8_t protocol_sequence,
    int64_t b_receive_timestamp_us)
{
    if (payload == NULL) return ESP_ERR_INVALID_ARG;
    esp_err_t result = ensure_initialized();
    if (result != ESP_OK) return result;

    drone_realtime_data_t decoded = {
        .latitude_e7 = (int32_t)read_le32(payload + 0),
        .longitude_e7 = (int32_t)read_le32(payload + 4),
        .altitude_relative_mm = (int32_t)read_le32(payload + 8),
        .utc_seconds = read_le32(payload + 12),
        .a_monotonic_ms = read_le32(payload + 16),
        .utc_milliseconds = read_le16(payload + 20),
        .source_flags = payload[22],
        .gps_fix = payload[23],
        .rtk_solution = payload[24],
        .flight_status = payload[25],
        .display_mode = payload[26],
        .battery_percent = payload[27],
        .a_status = payload[28],
        .valid_flags = payload[29],
    };
    if (decoded.utc_milliseconds > 999 || decoded.battery_percent > 100) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return ESP_FAIL;
    publish(&decoded, protocol_sequence, b_receive_timestamp_us);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t drone_data_get_latest(gps_record_t *out_record)
{
    if (out_record == NULL) return ESP_ERR_INVALID_ARG;
    if (s_lock == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return ESP_FAIL;
    *out_record = s_latest;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}
