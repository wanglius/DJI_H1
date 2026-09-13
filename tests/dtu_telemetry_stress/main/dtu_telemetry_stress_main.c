#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "data_records.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "telemetry.h"

static const char *const TAG = "DTU_STRESS";

#define DTU_UART_PORT UART_NUM_1
#define DTU_TX_GPIO 17
#define DTU_RX_GPIO 18
#define DTU_BAUD_RATE 460800U
#define DTU_FRAGMENT_GAP_MS 0U
#define TELEMETRY_POOL_LENGTH 128U
#define ACK_TIMEOUT_MS 3000U
#define MAX_RETRIES 3U

#define TEST_START_DELAY_MS 60000U
#define TEST_DURATION_US UINT64_C(10000000)
#define REFLECTANCE_PERIOD_US UINT64_C(148000)
#define GPS_PERIOD_US UINT64_C(200000)
#define TEST_SAMPLE_COUNT 711U
#define EXPECTED_REFLECTANCE_COUNT 68U
#define EXPECTED_GPS_COUNT 50U
#define ABORT_BACKLOG_COUNT 24U
#define TEST_SESSION_ID UINT32_C(0x53545231) /* "STR1" */
#define TEST_SEGMENT_ID 1U
#define TEST_UTC_START_MS UINT64_C(1789257600000)
/* Unique to the cache-off qualification run; the host monitor filters on it
 * while continuing to ACK any older mission that arrives late. */
#define TEST_MISSION_ID UINT64_C(0x5354523209130003)

static reflectance_record_t s_reflectance;
static gps_record_t s_gps;

static uint64_t factory_source_id(void)
{
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_efuse_mac_get_default(mac));
    uint64_t source_id = 0;
    for (size_t i = 0; i < sizeof(mac); i++) {
        source_id = (source_id << 8) | mac[i];
    }
    return source_id;
}

static record_time_t fake_timestamp(uint64_t b_us, uint64_t elapsed_us)
{
    return (record_time_t) {
        .b_monotonic_us = b_us,
        .a_monotonic_ms = elapsed_us / 1000U,
        .utc_ms = TEST_UTC_START_MS + elapsed_us / 1000U,
        .sync_age_ms = 0,
        .sync_generation = 1,
        .sync_state = CLOCK_SYNC_LOCKED,
        .valid_flags = RECORD_TIME_VALID_B_MONOTONIC |
                       RECORD_TIME_VALID_A_MONOTONIC |
                       RECORD_TIME_VALID_UTC,
    };
}

static void make_reflectance(uint32_t sequence, uint64_t now_us,
                             uint64_t elapsed_us)
{
    memset(&s_reflectance, 0, sizeof(s_reflectance));
    record_time_t timestamp = fake_timestamp(now_us, elapsed_us);
    data_record_header_init(
        &s_reflectance.header, DATA_RECORD_REFLECTANCE,
        REFLECTANCE_RECORD_WIRE_SIZE(TEST_SAMPLE_COUNT), sequence,
        TEST_SESSION_ID, TEST_SEGMENT_ID, &timestamp);
    s_reflectance.calculation_count = sequence;
    s_reflectance.ground_frame_count = sequence;
    s_reflectance.sky_frame_count = sequence;
    s_reflectance.sky_b_monotonic_us = now_us - 30000U;
    s_reflectance.sky_age_us = 30000U;
    s_reflectance.sample_count = TEST_SAMPLE_COUNT;
    s_reflectance.valid_sample_count = TEST_SAMPLE_COUNT;
    s_reflectance.input_quality_flags =
        REFLECTANCE_INPUT_GROUND_VALID | REFLECTANCE_INPUT_SKY_VALID |
        REFLECTANCE_INPUT_SKY_FRESH |
        REFLECTANCE_INPUT_SAMPLE_COUNTS_MATCH;

    /* A smoothly changing full-size spectrum makes truncation/reordering
     * visible to a decoder without wasting CPU on a synthetic sensor model. */
    for (uint16_t i = 0; i < TEST_SAMPLE_COUNT; i++) {
        s_reflectance.reflectance_0p01_percent[i] =
            (uint16_t)(1800U + ((uint32_t)i * 7U + sequence * 31U) % 7201U);
        s_reflectance.sample_flags[i] = REFLECTANCE_SAMPLE_VALID;
    }
}

