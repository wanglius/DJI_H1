#include "measurement_recorder.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "calculation.h"
#include "clock_sync.h"
#include "data_records.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "sd_card.h"

static const char *TAG = "MEAS_REC";

#define RAW_POOL_COUNT 24
#define WRITER_QUEUE_LENGTH 64
#define SERIAL_BUFFER_SIZE 4096
#define FILE_HEADER_SIZE 16U
#define SKY_HISTORY_COUNT 8U
#define PERIODIC_FLUSH_MS 1500U

typedef enum {
    MSG_RAW, MSG_GPS, MSG_EVENT, MSG_CHECKPOINT, MSG_BARRIER, MSG_FINALIZE
} message_type_t;
typedef struct {
    measurement_event_t code;
    uint32_t sequence;
    uint32_t session_id;
    uint16_t segment_id;
    uint16_t reserved;
    uint32_t argument0;
    int32_t argument1;
    record_time_t timestamp;
} operation_event_t;
typedef struct {
    message_type_t type;
    union {
        raw_spectrum_record_t *raw;
        gps_record_t gps;
        operation_event_t event;
    } data;
} message_t;

typedef struct {
    uint32_t segments_completed;
    uint32_t raw_written;
    uint32_t reflectance_written;
    uint32_t gps_written;
    uint32_t events_written;
    uint32_t raw_dropped;
    uint32_t gps_dropped;
    uint32_t events_dropped;
    uint32_t calculation_rejected;
    uint32_t write_errors;
    uint32_t flush_errors;
    uint32_t max_flush_us;
} mission_totals_t;

static raw_spectrum_record_t s_pool[RAW_POOL_COUNT];
static QueueHandle_t s_free_queue, s_writer_queue;
static SemaphoreHandle_t s_barrier;
static TaskHandle_t s_task;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static measurement_recorder_status_t s_status = {.healthy = true};
static uint32_t s_session, s_raw_sequence, s_calculation_sequence;
static uint32_t s_gps_sequence, s_event_sequence;
static uint16_t s_segment;
static uint32_t s_flight_index;
static bool s_have_flight, s_accept_aux;
/* Lifecycle transitions retain their progress after a timeout. A retry waits
 * for the already-enqueued marker instead of closing files underneath it. */
static bool s_end_pending, s_end_barrier_queued;
static bool s_shutdown_pending, s_finalize_queued;
static uint32_t s_aux_submitters;
static uint32_t s_raw_submitters;
static char s_directory[24];
static sd_card_file_t *s_raw_file, *s_reflectance_file;
static sd_card_file_t *s_gps_file, *s_event_file;
static mission_totals_t s_totals;
static record_time_t s_flight_started;
static uint8_t s_drone_serial[32];
static uint16_t s_a_firmware_version;
static uint8_t s_drone_link;
/* Sole writer-task workspaces live in BSS to keep its stack bounded. */
static uint8_t s_serial_buffer[SERIAL_BUFFER_SIZE];
static raw_spectrum_record_t s_sky_history[SKY_HISTORY_COUNT];
static reflectance_record_t s_calculated;
#if CONFIG_DJI_H1_TEST_FAULT_INJECTION && \
    CONFIG_DJI_H1_TEST_ONESHOT_FLUSH_STALL_MS > 0
static bool s_test_stall_done;
#endif

static esp_err_t close_files(void);
static esp_err_t open_flight_files(void);

static void remember_sky(const raw_spectrum_record_t *sky,
                         size_t *next, size_t *count)
{
    s_sky_history[*next] = *sky;
    *next = (*next + 1U) % SKY_HISTORY_COUNT;
    if (*count < SKY_HISTORY_COUNT) (*count)++;
}

static const raw_spectrum_record_t *find_sky_for_ground(
    const raw_spectrum_record_t *ground, size_t count)
{
    const raw_spectrum_record_t *best = NULL;
    uint64_t ground_us = ground->header.timestamp.b_monotonic_us;
    for (size_t i = 0; i < count; i++) {
        const raw_spectrum_record_t *candidate = &s_sky_history[i];
        uint64_t candidate_us = candidate->header.timestamp.b_monotonic_us;
        if (candidate->header.session_id == ground->header.session_id &&
            candidate->header.segment_id == ground->header.segment_id &&
            candidate_us <= ground_us &&
            (best == NULL || candidate_us >
                             best->header.timestamp.b_monotonic_us)) {
            best = candidate;
        }
    }
    return best;
}

static void put16(uint8_t **p, uint16_t v)
{ (*p)[0] = v; (*p)[1] = v >> 8; *p += 2; }
static void put32(uint8_t **p, uint32_t v)
{ for (unsigned i = 0; i < 4; i++) (*p)[i] = v >> (8 * i); *p += 4; }
static void put64(uint8_t **p, uint64_t v)
{ for (unsigned i = 0; i < 8; i++) (*p)[i] = v >> (8 * i); *p += 8; }

static uint32_t crc32(const uint8_t *data, size_t length)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xEDB88320U & (uint32_t)-(int32_t)(crc & 1U));
    }
    return ~crc;
}

static void serialize_time(uint8_t **p, const record_time_t *t)
{
    put64(p, t->b_monotonic_us); put64(p, t->a_monotonic_ms);
    put64(p, t->utc_ms); put32(p, t->sync_age_ms);
    put16(p, t->sync_generation); *(*p)++ = t->sync_state;
    *(*p)++ = t->valid_flags;
}

static void serialize_header(uint8_t **p, const measurement_record_header_t *h,
                             uint32_t wire_size)
{
    put32(p, DATA_RECORD_MAGIC); put16(p, DATA_RECORD_FORMAT_VERSION);
    put16(p, h->record_type); put32(p, DATA_RECORD_WIRE_HEADER_SIZE);
    put32(p, wire_size); put32(p, h->session_id); put16(p, h->segment_id);
    put16(p, h->flags); put32(p, h->record_sequence);
    serialize_time(p, &h->timestamp);
}

