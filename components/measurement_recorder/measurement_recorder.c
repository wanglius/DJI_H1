#include "measurement_recorder.h"

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
#include "sd_card.h"

static const char *TAG = "MEAS_REC";

#define RAW_POOL_COUNT 12
#define WRITER_QUEUE_LENGTH (RAW_POOL_COUNT + 2)
#define SERIAL_BUFFER_SIZE 4096
#define FILE_HEADER_SIZE 16U
#define SKY_HISTORY_COUNT 8U

typedef enum { MSG_RAW, MSG_BARRIER } message_type_t;
typedef struct { message_type_t type; raw_spectrum_record_t *raw; } message_t;

static raw_spectrum_record_t s_pool[RAW_POOL_COUNT];
static QueueHandle_t s_free_queue, s_writer_queue;
static SemaphoreHandle_t s_barrier;
static TaskHandle_t s_task;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static measurement_recorder_status_t s_status = {.healthy = true};
static uint32_t s_session, s_raw_sequence, s_calculation_sequence;
static uint16_t s_segment;
static uint16_t s_flight_key;
static bool s_have_flight;
static char s_directory[24];
static sd_card_file_t *s_raw_file, *s_reflectance_file;
/* Sole writer-task workspaces live in BSS to keep its stack bounded. */
static uint8_t s_serial_buffer[SERIAL_BUFFER_SIZE];
static raw_spectrum_record_t s_sky_history[SKY_HISTORY_COUNT];
static reflectance_record_t s_calculated;

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
        if (reflectance) s_status.reflectance_written++;
        else s_status.raw_written++;
    } else {
        s_status.write_errors++;
        s_status.healthy = false;
    }
    taskEXIT_CRITICAL(&s_lock);
}

static void writer_task(void *unused)
{
    (void)unused;
    size_t sky_next = 0, sky_count = 0;
    message_t message;
    while (true) {
        if (xQueueReceive(s_writer_queue, &message, portMAX_DELAY) != pdTRUE)
            continue;
        if (message.type == MSG_BARRIER) {
            esp_err_t result = s_raw_file
                ? sd_card_file_flush(s_raw_file) : ESP_OK;
            esp_err_t second = s_reflectance_file
                ? sd_card_file_flush(s_reflectance_file) : ESP_OK;
            if (result == ESP_OK) result = second;
            taskENTER_CRITICAL(&s_lock);
            if (result != ESP_OK) {
                s_status.write_errors++;
                s_status.healthy = false;
            }
            taskEXIT_CRITICAL(&s_lock);
            /* A barrier terminates a capture segment. Never pair a later
             * ground frame with a reference retained across that boundary. */
            sky_next = sky_count = 0;
            xSemaphoreGive(s_barrier);
            continue;
        }
        raw_spectrum_record_t *raw = message.raw;
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
                taskEXIT_CRITICAL(&s_lock);
            }
        }
        xQueueSend(s_free_queue, &raw, portMAX_DELAY);
    }
}

esp_err_t measurement_recorder_init(void)
{
    if (s_task != NULL) return ESP_ERR_INVALID_STATE;
    s_free_queue = xQueueCreate(RAW_POOL_COUNT, sizeof(raw_spectrum_record_t *));
    s_writer_queue = xQueueCreate(WRITER_QUEUE_LENGTH, sizeof(message_t));
    s_barrier = xSemaphoreCreateBinary();
    if (!s_free_queue || !s_writer_queue || !s_barrier) return ESP_ERR_NO_MEM;
    for (size_t i = 0; i < RAW_POOL_COUNT; i++) {
        raw_spectrum_record_t *item = &s_pool[i];
        xQueueSend(s_free_queue, &item, 0);
    }
    s_status.healthy = true;
    return xTaskCreatePinnedToCore(writer_task, "measurement_writer", 8192, NULL,
                                   4, &s_task, 0) == pdPASS
        ? ESP_OK : ESP_ERR_NO_MEM;
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
    return result;
}