static void make_gps(uint32_t sequence, uint64_t now_us,
                     uint64_t elapsed_us)
{
    memset(&s_gps, 0, sizeof(s_gps));
    record_time_t timestamp = fake_timestamp(now_us, elapsed_us);
    data_record_header_init(&s_gps.header, DATA_RECORD_GPS,
                            GPS_RECORD_WIRE_SIZE, sequence, TEST_SESSION_ID,
                            TEST_SEGMENT_ID, &timestamp);
    s_gps.protocol_sequence = (uint8_t)sequence;
    s_gps.data.latitude_e7 = 312304000 + (int32_t)(sequence * 25U);
    s_gps.data.longitude_e7 = 1214737000 + (int32_t)(sequence * 30U);
    s_gps.data.altitude_relative_mm = 35000 + (int32_t)(sequence * 100U);
    s_gps.data.utc_seconds = (uint32_t)(timestamp.utc_ms / 1000U);
    s_gps.data.utc_milliseconds = (uint16_t)(timestamp.utc_ms % 1000U);
    s_gps.data.a_monotonic_ms = (uint32_t)timestamp.a_monotonic_ms;
    s_gps.data.source_flags = DRONE_SOURCE_POSITION_RTK |
                              DRONE_SOURCE_ALTITUDE_RTK;
    s_gps.data.gps_fix = 3;
    s_gps.data.rtk_solution = 2;
    s_gps.data.flight_status = 2;
    s_gps.data.display_mode = 6;
    s_gps.data.battery_percent = (uint8_t)(95U - sequence / 2U);
    s_gps.data.a_status = 1;
    s_gps.data.valid_flags = DRONE_VALID_POSITION | DRONE_VALID_ALTITUDE |
                             DRONE_VALID_UTC | DRONE_VALID_BATTERY;
}

static void print_status(const char *phase)
{
    telemetry_status_t status;
    telemetry_get_status(&status);
    uint32_t average_rtt_us = status.acknowledgements_received == 0 ? 0U :
        (uint32_t)(status.acknowledgement_rtt_sum_us /
                   status.acknowledgements_received);
    ESP_LOGI(TAG,
             "%s: healthy=%u GPS submit/ack/drop=%" PRIu32 "/%" PRIu32
             "/%" PRIu32 " REF submit/ack/drop=%" PRIu32 "/%" PRIu32
             "/%" PRIu32 " pool=%" PRIu32 "/%" PRIu32 " high=%" PRIu32
             " attempts=%" PRIu32 " fragments=%" PRIu32 " bytes=%" PRIu32,
             phase, status.healthy, status.gps_submitted, status.gps_sent,
             status.gps_queue_overflows, status.reflectance_submitted,
             status.reflectance_sent, status.reflectance_queue_overflows,
             status.pool_used, status.pool_capacity,
             status.pool_high_watermark, status.transmission_attempts,
             status.fragments_sent, status.bytes_sent);
    ESP_LOGI(TAG,
             "%s: ACK=%" PRIu32 " timeout=%" PRIu32 " retry=%" PRIu32
             " recovery=%" PRIu32
             " failed=%" PRIu32 " mismatch=%" PRIu32 " negative=%" PRIu32
             " RTT last/avg/max=%" PRIu32 "/%" PRIu32 "/%" PRIu32 " us"
             " UART errors=%" PRIu32 " drain timeouts=%" PRIu32,
              phase, status.acknowledgements_received,
              status.acknowledgement_timeouts, status.messages_retried,
              status.recovery_probes, status.messages_failed,
              status.acknowledgements_mismatched,
             status.acknowledgements_negative,
             status.acknowledgement_rtt_last_us, average_rtt_us,
             status.acknowledgement_rtt_max_us, status.uart_errors,
             status.drain_timeouts);
}