static esp_err_t write_block(sd_card_file_t *file, uint8_t *buffer, size_t length)
{
    uint8_t *tail = buffer + length;
    put32(&tail, crc32(buffer, length));
    size_t written = 0;
    return sd_card_file_write(file, buffer, length + 4, &written);
}

static esp_err_t write_raw(const raw_spectrum_record_t *r)
{
    uint8_t *p = s_serial_buffer;
    uint32_t size = RAW_RECORD_WIRE_SIZE(r->sample_count);
    serialize_header(&p, &r->header, size);
    put32(&p, r->frame_count); put32(&p, r->exposure_us);
    put16(&p, r->sample_count); put16(&p, (uint16_t)r->spectrum_scale);
    *p++ = r->spectrometer_role; *p++ = r->exposure_status;
    *p++ = r->frame_quality; *p++ = 0;
    for (uint16_t i = 0; i < r->sample_count; i++) put16(&p, r->samples[i]);
    return write_block(s_raw_file, s_serial_buffer,
                       (size_t)(p - s_serial_buffer));
}

static esp_err_t write_reflectance(const reflectance_record_t *r)
{
    uint8_t *p = s_serial_buffer;
    uint32_t size = REFLECTANCE_RECORD_WIRE_SIZE(r->sample_count);
    serialize_header(&p, &r->header, size);
    put32(&p, r->calculation_count); put32(&p, r->ground_frame_count);
    put32(&p, r->sky_frame_count); put64(&p, r->sky_b_monotonic_us);
    put32(&p, r->sky_age_us); put16(&p, r->sample_count);
    put16(&p, r->valid_sample_count); put16(&p, r->clamped_low_count);
    put16(&p, r->clamped_high_count); put16(&p, r->invalid_denominator_count);
    put16(&p, r->input_quality_flags);
    for (uint16_t i = 0; i < r->sample_count; i++)
        put16(&p, r->reflectance_0p01_percent[i]);
    memcpy(p, r->sample_flags, r->sample_count); p += r->sample_count;
    return write_block(s_reflectance_file, s_serial_buffer,
                       (size_t)(p - s_serial_buffer));
}

static esp_err_t write_gps(const gps_record_t *r)
{
    uint8_t *p = s_serial_buffer;
    serialize_header(&p, &r->header, GPS_RECORD_WIRE_SIZE);
    *p++ = r->protocol_sequence;
    *p++ = 0; *p++ = 0; *p++ = 0;
    put32(&p, (uint32_t)r->data.latitude_e7);
    put32(&p, (uint32_t)r->data.longitude_e7);
    put32(&p, (uint32_t)r->data.altitude_relative_mm);
    put32(&p, r->data.utc_seconds);
    put32(&p, r->data.a_monotonic_ms);
    put16(&p, r->data.utc_milliseconds);
    *p++ = r->data.source_flags;
    *p++ = r->data.gps_fix;
    *p++ = r->data.rtk_solution;
    *p++ = r->data.flight_status;
    *p++ = r->data.display_mode;
    *p++ = r->data.battery_percent;
    *p++ = r->data.a_status;
    *p++ = r->data.valid_flags;
    return write_block(s_gps_file, s_serial_buffer,
                       (size_t)(p - s_serial_buffer));
}

static const char *event_name(measurement_event_t event)
{
    switch (event) {
    case MEASUREMENT_EVENT_HANDSHAKE: return "handshake";
    case MEASUREMENT_EVENT_SEGMENT_START: return "segment_start";
    case MEASUREMENT_EVENT_STOP_REQUEST: return "stop_request";
    case MEASUREMENT_EVENT_SEGMENT_END: return "segment_end";
    case MEASUREMENT_EVENT_POWER_OFF_REQUEST: return "power_off_request";
    case MEASUREMENT_EVENT_PROTOCOL_CRC_ERROR: return "protocol_crc_error";
    case MEASUREMENT_EVENT_PROTOCOL_TIMEOUT: return "protocol_timeout";
    case MEASUREMENT_EVENT_CLOCK_OBSERVATION_DROP: return "clock_observation_drop";
    case MEASUREMENT_EVENT_REFLECTANCE_REJECTED: return "reflectance_rejected";
    case MEASUREMENT_EVENT_CAPTURE_RESULT: return "capture_result";
    case MEASUREMENT_EVENT_FLIGHT_CLOSED: return "flight_closed";
    default: return "unknown";
    }
}

static esp_err_t write_event(const operation_event_t *event)
{
    int length = snprintf((char *)s_serial_buffer, sizeof(s_serial_buffer),
        "{\"schema_version\":1,\"sequence\":%" PRIu32
        ",\"event\":\"%s\",\"session_id\":%" PRIu32
        ",\"segment_id\":%u,\"b_monotonic_us\":%" PRIu64
        ",\"utc_ms\":%" PRIu64 ",\"sync_state\":%u"
        ",\"time_valid_flags\":%u,\"argument0\":%" PRIu32
        ",\"argument1\":%" PRId32 "}\n",
        event->sequence, event_name(event->code), event->session_id,
        event->segment_id, event->timestamp.b_monotonic_us,
        event->timestamp.utc_ms, event->timestamp.sync_state,
        event->timestamp.valid_flags, event->argument0, event->argument1);
    if (length <= 0 || (size_t)length >= sizeof(s_serial_buffer))
        return ESP_ERR_INVALID_SIZE;
    size_t written = 0;
    return sd_card_file_write(s_event_file, s_serial_buffer,
                              (size_t)length, &written);
}

