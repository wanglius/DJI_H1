#include "drone_data.h"

#include <string.h>

#include "ab_protocol.h"
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
static bool s_have_sample;

static esp_err_t ensure_initialized(void)
{
    taskENTER_CRITICAL(&s_init_guard);
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
    }
    taskEXIT_CRITICAL(&s_init_guard);
    return s_lock != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

/* Caller holds s_lock across sequence allocation and the complete snapshot. */
static void publish(const drone_realtime_data_t *data,
                    uint8_t protocol_sequence,
                    int64_t b_timestamp_us)
{
    record_time_t timestamp = {
        .b_monotonic_us = b_timestamp_us >= 0 ? (uint64_t)b_timestamp_us : 0,
        .valid_flags = RECORD_TIME_VALID_B_MONOTONIC,
    };
    data_record_header_init(&s_latest.header, DATA_RECORD_GPS,
                            sizeof(s_latest), ++s_record_sequence,
                            0, 0, &timestamp);
    s_latest.protocol_sequence = protocol_sequence;
    memset(s_latest.reserved, 0, sizeof(s_latest.reserved));
    s_latest.data = *data;
    s_have_sample = true;
}

esp_err_t drone_data_clear(void)
{
    esp_err_t result = ensure_initialized();
    if (result != ESP_OK) return result;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(&s_latest, 0, sizeof(s_latest));
    s_record_sequence = 0;
    s_have_sample = false;
    xSemaphoreGive(s_lock);
    return ESP_OK;
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
    const uint8_t payload[AB_REALTIME_DATA_SIZE],
    uint8_t protocol_sequence,
    int64_t b_receive_timestamp_us)
{
    if (payload == NULL) return ESP_ERR_INVALID_ARG;
    esp_err_t result = ensure_initialized();
    if (result != ESP_OK) return result;

    drone_realtime_data_t decoded;
    if (!ab_decode_realtime_data(payload, AB_REALTIME_DATA_SIZE, &decoded)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (((decoded.valid_flags & DRONE_VALID_UTC) &&
         decoded.utc_milliseconds > 999) ||
        ((decoded.valid_flags & DRONE_VALID_BATTERY) &&
         decoded.battery_percent > 100)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    /* Section 4.3.6 forbids recording invalid fields. Preserve validity flags
     * and canonicalize unavailable values so downstream logging cannot
     * accidentally treat protocol placeholders as measurements. */
    if (!(decoded.valid_flags & DRONE_VALID_POSITION)) {
        decoded.latitude_e7 = 0;
        decoded.longitude_e7 = 0;
    }
    if (!(decoded.valid_flags & DRONE_VALID_ALTITUDE))
        decoded.altitude_relative_mm = 0;
    if (!(decoded.valid_flags & DRONE_VALID_UTC)) {
        decoded.utc_seconds = 0;
        decoded.utc_milliseconds = 0;
    }
    if (!(decoded.valid_flags & DRONE_VALID_BATTERY))
        decoded.battery_percent = 0;

    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return ESP_FAIL;
    publish(&decoded, protocol_sequence, b_receive_timestamp_us);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t drone_data_get_latest(gps_record_t *out_record)
{
    if (out_record == NULL) return ESP_ERR_INVALID_ARG;
    esp_err_t result = ensure_initialized();
    if (result != ESP_OK) return result;
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return ESP_FAIL;
    if (!s_have_sample) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    *out_record = s_latest;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}
