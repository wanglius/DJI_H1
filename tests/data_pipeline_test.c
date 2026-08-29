#include "data_pipeline_test.h"

#include <string.h>

#include "data_records.h"
#include "drone_data.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "DATA_TEST";

#define CHECK(condition, message) do {      \
    if (!(condition)) {                    \
        ESP_LOGE(TAG, "FAIL: %s", message); \
        return ESP_FAIL;                   \
    }                                     \
} while (0)

esp_err_t data_pipeline_self_test(void)
{
    gps_record_t record;
    ESP_RETURN_ON_ERROR(drone_data_init_fake(), TAG, "Fake source init failed");
    ESP_RETURN_ON_ERROR(drone_data_get_latest(&record), TAG,
                        "Could not read fake record");
    CHECK(record.header.format_version == DATA_RECORD_FORMAT_VERSION,
          "record format version");
    CHECK(record.header.record_type == DATA_RECORD_GPS, "GPS record type");
    CHECK(record.header.record_size == sizeof(gps_record_t), "GPS record size");
    CHECK(memcmp(&record.data, &DRONE_DATA_FAKE_FIXED,
                 sizeof(record.data)) == 0, "fixed GPS contents");

    /* Explicit CMD 0x01 test vector from the documented field layout. */
    uint8_t payload[DRONE_REALTIME_PAYLOAD_SIZE] = {
        0xD0, 0xE5, 0xC8, 0x17, /* latitude 399042000 */
        0x10, 0x5C, 0x62, 0x45, /* longitude 1164074000 */
        0xC0, 0xD4, 0x01, 0x00, /* relative altitude 120000 mm */
        0x00, 0xB9, 0x55, 0x69, /* UTC seconds 1767225600 */
        0x40, 0xE2, 0x01, 0x00, /* A monotonic time 123456 ms */
        0xF4, 0x01,             /* UTC milliseconds 500 */
        0x03,                   /* position and altitude from RTK */
        0x03,                   /* 3D GPS fix */
        0x32,                   /* RTK fixed solution (50) */
        0x02,                   /* airborne */
        0x0F,                   /* return-to-home display mode */
        0x55,                   /* battery 85% */
        0x1F,                   /* A-board status bits 0..4 valid */
        0x0F,                   /* all documented data valid */
    };
    const int64_t receive_timestamp_us = 987654321;
    ESP_RETURN_ON_ERROR(drone_data_update_payload(
                            payload, 0x5A, receive_timestamp_us),
                        TAG, "Protocol-vector decode failed");
    ESP_RETURN_ON_ERROR(drone_data_get_latest(&record), TAG,
                        "Could not read decoded record");

    CHECK(record.header.sequence == 2, "record sequence advancement");
    CHECK(record.header.b_timestamp_us == receive_timestamp_us,
          "B-board receive timestamp");
    CHECK(record.protocol_sequence == 0x5A, "protocol sequence");
    CHECK(record.data.latitude_e7 == 399042000, "latitude decode");
    CHECK(record.data.longitude_e7 == 1164074000, "longitude decode");
    CHECK(record.data.altitude_relative_mm == 120000, "altitude decode");
    CHECK(record.data.utc_seconds == 1767225600, "UTC seconds decode");
    CHECK(record.data.a_monotonic_ms == 123456, "monotonic time decode");
    CHECK(record.data.utc_milliseconds == 500, "UTC milliseconds decode");
    CHECK(record.data.source_flags == 0x03, "source flags decode");
    CHECK(record.data.gps_fix == 3, "GPS fix decode");
    CHECK(record.data.rtk_solution == 50, "RTK solution decode");
    CHECK(record.data.flight_status == 2, "flight status decode");
    CHECK(record.data.display_mode == 15, "display mode decode");
    CHECK(record.data.battery_percent == 85, "battery decode");
    CHECK(record.data.a_status == 0x1F, "A-board status decode");
    CHECK(record.data.valid_flags == 0x0F, "valid flags decode");

    payload[27] = 101;
    CHECK(drone_data_update_payload(payload, 0x5B, receive_timestamp_us + 1) ==
              ESP_ERR_INVALID_RESPONSE,
          "invalid battery rejection");
    ESP_RETURN_ON_ERROR(drone_data_get_latest(&record), TAG,
                        "Could not reread latest record");
    CHECK(record.header.sequence == 2 && record.protocol_sequence == 0x5A,
          "last valid record preservation");

    data_record_header_t raw_header = {0};
    data_record_header_init(&raw_header, DATA_RECORD_RAW_SPECTRAL_PAIR,
                            sizeof(raw_spectral_record_t), 7, 42, 1000);
    CHECK(raw_header.record_type == DATA_RECORD_RAW_SPECTRAL_PAIR &&
              raw_header.sequence == 7 && raw_header.session_id == 42,
          "raw spectral header initialization");

    data_record_header_t reflectance_header = {0};
    data_record_header_init(&reflectance_header, DATA_RECORD_REFLECTANCE,
                            sizeof(reflectance_record_t), 8, 42, 1100);
    CHECK(reflectance_header.record_type == DATA_RECORD_REFLECTANCE &&
              reflectance_header.record_size == sizeof(reflectance_record_t),
          "reflectance header initialization");

    ESP_LOGI(TAG, "DATA RECORD AND FAKE GPS SELF-TEST PASSED");
    return ESP_OK;
}