void app_main(void)
{
    ESP_LOGI(TAG, "DTU telemetry stress test: 10 s at 148 ms/full spectrum + 5 Hz GPS");
    ESP_LOGI(TAG, "UART1 GPIO17/18 at %u baud, application fragment gap=%u ms",
             DTU_BAUD_RATE, DTU_FRAGMENT_GAP_MS);
    ESP_LOGI(TAG, "DTU must already be configured for 460800 baud, 1024-byte packet limit, QoS 0 uplink");

    ESP_ERROR_CHECK(esp_psram_is_initialized() ? ESP_OK : ESP_ERR_NOT_FOUND);
    ESP_LOGI(TAG, "PSRAM detected=%u bytes free=%u bytes",
             (unsigned)esp_psram_get_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    uint64_t source_id = factory_source_id();
    const telemetry_config_t config = {
        .uart_port = DTU_UART_PORT,
        .tx_gpio = DTU_TX_GPIO,
        .rx_gpio = DTU_RX_GPIO,
        .baud_rate = DTU_BAUD_RATE,
        .fragment_gap_ms = DTU_FRAGMENT_GAP_MS,
        .source_id = source_id,
        .ack_timeout_ms = ACK_TIMEOUT_MS,
        .max_retries = MAX_RETRIES,
        .pool_length = TELEMETRY_POOL_LENGTH,
        .timing_diagnostics = true,
    };
    ESP_ERROR_CHECK(telemetry_start(&config));

    ESP_LOGI(TAG, "Start the host MQTT monitor now; generation begins in %u s",
             TEST_START_DELAY_MS / 1000U);
    vTaskDelay(pdMS_TO_TICKS(TEST_START_DELAY_MS));

    uint64_t mission_id = TEST_MISSION_ID;
    ESP_ERROR_CHECK(telemetry_begin_mission(mission_id));
    ESP_LOGI(TAG, "BEGIN mission=%016" PRIX64, mission_id);

    uint64_t started_us = (uint64_t)esp_timer_get_time();
    uint64_t next_reflectance_us = started_us;
    uint64_t next_gps_us = started_us;
    uint32_t reflectance_sequence = 0;
    uint32_t gps_sequence = 0;
    uint32_t reflectance_submit_failures = 0;
    uint32_t gps_submit_failures = 0;

    while ((uint64_t)esp_timer_get_time() - started_us < TEST_DURATION_US) {
        uint64_t now_us = (uint64_t)esp_timer_get_time();
        uint64_t elapsed_us = now_us - started_us;
        if (now_us >= next_reflectance_us) {
            make_reflectance(++reflectance_sequence, now_us, elapsed_us);
            if (telemetry_submit_reflectance(&s_reflectance) != ESP_OK) {
                reflectance_submit_failures++;
            }
            do {
                next_reflectance_us += REFLECTANCE_PERIOD_US;
            } while (next_reflectance_us <= now_us);
        }
        if (now_us >= next_gps_us) {
            make_gps(++gps_sequence, now_us, elapsed_us);
            if (telemetry_submit_gps(&s_gps) != ESP_OK) gps_submit_failures++;
            do {
                next_gps_us += GPS_PERIOD_US;
            } while (next_gps_us <= now_us);
        }
        vTaskDelay(1);
    }

    ESP_LOGI(TAG,
             "GENERATION COMPLETE: reflectance=%" PRIu32 " failed=%" PRIu32
             " GPS=%" PRIu32 " failed=%" PRIu32,
             reflectance_sequence, reflectance_submit_failures,
             gps_sequence, gps_submit_failures);
    print_status("at 10 seconds");

    /* Keep admission open while waiting so the same mission can stage a
     * deliberate shutdown backlog after its measured delivery is proven. */
    int64_t drain_deadline_us = esp_timer_get_time() + 30000000LL;
    telemetry_status_t status;
    do {
        telemetry_get_status(&status);
        if (status.pool_used == 0 &&
            status.gps_sent >= EXPECTED_GPS_COUNT &&
            status.reflectance_sent >= EXPECTED_REFLECTANCE_COUNT) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    } while (esp_timer_get_time() < drain_deadline_us);
    bool delivery_pass = status.pool_used == 0 &&
        status.gps_sent == EXPECTED_GPS_COUNT &&
        status.reflectance_sent == EXPECTED_REFLECTANCE_COUNT;
    ESP_LOGI(TAG, "DELIVERY %s: GPS=%" PRIu32 " REF=%" PRIu32
             " pool=%" PRIu32,
             delivery_pass ? "PASS" : "FAIL", status.gps_sent,
             status.reflectance_sent, status.pool_used);

    uint32_t abort_backlog_admitted = 0;
    if (delivery_pass) {
        uint64_t now_us = (uint64_t)esp_timer_get_time();
        for (uint32_t i = 0; i < ABORT_BACKLOG_COUNT; i++) {
            make_reflectance(++reflectance_sequence, now_us + i,
                             TEST_DURATION_US + i);
            if (telemetry_submit_reflectance(&s_reflectance) == ESP_OK)
                abort_backlog_admitted++;
        }
    }
    telemetry_get_status(&status);
    uint32_t pending_before_abort = status.pool_used;
    int64_t abort_started_us = esp_timer_get_time();
    esp_err_t abort_result = telemetry_abort_mission();
    int64_t abort_elapsed_us = esp_timer_get_time() - abort_started_us;

    int64_t reclaim_deadline_us = esp_timer_get_time() + 1000000LL;
    do {
        telemetry_get_status(&status);
        if (status.pool_used == 0) break;
        vTaskDelay(1);
    } while (esp_timer_get_time() < reclaim_deadline_us);
    gps_record_t rejected_gps = s_gps;
    esp_err_t post_abort_submit = telemetry_submit_gps(&rejected_gps);
    bool abort_pass = abort_result == ESP_OK && abort_backlog_admitted > 0 &&
        pending_before_abort > 0 && abort_elapsed_us < 50000 &&
        status.shutdown_aborted &&
        status.messages_abandoned_shutdown > 0 &&
        status.pool_used == 0 &&
        post_abort_submit == ESP_ERR_INVALID_STATE;
    ESP_LOGI(TAG,
             "ABORT %s: admitted=%" PRIu32 " pending=%" PRIu32
             " call=%" PRId64 "us abandoned=%" PRIu32
             " pool=%" PRIu32 " post_submit=%s",
             abort_pass ? "PASS" : "FAIL", abort_backlog_admitted,
             pending_before_abort, abort_elapsed_us,
             status.messages_abandoned_shutdown, status.pool_used,
             esp_err_to_name(post_abort_submit));
    print_status("final");
    ESP_LOGI(TAG, "TEST %s",
             delivery_pass && abort_pass ? "PASS" : "FAIL");
}