static void serial_strings(char text[33], char hex_output[65])
{
    static const char hex[] = "0123456789ABCDEF";
    size_t text_length = 0;
    for (size_t i = 0; i < sizeof(s_drone_serial); i++) {
        uint8_t value = s_drone_serial[i];
        hex_output[i * 2] = hex[value >> 4];
        hex_output[i * 2 + 1] = hex[value & 0x0F];
        if (value != 0 && text_length < 32) {
            bool safe = (value >= 'A' && value <= 'Z') ||
                        (value >= 'a' && value <= 'z') ||
                        (value >= '0' && value <= '9') ||
                        value == '-' || value == '_' || value == '.';
            text[text_length++] = safe ? (char)value : '_';
        }
    }
    text[text_length] = '\0';
    hex_output[64] = '\0';
}

static esp_err_t write_mission_summary(const char *state)
{
    mission_totals_t totals;
    record_time_t flight_started, updated;
    uint8_t drone_link;
    uint16_t a_firmware;
    char drone_serial_text[33];
    char drone_serial_hex[65];
    taskENTER_CRITICAL(&s_lock);
    totals = s_totals;
    drone_link = s_drone_link;
    a_firmware = s_a_firmware_version;
    serial_strings(drone_serial_text, drone_serial_hex);
    flight_started = s_flight_started;
    taskEXIT_CRITICAL(&s_lock);
    /* The directory is created before A time is available. Re-project its
     * stable B timestamp once the clock model has locked so the final summary
     * can still contain an absolute mission-start estimate. */
    (void)clock_sync_timestamp((int64_t)flight_started.b_monotonic_us,
                               &flight_started);
    (void)clock_sync_timestamp(esp_timer_get_time(), &updated);
    const esp_app_desc_t *app = esp_app_get_description();
    int length = snprintf((char *)s_serial_buffer, sizeof(s_serial_buffer),
        "{\n"
        "  \"schema\": \"DJI_H1_MISSION\",\n"
        "  \"schema_version\": 1,\n"
        "  \"record_format_version\": %u,\n"
        "  \"state\": \"%s\",\n"
        "  \"directory\": \"%s\",\n"
        "  \"flight_index\": %" PRIu32 ",\n"
        "  \"firmware_version\": \"%s\",\n"
        "  \"a_firmware_version\": %u,\n"
        "  \"drone_link\": %u,\n"
        "  \"drone_serial\": \"%s\",\n"
        "  \"drone_serial_hex\": \"%s\",\n"
        "  \"files\": {\"raw\": \"RAW_SPECTRA.BIN\", "
        "\"reflectance\": \"REFLECTANCE.BIN\", \"gps\": \"GPS_TRACK.BIN\", "
        "\"events\": \"EVENTS.JSONL\"},\n"
        "  \"started_b_monotonic_us\": %" PRIu64 ",\n"
        "  \"started_utc_ms\": %" PRIu64 ",\n"
        "  \"started_time_valid_flags\": %u,\n"
        "  \"updated_b_monotonic_us\": %" PRIu64 ",\n"
        "  \"updated_utc_ms\": %" PRIu64 ",\n"
        "  \"updated_time_valid_flags\": %u,\n"
        "  \"segments_completed\": %" PRIu32 ",\n"
        "  \"records\": {\"raw\": %" PRIu32 ", \"reflectance\": %" PRIu32
        ", \"gps\": %" PRIu32 ", \"events\": %" PRIu32 "},\n"
        "  \"drops\": {\"raw\": %" PRIu32 ", \"gps\": %" PRIu32
        ", \"events\": %" PRIu32 "},\n"
        "  \"calculation_rejected\": %" PRIu32 ",\n"
        "  \"write_errors\": %" PRIu32 ",\n"
        "  \"flush_errors\": %" PRIu32 ",\n"
        "  \"max_flush_us\": %" PRIu32 "\n"
        "}\n",
        DATA_RECORD_FORMAT_VERSION, state, s_directory, s_flight_index,
        app ? app->version : "unknown", a_firmware, drone_link,
        drone_serial_text, drone_serial_hex, flight_started.b_monotonic_us,
        flight_started.utc_ms, flight_started.valid_flags,
        updated.b_monotonic_us, updated.utc_ms, updated.valid_flags,
        totals.segments_completed, totals.raw_written,
        totals.reflectance_written, totals.gps_written, totals.events_written,
        totals.raw_dropped, totals.gps_dropped, totals.events_dropped,
        totals.calculation_rejected, totals.write_errors,
        totals.flush_errors, totals.max_flush_us);
    if (length <= 0 || (size_t)length >= sizeof(s_serial_buffer))
        return ESP_ERR_INVALID_SIZE;

    char temporary[56], target[56], backup[56];
    snprintf(temporary, sizeof(temporary), "%s/MISSION.TMP", s_directory);
    snprintf(target, sizeof(target), "%s/MISSION.JSON", s_directory);
    snprintf(backup, sizeof(backup), "%s/MISSION.BAK", s_directory);
    sd_card_file_t *file = NULL;
    esp_err_t result = sd_card_file_open(temporary, "wb", &file);
    size_t written = 0;
    if (result == ESP_OK)
        result = sd_card_file_write(file, s_serial_buffer, (size_t)length, &written);
    if (result == ESP_OK) result = sd_card_file_flush(file);
    if (file != NULL) {
        esp_err_t close_result = sd_card_file_close(file);
        if (result == ESP_OK) result = close_result;
    }
    if (result == ESP_OK)
        result = sd_card_replace_file(temporary, target, backup);
    return result;
}

static esp_err_t write_file_header(sd_card_file_t *file, data_record_type_t type)
{
    uint8_t bytes[FILE_HEADER_SIZE], *p = bytes;
    put32(&p, DATA_FILE_MAGIC); put16(&p, DATA_RECORD_FORMAT_VERSION);
    put16(&p, (uint16_t)type); put32(&p, FILE_HEADER_SIZE); put32(&p, 0);
    size_t written;
    return sd_card_file_write(file, bytes, sizeof(bytes), &written);
}