esp_err_t measurement_recorder_begin(uint32_t session_id, uint16_t segment_id)
{
    if (s_task == NULL || session_id == 0 || segment_id == 0)
        return ESP_ERR_INVALID_ARG;
    measurement_recorder_status_t status;
    measurement_recorder_get_status(&status);
    if (status.active) return ESP_ERR_INVALID_STATE;

    /* The upper 16 bits are assigned by A once per flight; the lower bits
     * identify restartable capture sessions within that flight. */
    uint16_t flight_key = (uint16_t)(session_id >> 16);
    if (!s_have_flight || s_flight_key != flight_key) {
        esp_err_t result = close_files();
        if (result != ESP_OK) return result;

        /* Never append to an old binary stream: after a B-board reboot its
         * sequence counters restart. A recovery suffix makes that boundary
         * explicit and prevents a superficially valid but ambiguous file. */
        char path[56];
        bool exists = false;
        for (unsigned recovery = 0; recovery < 100; recovery++) {
            if (recovery == 0)
                snprintf(s_directory, sizeof(s_directory), "F_%04X", flight_key);
            else
                snprintf(s_directory, sizeof(s_directory), "F_%04X_R%02u",
                         flight_key, recovery);
            /* Reserve an entirely unused directory. This also avoids
             * overwriting a lone file left by a partially completed open. */
            result = sd_card_path_exists(s_directory, &exists);
            if (result != ESP_OK) return result;
            if (!exists) break;
        }
        if (exists) return ESP_ERR_NO_MEM;
        result = sd_card_mkdir(s_directory);
        if (result != ESP_OK) return result;
        snprintf(path, sizeof(path), "%s/RAW_SPECTRA.BIN", s_directory);
        result = sd_card_file_open(path, "wb", &s_raw_file);
        if (result != ESP_OK) return result;
        snprintf(path, sizeof(path), "%s/REFLECTANCE.BIN", s_directory);
        result = sd_card_file_open(path, "wb", &s_reflectance_file);
        if (result != ESP_OK) { (void)close_files(); return result; }
        result = write_file_header(s_raw_file, DATA_RECORD_RAW_SPECTRUM);
        if (result == ESP_OK)
            result = write_file_header(s_reflectance_file,
                                       DATA_RECORD_REFLECTANCE);
        if (result != ESP_OK) { (void)close_files(); return result; }
        s_flight_key = flight_key;
        s_have_flight = true;
        s_raw_sequence = s_calculation_sequence = 0;
    }
    s_session = session_id;
    s_segment = segment_id;
    taskENTER_CRITICAL(&s_lock);
    memset(&s_status, 0, sizeof(s_status));
    s_status.active = s_status.healthy = true;
    taskEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "Recording %s session=%lu segment=%u", s_directory,
             (unsigned long)s_session, s_segment);
    return ESP_OK;
}

esp_err_t measurement_recorder_submit(spectrometer_role_t role,
                                      uint32_t frame_count,
                                      const h1_spectrum_frame_t *frame,
                                      int64_t b_timestamp_us)
{
    measurement_recorder_status_t status;
    measurement_recorder_get_status(&status);
    if (!status.active || !status.healthy || frame == NULL || frame->sample_count == 0 ||
        frame->sample_count > H1_MAX_SPECTRUM_SAMPLES) return ESP_ERR_INVALID_STATE;
    raw_spectrum_record_t *raw;
    if (xQueueReceive(s_free_queue, &raw, 0) != pdTRUE) {
        taskENTER_CRITICAL(&s_lock); s_status.raw_dropped++; taskEXIT_CRITICAL(&s_lock);
        return ESP_ERR_NO_MEM;
    }
    memset(raw, 0, sizeof(*raw));
    record_time_t time;
    clock_sync_timestamp(b_timestamp_us, &time);
    uint32_t sequence = __atomic_add_fetch(&s_raw_sequence, 1, __ATOMIC_RELAXED);
    data_record_header_init(&raw->header, DATA_RECORD_RAW_SPECTRUM,
                            RAW_RECORD_WIRE_SIZE(frame->sample_count), sequence,
                            s_session, s_segment, &time);
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
    message_t message = {.type = MSG_RAW, .raw = raw};
    if (xQueueSend(s_writer_queue, &message, 0) != pdTRUE) {
        xQueueSend(s_free_queue, &raw, 0);
        taskENTER_CRITICAL(&s_lock); s_status.raw_dropped++; taskEXIT_CRITICAL(&s_lock);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t measurement_recorder_end(void)
{
    measurement_recorder_status_t status;
    measurement_recorder_get_status(&status);
    if (!status.active) return status.healthy ? ESP_OK : ESP_FAIL;
    taskENTER_CRITICAL(&s_lock); s_status.active = false; taskEXIT_CRITICAL(&s_lock);
    message_t barrier = {.type = MSG_BARRIER};
    if (xQueueSend(s_writer_queue, &barrier, pdMS_TO_TICKS(2000)) != pdTRUE ||
        xSemaphoreTake(s_barrier, pdMS_TO_TICKS(10000)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    measurement_recorder_get_status(&status);
    ESP_LOGI(TAG, "Segment recorded: raw=%lu reflectance=%lu dropped=%lu "
                  "rejected=%lu write_errors=%lu",
             (unsigned long)status.raw_written,
             (unsigned long)status.reflectance_written,
             (unsigned long)status.raw_dropped,
             (unsigned long)status.calculation_rejected,
             (unsigned long)status.write_errors);
    return status.healthy ? ESP_OK : ESP_FAIL;
}

esp_err_t measurement_recorder_shutdown(void)
{
    esp_err_t result = measurement_recorder_end();
    /* A timeout means the writer may still own a FILE. Closing or unmounting
     * underneath it would turn a recoverable fault into memory corruption. */
    if (result == ESP_ERR_TIMEOUT) return result;
    esp_err_t close_result = close_files();
    if (result == ESP_OK) result = close_result;
    s_session = 0;
    s_have_flight = false;
    s_directory[0] = '\0';
    return result;
}

void measurement_recorder_get_status(measurement_recorder_status_t *out)
{
    if (!out) return;
    taskENTER_CRITICAL(&s_lock); *out = s_status; taskEXIT_CRITICAL(&s_lock);
}
