#include "data_pipeline_test.h"

#include <string.h>

#include "data_records.h"
#include "drone_data.h"
#include "esp_check.h"
#include "esp_log.h"
#include "telemetry_transport.h"

static const char *TAG = "DATA_TEST";

#define CHECK(condition, message) do {      \
    if (!(condition)) {                    \
        ESP_LOGE(TAG, "FAIL: %s", message); \
        return ESP_FAIL;                   \
    }                                     \
} while (0)

static uint32_t read_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

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
    uint8_t payload[AB_REALTIME_DATA_SIZE] = {
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

    CHECK(record.header.record_sequence == 2, "record sequence advancement");
    CHECK(record.header.timestamp.b_monotonic_us == receive_timestamp_us,
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
    CHECK(record.header.record_sequence == 2 && record.protocol_sequence == 0x5A,
          "last valid record preservation");

    /* Invalid protocol fields may contain placeholders; they must not reject
     * valid timing/status data and must be canonicalized before recording. */
    payload[20] = 0xFF; payload[21] = 0xFF;
    payload[27] = 0xFF;
    payload[29] = DRONE_VALID_POSITION | DRONE_VALID_ALTITUDE;
    ESP_RETURN_ON_ERROR(drone_data_update_payload(
                            payload, 0x5C, receive_timestamp_us + 2),
                        TAG, "invalid-field placeholder handling");
    ESP_RETURN_ON_ERROR(drone_data_get_latest(&record), TAG,
                        "invalid-field canonical record");
    CHECK(record.data.utc_seconds == 0 &&
              record.data.utc_milliseconds == 0 &&
              record.data.battery_percent == 0,
          "invalid fields canonicalized");

    /* SD and MQTT consume the same canonical serializer. In-place renewal is
     * explicitly supported because recorder callers may retain the timestamp
     * inside the header being rebuilt. */
    const uint64_t expected_b_time = record.header.timestamp.b_monotonic_us;
    data_record_header_init(&record.header, DATA_RECORD_GPS,
                            GPS_RECORD_WIRE_SIZE, 3, 42, 3,
                            &record.header.timestamp);
    CHECK(record.header.timestamp.b_monotonic_us == expected_b_time,
          "in-place header timestamp preservation");
    uint8_t gps_wire[GPS_RECORD_WIRE_SIZE];
    size_t gps_wire_length = 0;
    CHECK(data_record_serialize_gps(
              &record, gps_wire, sizeof(gps_wire), &gps_wire_length) == ESP_OK,
          "GPS serialization");
    CHECK(gps_wire_length == GPS_RECORD_WIRE_SIZE &&
              read_le32(gps_wire) == DATA_RECORD_MAGIC &&
              read_le32(gps_wire + gps_wire_length - 4) ==
                  telemetry_crc32(gps_wire, gps_wire_length - 4),
          "GPS serialized size/magic/CRC");

    /* A full reflectance record is about 3 KiB.  Keep this boot-only test
     * fixture out of app_main's deliberately small task stack. */
    static reflectance_record_t reflectance;
    memset(&reflectance, 0, sizeof(reflectance));
    reflectance.sample_count = 3;
    reflectance.valid_sample_count = 3;
    reflectance.reflectance_0p01_percent[0] = 1000;
    reflectance.reflectance_0p01_percent[1] = 5000;
    reflectance.reflectance_0p01_percent[2] = 10000;
    reflectance.sample_flags[0] = REFLECTANCE_SAMPLE_VALID;
    reflectance.sample_flags[1] = REFLECTANCE_SAMPLE_VALID;
    reflectance.sample_flags[2] = REFLECTANCE_SAMPLE_VALID;
    data_record_header_init(
        &reflectance.header, DATA_RECORD_REFLECTANCE,
        REFLECTANCE_RECORD_WIRE_SIZE(reflectance.sample_count), 4, 42, 3,
        &record.header.timestamp);
    uint8_t reflectance_wire[REFLECTANCE_RECORD_WIRE_SIZE(3)];
    size_t reflectance_wire_length = 0;
    CHECK(data_record_serialize_reflectance(
              &reflectance, reflectance_wire, sizeof(reflectance_wire),
              &reflectance_wire_length) == ESP_OK,
          "reflectance serialization");
    CHECK(reflectance_wire_length == sizeof(reflectance_wire) &&
              read_le32(reflectance_wire + reflectance_wire_length - 4) ==
                  telemetry_crc32(reflectance_wire,
                                  reflectance_wire_length - 4),
          "reflectance serialized size/CRC");

    record_time_t raw_time = {
        .b_monotonic_us = 1000,
        .valid_flags = RECORD_TIME_VALID_B_MONOTONIC,
    };
    data_record_header_t raw_header = {0};
    data_record_header_init(&raw_header, DATA_RECORD_RAW_SPECTRUM,
                            sizeof(raw_spectrum_record_t), 7, 42, 3, &raw_time);
    CHECK(raw_header.magic == DATA_RECORD_MAGIC &&
              raw_header.format_version == 0x01 &&
              raw_header.header_size == DATA_RECORD_WIRE_HEADER_SIZE &&
              raw_header.record_type == DATA_RECORD_RAW_SPECTRUM &&
              raw_header.record_sequence == 7 && raw_header.session_id == 42 &&
              raw_header.segment_id == 3 &&
              raw_header.timestamp.b_monotonic_us == 1000,
          "raw spectral header initialization");

    static raw_spectrum_record_t raw;
    memset(&raw, 0, sizeof(raw));
    raw.sample_count = 3;
    raw.frame_count = 9;
    raw.exposure_us = 1000;
    raw.spectrometer_role = SPECTROMETER_GROUND;
    raw.frame_quality = RAW_QUALITY_VALID;
    raw.samples[0] = 11; raw.samples[1] = 22; raw.samples[2] = 33;
    data_record_header_init(&raw.header, DATA_RECORD_RAW_SPECTRUM,
                            RAW_RECORD_WIRE_SIZE(raw.sample_count), 9, 42, 3,
                            &raw_time);
    uint8_t raw_wire[RAW_RECORD_WIRE_SIZE(3)];
    size_t raw_wire_length = 0;
    CHECK(data_record_serialize_raw(&raw, raw_wire, sizeof(raw_wire),
                                    &raw_wire_length) == ESP_OK &&
              raw_wire_length == sizeof(raw_wire) &&
              read_le32(raw_wire + raw_wire_length - 4) ==
                  telemetry_crc32(raw_wire, raw_wire_length - 4),
          "raw spectrum serialized size/CRC");

    data_record_header_t reflectance_header = {0};
    data_record_header_init(&reflectance_header, DATA_RECORD_REFLECTANCE,
                            sizeof(reflectance_record_t), 8, 42, 3, &raw_time);
    CHECK(reflectance_header.record_type == DATA_RECORD_REFLECTANCE &&
              reflectance_header.record_size == sizeof(reflectance_record_t),
          "reflectance header initialization");

    ESP_LOGI(TAG, "DATA RECORD AND FAKE GPS SELF-TEST PASSED");
    return ESP_OK;
}