static void note_write_result(esp_err_t result, bool reflectance)
{
    taskENTER_CRITICAL(&s_lock);
    if (result == ESP_OK) {
        if (reflectance) {
            s_status.reflectance_written++;
            s_totals.reflectance_written++;
        } else {
            s_status.raw_written++;
            s_totals.raw_written++;
        }
    } else {
        s_status.write_errors++;
        s_totals.write_errors++;
        s_status.healthy = false;
    }
    taskEXIT_CRITICAL(&s_lock);
}

static void note_aux_write(esp_err_t result, bool gps)
{
    taskENTER_CRITICAL(&s_lock);
    if (result == ESP_OK) {
        if (gps) {
            s_status.gps_written++;
            s_totals.gps_written++;
        } else {
            s_status.events_written++;
            s_totals.events_written++;
        }
    } else {
        s_status.write_errors++;
        s_totals.write_errors++;
        s_status.healthy = false;
    }
    taskEXIT_CRITICAL(&s_lock);
}

static void note_queue_depth(void)
{
    UBaseType_t depth = uxQueueMessagesWaiting(s_writer_queue);
    taskENTER_CRITICAL(&s_lock);
    if (depth > s_status.queue_high_watermark)
        s_status.queue_high_watermark = depth;
    taskEXIT_CRITICAL(&s_lock);
}

static void note_storage_result(esp_err_t result, const char *operation)
{
    if (result == ESP_OK) return;
    taskENTER_CRITICAL(&s_lock);
    s_status.write_errors++;
    s_totals.write_errors++;
    s_status.healthy = false;
    taskEXIT_CRITICAL(&s_lock);
    ESP_LOGE(TAG, "%s failed: %s", operation, esp_err_to_name(result));
}

static esp_err_t flush_files(void)
{
    int64_t started_us = esp_timer_get_time();
#if CONFIG_DJI_H1_TEST_FAULT_INJECTION && \
    CONFIG_DJI_H1_TEST_ONESHOT_FLUSH_STALL_MS > 0
    if (!s_test_stall_done) {
        s_test_stall_done = true;
        ESP_LOGW(TAG, "TEST ONLY: injecting %d ms one-shot flush stall",
                 CONFIG_DJI_H1_TEST_ONESHOT_FLUSH_STALL_MS);
        vTaskDelay(pdMS_TO_TICKS(CONFIG_DJI_H1_TEST_ONESHOT_FLUSH_STALL_MS));
    }
#endif
    esp_err_t result = s_raw_file ? sd_card_file_flush(s_raw_file) : ESP_OK;
    esp_err_t second = s_reflectance_file
        ? sd_card_file_flush(s_reflectance_file) : ESP_OK;
    if (result == ESP_OK) result = second;
    second = s_gps_file ? sd_card_file_flush(s_gps_file) : ESP_OK;
    if (result == ESP_OK) result = second;
    second = s_event_file ? sd_card_file_flush(s_event_file) : ESP_OK;
    if (result == ESP_OK) result = second;
    uint32_t elapsed_us = (uint32_t)(esp_timer_get_time() - started_us);

    taskENTER_CRITICAL(&s_lock);
    s_status.flush_count++;
    if (elapsed_us > s_status.max_flush_us) s_status.max_flush_us = elapsed_us;
    if (elapsed_us > s_totals.max_flush_us) s_totals.max_flush_us = elapsed_us;
    if (result != ESP_OK) {
        s_status.flush_errors++;
        s_status.write_errors++;
        s_totals.flush_errors++;
        s_totals.write_errors++;
        s_status.healthy = false;
    }
    taskEXIT_CRITICAL(&s_lock);
    if (result != ESP_OK)
        ESP_LOGE(TAG, "SD flush failed after %lu us: %s",
                 (unsigned long)elapsed_us, esp_err_to_name(result));
    return result;
}

static operation_event_t make_event(measurement_event_t code,
                                    uint32_t argument0,
                                    int32_t argument1)
{
    operation_event_t event = {
        .code = code,
        .argument0 = argument0,
        .argument1 = argument1,
    };
    taskENTER_CRITICAL(&s_lock);
    event.sequence = ++s_event_sequence;
    event.session_id = s_session;
    event.segment_id = s_segment;
    taskEXIT_CRITICAL(&s_lock);
    (void)clock_sync_timestamp(esp_timer_get_time(), &event.timestamp);
    return event;
}

static void write_internal_event(measurement_event_t code,
                                 uint32_t argument0,
                                 int32_t argument1)
{
    operation_event_t event = make_event(code, argument0, argument1);
    note_aux_write(write_event(&event), false);
}

static void writer_task(void *unused)
{
    (void)unused;
    size_t sky_next = 0, sky_count = 0;
    TickType_t last_flush = xTaskGetTickCount();
    message_t message;
    while (true) {
        if (xQueueReceive(s_writer_queue, &message,
                          pdMS_TO_TICKS(PERIODIC_FLUSH_MS)) != pdTRUE) {
            if (s_raw_file || s_reflectance_file || s_gps_file || s_event_file) {
                (void)flush_files();
                last_flush = xTaskGetTickCount();
            }
            continue;
        }
        if (message.type == MSG_BARRIER) {
            write_internal_event(MEASUREMENT_EVENT_SEGMENT_END,
                                 s_status.raw_written,
                                 (int32_t)s_status.reflectance_written);
            taskENTER_CRITICAL(&s_lock);
            s_totals.segments_completed++;
            taskEXIT_CRITICAL(&s_lock);
            (void)flush_files();
            note_storage_result(write_mission_summary("in_progress"),
                                "MISSION.JSON checkpoint");
            last_flush = xTaskGetTickCount();
            /* A barrier terminates a capture segment. Never pair a later
             * ground frame with a reference retained across that boundary. */
            sky_next = sky_count = 0;
            xSemaphoreGive(s_barrier);
            continue;
        }
        if (message.type == MSG_FINALIZE) {
            write_internal_event(MEASUREMENT_EVENT_FLIGHT_CLOSED,
                                 s_totals.segments_completed, 0);
            (void)flush_files();
            xSemaphoreGive(s_barrier);
            continue;
        }
        if (message.type == MSG_GPS) {
            note_aux_write(write_gps(&message.data.gps), true);
            goto periodic_flush;
        }
        if (message.type == MSG_EVENT) {
            note_aux_write(write_event(&message.data.event), false);
            goto periodic_flush;
        }
        if (message.type == MSG_CHECKPOINT) {
            /* The preceding handshake event and its identity become durable
             * before the first capture, even if power is lost in idle flight. */
            (void)flush_files();
            note_storage_result(write_mission_summary("in_progress"),
                                "Handshake MISSION.JSON checkpoint");
            last_flush = xTaskGetTickCount();
            continue;
        }
        raw_spectrum_record_t *raw = message.data.raw;
        esp_err_t result = write_raw(raw);
        note_write_result(result, false);
        if (result == ESP_OK && raw->spectrometer_role == SPECTROMETER_SKY) {
            /* Producers run independently, so queue arrival order is not
             * guaranteed to match acquisition timestamps. Keep enough recent
             * references to find the true predecessor of each ground frame. */
            remember_sky(raw, &sky_next, &sky_count);
        } else if (result == ESP_OK &&
                   raw->spectrometer_role == SPECTROMETER_GROUND) {
            const raw_spectrum_record_t *sky =
                find_sky_for_ground(raw, sky_count);
            if (sky == NULL) {
                ESP_LOGW(TAG, "No causal sky for ground frame=%lu t=%llu",
                         (unsigned long)raw->frame_count,
                         (unsigned long long)raw->header.timestamp.b_monotonic_us);
                taskENTER_CRITICAL(&s_lock);
                s_status.calculation_rejected++;
                s_totals.calculation_rejected++;
                taskEXIT_CRITICAL(&s_lock);
                xQueueSend(s_free_queue, &raw, portMAX_DELAY);
                continue;
            }
            uint32_t count = ++s_calculation_sequence;
            result = calculation_reflectance(raw, sky, count,
                                             &s_calculated);
            if (result == ESP_OK) {
                note_write_result(write_reflectance(&s_calculated), true);
            } else {
                ESP_LOGW(TAG, "Reflectance rejected ground=%lu sky=%lu "
                              "ground_t=%llu sky_t=%llu age=%lldus "
                              "scales=%d/%d: %s",
                         (unsigned long)raw->frame_count,
                         (unsigned long)sky->frame_count,
                         (unsigned long long)raw->header.timestamp.b_monotonic_us,
                         (unsigned long long)sky->header.timestamp.b_monotonic_us,
                         (long long)(raw->header.timestamp.b_monotonic_us -
                                     sky->header.timestamp.b_monotonic_us),
                         (int)raw->spectrum_scale, (int)sky->spectrum_scale,
                         esp_err_to_name(result));
                taskENTER_CRITICAL(&s_lock);
                s_status.calculation_rejected++;
                s_totals.calculation_rejected++;
                taskEXIT_CRITICAL(&s_lock);
                if (s_status.calculation_rejected == 1 ||
                    s_status.calculation_rejected % 100 == 0) {
                    write_internal_event(MEASUREMENT_EVENT_REFLECTANCE_REJECTED,
                                         raw->frame_count,
                                         (int32_t)(raw->header.timestamp.b_monotonic_us -
                                                   sky->header.timestamp.b_monotonic_us));
                }
            }
        }
        xQueueSend(s_free_queue, &raw, portMAX_DELAY);
periodic_flush:
        if (xTaskGetTickCount() - last_flush >=
            pdMS_TO_TICKS(PERIODIC_FLUSH_MS)) {
            (void)flush_files();
            last_flush = xTaskGetTickCount();
        }
    }
}

esp_err_t measurement_recorder_init(void)
{
    if (s_task != NULL) return ESP_ERR_INVALID_STATE;
#if CONFIG_DJI_H1_TEST_FAULT_INJECTION && \
    CONFIG_DJI_H1_TEST_ONESHOT_FLUSH_STALL_MS > 0
    ESP_LOGW(TAG, "TEST BUILD: recorder flush fault injection enabled (%d ms)",
             CONFIG_DJI_H1_TEST_ONESHOT_FLUSH_STALL_MS);
#endif
    s_free_queue = xQueueCreate(RAW_POOL_COUNT, sizeof(raw_spectrum_record_t *));
    s_writer_queue = xQueueCreate(WRITER_QUEUE_LENGTH, sizeof(message_t));
    s_barrier = xSemaphoreCreateBinary();
    if (!s_free_queue || !s_writer_queue || !s_barrier) return ESP_ERR_NO_MEM;
    for (size_t i = 0; i < RAW_POOL_COUNT; i++) {
        raw_spectrum_record_t *item = &s_pool[i];
        xQueueSend(s_free_queue, &item, 0);
    }
    taskENTER_CRITICAL(&s_lock);
    memset(&s_status, 0, sizeof(s_status));
    s_status.healthy = true;
    s_have_flight = s_accept_aux = false;
    s_end_pending = s_end_barrier_queued = false;
    s_shutdown_pending = s_finalize_queued = false;
    taskEXIT_CRITICAL(&s_lock);
    esp_err_t result = open_flight_files();
    if (result != ESP_OK) {
        taskENTER_CRITICAL(&s_lock);
        s_status.healthy = false;
        taskEXIT_CRITICAL(&s_lock);
        return result;
    }
    if (xTaskCreatePinnedToCore(writer_task, "measurement_writer", 8192, NULL,
                                4, &s_task, 0) != pdPASS) {
        taskENTER_CRITICAL(&s_lock);
        s_have_flight = s_accept_aux = false;
        s_status.healthy = false;
        taskEXIT_CRITICAL(&s_lock);
        (void)close_files();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t close_files(void)
{
    esp_err_t result = ESP_OK;
    if (s_raw_file) {
        result = sd_card_file_close(s_raw_file);
        s_raw_file = NULL;
    }
    if (s_reflectance_file) {
        esp_err_t second = sd_card_file_close(s_reflectance_file);
        if (result == ESP_OK) result = second;
        s_reflectance_file = NULL;
    }
    if (s_gps_file) {
        esp_err_t second = sd_card_file_close(s_gps_file);
        if (result == ESP_OK) result = second;
        s_gps_file = NULL;
    }
    if (s_event_file) {
        esp_err_t second = sd_card_file_close(s_event_file);
        if (result == ESP_OK) result = second;
        s_event_file = NULL;
    }
    return result;
}

static esp_err_t open_flight_files(void)
{
    esp_err_t result = close_files();
    if (result != ESP_OK) return result;

    /* session_id belongs to A and is opaque to B (protocol section 4.9).
     * Allocate a card-local mission number instead, once per B-board boot. */
    char path[56];
    bool exists = false;
    uint32_t flight_index;
    for (flight_index = 1; flight_index <= 9999; flight_index++) {
        snprintf(s_directory, sizeof(s_directory), "F_%04" PRIu32,
                 flight_index);
        result = sd_card_path_exists(s_directory, &exists);
        if (result != ESP_OK) return result;
        if (!exists) break;
    }
    if (exists) return ESP_ERR_NOT_FOUND;
    result = sd_card_mkdir(s_directory);
    if (result != ESP_OK) return result;

    snprintf(path, sizeof(path), "%s/RAW_SPECTRA.BIN", s_directory);
    result = sd_card_file_open(path, "wb", &s_raw_file);
    if (result == ESP_OK) {
        snprintf(path, sizeof(path), "%s/REFLECTANCE.BIN", s_directory);
        result = sd_card_file_open(path, "wb", &s_reflectance_file);
    }
    if (result == ESP_OK) {
        snprintf(path, sizeof(path), "%s/GPS_TRACK.BIN", s_directory);
        result = sd_card_file_open(path, "wb", &s_gps_file);
    }
    if (result == ESP_OK) {
        snprintf(path, sizeof(path), "%s/EVENTS.JSONL", s_directory);
        result = sd_card_file_open(path, "wb", &s_event_file);
    }
    if (result == ESP_OK)
        result = write_file_header(s_raw_file, DATA_RECORD_RAW_SPECTRUM);
    if (result == ESP_OK)
        result = write_file_header(s_reflectance_file, DATA_RECORD_REFLECTANCE);
    if (result == ESP_OK)
        result = write_file_header(s_gps_file, DATA_RECORD_GPS);
    if (result != ESP_OK) {
        (void)close_files();
        return result;
    }

    record_time_t flight_started;
    (void)clock_sync_timestamp(esp_timer_get_time(), &flight_started);
    taskENTER_CRITICAL(&s_lock);
    s_flight_index = flight_index;
    s_session = 0;
    s_segment = 0;
    s_raw_sequence = s_calculation_sequence = 0;
    s_gps_sequence = s_event_sequence = 0;
    memset(&s_totals, 0, sizeof(s_totals));
    s_flight_started = flight_started;
    taskEXIT_CRITICAL(&s_lock);
    result = write_mission_summary("in_progress");
    if (result != ESP_OK) {
        (void)close_files();
        return result;
    }
    taskENTER_CRITICAL(&s_lock);
    s_have_flight = true;
    s_accept_aux = true;
    taskEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "Mission directory %s ready before A-board link", s_directory);
    return ESP_OK;
}

esp_err_t measurement_recorder_begin(uint32_t session_id, uint16_t segment_id)
{
    if (s_task == NULL || segment_id == 0)
        return ESP_ERR_INVALID_ARG;
    taskENTER_CRITICAL(&s_lock);
    bool have_flight = s_have_flight;
    bool active = s_status.active;
    bool healthy = s_status.healthy;
    bool transition_pending = s_end_pending || s_shutdown_pending;
    taskEXIT_CRITICAL(&s_lock);
    if (!have_flight || active || transition_pending) return ESP_ERR_INVALID_STATE;
    if (!healthy) return ESP_FAIL;
    taskENTER_CRITICAL(&s_lock);
    s_session = session_id;
    s_segment = segment_id;
    memset(&s_status, 0, sizeof(s_status));
    s_status.active = s_status.healthy = true;
    taskEXIT_CRITICAL(&s_lock);
    (void)measurement_recorder_log_event(MEASUREMENT_EVENT_SEGMENT_START,
                                         session_id, segment_id);
    ESP_LOGI(TAG, "Recording %s session=%lu segment=%u", s_directory,
             (unsigned long)s_session, s_segment);
    return ESP_OK;
}

esp_err_t measurement_recorder_submit(spectrometer_role_t role,
                                      uint32_t frame_count,
                                      const h1_spectrum_frame_t *frame,
                                      int64_t b_timestamp_us)
{
    if (frame == NULL || frame->sample_count == 0 ||
        frame->sample_count > H1_MAX_SPECTRUM_SAMPLES) return ESP_ERR_INVALID_ARG;
    taskENTER_CRITICAL(&s_lock);
    if (!s_status.active || !s_status.healthy) {
        taskEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t session = s_session;
    uint16_t segment = s_segment;
    s_raw_submitters++;
    taskEXIT_CRITICAL(&s_lock);
    raw_spectrum_record_t *raw;
    if (xQueueReceive(s_free_queue, &raw, 0) != pdTRUE) {
        taskENTER_CRITICAL(&s_lock);
        s_raw_submitters--;
        s_status.raw_dropped++;
        s_totals.raw_dropped++;
        taskEXIT_CRITICAL(&s_lock);
        return ESP_ERR_NO_MEM;
    }
    memset(raw, 0, sizeof(*raw));
    record_time_t time;
    clock_sync_timestamp(b_timestamp_us, &time);
    uint32_t sequence = __atomic_add_fetch(&s_raw_sequence, 1, __ATOMIC_RELAXED);
    data_record_header_init(&raw->header, DATA_RECORD_RAW_SPECTRUM,
                            RAW_RECORD_WIRE_SIZE(frame->sample_count), sequence,
                            session, segment, &time);
    raw->frame_count = frame_count;
    raw->exposure_us = frame->exposure_us;
    raw->sample_count = (uint16_t)frame->sample_count;
    raw->spectrum_scale = frame->spectrum_scale;
    raw->spectrometer_role = role;
    raw->exposure_status = frame->exposure_status;
    raw->frame_quality = RAW_QUALITY_VALID;
    if (frame->exposure_status == H1_EXPOSURE_STATUS_OVER)
        raw->frame_quality |= RAW_QUALITY_SATURATED;
    if (frame->exposure_status == H1_EXPOSURE_STATUS_UNDER)
        raw->frame_quality |= RAW_QUALITY_UNDEREXPOSED;
    memcpy(raw->samples, frame->spectrum, frame->sample_count * sizeof(uint16_t));
    message_t message = {.type = MSG_RAW, .data.raw = raw};
    if (xQueueSend(s_writer_queue, &message, 0) != pdTRUE) {
        xQueueSend(s_free_queue, &raw, 0);
        taskENTER_CRITICAL(&s_lock);
        s_raw_submitters--;
        s_status.raw_dropped++;
        s_totals.raw_dropped++;
        taskEXIT_CRITICAL(&s_lock);
        return ESP_ERR_NO_MEM;
    }
    taskENTER_CRITICAL(&s_lock);
    s_raw_submitters--;
    taskEXIT_CRITICAL(&s_lock);
    note_queue_depth();
    return ESP_OK;
}

esp_err_t measurement_recorder_submit_gps(const gps_record_t *record)
{
    if (record == NULL) return ESP_ERR_INVALID_ARG;
    message_t message = {.type = MSG_GPS};
    taskENTER_CRITICAL(&s_lock);
    if (s_task == NULL || !s_have_flight || !s_accept_aux ||
        s_gps_file == NULL) {
        taskEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t session = s_status.active ? s_session : 0;
    uint16_t segment = s_status.active ? s_segment : 0;
    uint32_t sequence = ++s_gps_sequence;
    s_aux_submitters++;
    taskEXIT_CRITICAL(&s_lock);
    message.data.gps = *record;
    data_record_header_init(&message.data.gps.header, DATA_RECORD_GPS,
                            GPS_RECORD_WIRE_SIZE, sequence, session, segment,
                            &record->header.timestamp);
    bool queued = xQueueSend(s_writer_queue, &message, 0) == pdTRUE;
    taskENTER_CRITICAL(&s_lock);
    s_aux_submitters--;
    if (!queued) {
        s_status.gps_dropped++;
        s_totals.gps_dropped++;
    }
    taskEXIT_CRITICAL(&s_lock);
    if (!queued) {
        return ESP_ERR_NO_MEM;
    }
    note_queue_depth();
    return ESP_OK;
}

esp_err_t measurement_recorder_log_event(measurement_event_t event,
                                         uint32_t argument0,
                                         int32_t argument1)
{
    if (event < MEASUREMENT_EVENT_HANDSHAKE ||
        event > MEASUREMENT_EVENT_FLIGHT_CLOSED) return ESP_ERR_INVALID_ARG;
    taskENTER_CRITICAL(&s_lock);
    bool available = s_task != NULL && s_have_flight && s_accept_aux &&
                     s_event_file != NULL;
    if (available) s_aux_submitters++;
    taskEXIT_CRITICAL(&s_lock);
    if (!available) return ESP_ERR_INVALID_STATE;
    message_t message = {.type = MSG_EVENT};
    message.data.event = make_event(event, argument0, argument1);
    bool queued = xQueueSend(s_writer_queue, &message, 0) == pdTRUE;
    taskENTER_CRITICAL(&s_lock);
    s_aux_submitters--;
    if (!queued) {
        s_status.events_dropped++;
        s_totals.events_dropped++;
    }
    taskEXIT_CRITICAL(&s_lock);
    if (!queued) {
        return ESP_ERR_NO_MEM;
    }
    note_queue_depth();
    return ESP_OK;
}

void measurement_recorder_note_handshake(const uint8_t drone_serial[32],
                                         uint16_t a_firmware_version,
                                         uint8_t drone_link)
{
    if (drone_serial == NULL) return;
    taskENTER_CRITICAL(&s_lock);
    bool changed = memcmp(s_drone_serial, drone_serial,
                          sizeof(s_drone_serial)) != 0 ||
                   s_a_firmware_version != a_firmware_version ||
                   s_drone_link != drone_link;
    memcpy(s_drone_serial, drone_serial, sizeof(s_drone_serial));
    s_a_firmware_version = a_firmware_version;
    s_drone_link = drone_link;
    taskEXIT_CRITICAL(&s_lock);
    (void)measurement_recorder_log_event(MEASUREMENT_EVENT_HANDSHAKE,
                                         a_firmware_version, drone_link);
    if (changed) {
        taskENTER_CRITICAL(&s_lock);
        bool available = s_task != NULL && s_have_flight && s_accept_aux;
        if (available) s_aux_submitters++;
        taskEXIT_CRITICAL(&s_lock);
        if (available) {
            message_t checkpoint = {.type = MSG_CHECKPOINT};
            bool queued = xQueueSend(s_writer_queue, &checkpoint, 0) == pdTRUE;
            taskENTER_CRITICAL(&s_lock);
            s_aux_submitters--;
            taskEXIT_CRITICAL(&s_lock);
            if (queued) {
                note_queue_depth();
            } else {
                ESP_LOGW(TAG, "Handshake summary checkpoint queue full");
            }
        }
    }
}

esp_err_t measurement_recorder_end(void)
{
    taskENTER_CRITICAL(&s_lock);
    if (s_status.active) {
        s_status.active = false;
        s_end_pending = true;
        s_end_barrier_queued = false;
    }
    bool pending = s_end_pending;
    bool barrier_queued = s_end_barrier_queued;
    bool healthy = s_status.healthy;
    taskEXIT_CRITICAL(&s_lock);
    if (!pending) return healthy ? ESP_OK : ESP_FAIL;

    int64_t submit_deadline = esp_timer_get_time() + 100000;
    while (true) {
        taskENTER_CRITICAL(&s_lock);
        bool raw_drained = s_raw_submitters == 0;
        taskEXIT_CRITICAL(&s_lock);
        if (raw_drained) break;
        if (esp_timer_get_time() >= submit_deadline) return ESP_ERR_TIMEOUT;
        vTaskDelay(1);
    }
    if (!barrier_queued) {
        message_t barrier = {.type = MSG_BARRIER};
        if (xQueueSend(s_writer_queue, &barrier,
                       pdMS_TO_TICKS(2000)) != pdTRUE) {
            return ESP_ERR_TIMEOUT;
        }
        taskENTER_CRITICAL(&s_lock);
        s_end_barrier_queued = true;
        taskEXIT_CRITICAL(&s_lock);
    }
    if (xSemaphoreTake(s_barrier, pdMS_TO_TICKS(10000)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    taskENTER_CRITICAL(&s_lock);
    s_end_pending = false;
    s_end_barrier_queued = false;
    taskEXIT_CRITICAL(&s_lock);
    measurement_recorder_status_t status;
    measurement_recorder_get_status(&status);
    ESP_LOGI(TAG, "Segment recorded: raw=%lu reflectance=%lu dropped=%lu "
                  "rejected=%lu write_errors=%lu flushes=%lu flush_errors=%lu "
                  "max_flush=%luus queue_hwm=%lu gps=%lu gps_dropped=%lu "
                  "events=%lu events_dropped=%lu",
             (unsigned long)status.raw_written,
             (unsigned long)status.reflectance_written,
             (unsigned long)status.raw_dropped,
             (unsigned long)status.calculation_rejected,
             (unsigned long)status.write_errors,
             (unsigned long)status.flush_count,
             (unsigned long)status.flush_errors,
             (unsigned long)status.max_flush_us,
             (unsigned long)status.queue_high_watermark,
             (unsigned long)status.gps_written,
             (unsigned long)status.gps_dropped,
             (unsigned long)status.events_written,
             (unsigned long)status.events_dropped);
    return status.healthy ? ESP_OK : ESP_FAIL;
}

esp_err_t measurement_recorder_shutdown(void)
{
    esp_err_t result = measurement_recorder_end();
    /* A timeout means the writer may still own a FILE. Closing or unmounting
     * underneath it would turn a recoverable fault into memory corruption. */
    if (result == ESP_ERR_TIMEOUT) return result;
    taskENTER_CRITICAL(&s_lock);
    bool had_flight = s_have_flight;
    if (had_flight && !s_shutdown_pending) {
        /* Stop new telemetry/events, but retain flight ownership until the
         * writer acknowledges FINALIZE and every handle has been closed. */
        s_accept_aux = false;
        s_shutdown_pending = true;
        s_finalize_queued = false;
    }
    bool finalize_queued = s_finalize_queued;
    taskEXIT_CRITICAL(&s_lock);
    if (!had_flight) return result;

    int64_t deadline = esp_timer_get_time() + 100000;
    while (true) {
        taskENTER_CRITICAL(&s_lock);
        bool drained = s_aux_submitters == 0 && s_raw_submitters == 0;
        taskEXIT_CRITICAL(&s_lock);
        if (drained) break;
        if (esp_timer_get_time() >= deadline) return ESP_ERR_TIMEOUT;
        vTaskDelay(1);
    }
    if (!finalize_queued) {
        message_t finalize = {.type = MSG_FINALIZE};
        if (xQueueSend(s_writer_queue, &finalize,
                       pdMS_TO_TICKS(2000)) != pdTRUE) {
            return ESP_ERR_TIMEOUT;
        }
        taskENTER_CRITICAL(&s_lock);
        s_finalize_queued = true;
        taskEXIT_CRITICAL(&s_lock);
    }
    if (xSemaphoreTake(s_barrier, pdMS_TO_TICKS(10000)) != pdTRUE)
        return ESP_ERR_TIMEOUT;

    esp_err_t close_result = close_files();
    if (result == ESP_OK) result = close_result;
    if (close_result != ESP_OK) note_storage_result(close_result, "Flight file close");
    measurement_recorder_status_t final_status;
    measurement_recorder_get_status(&final_status);
    if (!final_status.healthy && result == ESP_OK) result = ESP_FAIL;
    if (had_flight) {
        esp_err_t summary_result = write_mission_summary(
            result == ESP_OK ? "closed" : "fault");
        if (summary_result != ESP_OK) {
            note_storage_result(summary_result, "Final MISSION.JSON checkpoint");
            if (result == ESP_OK) result = summary_result;
        }
    }
    taskENTER_CRITICAL(&s_lock);
    s_have_flight = false;
    s_shutdown_pending = false;
    s_finalize_queued = false;
    s_session = 0;
    s_segment = 0;
    s_directory[0] = '\0';
    taskEXIT_CRITICAL(&s_lock);
    return result;
}

void measurement_recorder_get_status(measurement_recorder_status_t *out)
{
    if (!out) return;
    taskENTER_CRITICAL(&s_lock); *out = s_status; taskEXIT_CRITICAL(&s_lock);
}
