#include "telemetry.h"

#include <limits.h>
#include <string.h>

#include "data_records.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "telemetry_transport.h"

static const char *TAG = "TELEMETRY";

#define TELEMETRY_TASK_STACK_SIZE 6144
#define TELEMETRY_TASK_PRIORITY 3
#define TELEMETRY_TASK_CORE 0
#define TELEMETRY_UART_RX_BUFFER_SIZE 4096
#define TELEMETRY_UART_TX_BUFFER_SIZE 4096
#define TELEMETRY_UART_DRAIN_SIZE 256
#define TELEMETRY_ACK_STREAM_CAPACITY (TELEMETRY_ACK_WIRE_SIZE * 32U)
#define TELEMETRY_MIN_POOL_LENGTH 16U
#define TELEMETRY_MAX_POOL_LENGTH 2048U
#define TELEMETRY_POLL_MS 20U
#define TELEMETRY_UART_TX_TIMEOUT_MS 1000U
#define TELEMETRY_RECORD_BUFFER_SIZE \
    REFLECTANCE_RECORD_WIRE_SIZE(H1_MAX_SPECTRUM_SAMPLES)

typedef enum {
    ENTRY_FREE = 0,
    ENTRY_FILLING,
    ENTRY_ENQUEUING,
    ENTRY_QUEUED,
    ENTRY_SENDING,
    ENTRY_IN_FLIGHT,
    ENTRY_RETRY_DUE,
    ENTRY_EXHAUSTED,
} telemetry_entry_state_t;

typedef union {
    gps_record_t gps;
    reflectance_record_t reflectance;
} telemetry_record_t;

/** A slot owns one immutable logical message from admission until a matching
 * positive DTA1 acknowledgement (or deliberate mission abort). Keeping the
 * record, rather than only its serialized bytes, makes retries deterministic
 * without dedicating another multi-megabyte wire buffer in PSRAM. */
typedef struct {
    telemetry_record_t record;
    int64_t first_tx_done_us;
    int64_t last_tx_done_us;
    int64_t acknowledgement_deadline_us;
    uint32_t payload_crc32;
    uint16_t transmissions;
    uint8_t message_type;
    uint8_t state;
    bool counted_in_flight;
} telemetry_entry_t;

typedef struct {
    bool uart_failed;
    uint16_t fragments;
    uint32_t bytes;
    int64_t attempt_started_us;
    int64_t last_tx_done_us;
    int64_t emit_done_us;
    int64_t write_call_us;
    int64_t tx_wait_us;
    int64_t gap_wait_us;
} telemetry_timing_t;

static telemetry_config_t s_config;
static telemetry_status_t s_status;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t s_gps_queue;
static QueueHandle_t s_ready_queue;
static QueueHandle_t s_free_queue;
static telemetry_entry_t *s_pool;
static TaskHandle_t s_task;
static bool s_accepting;
static bool s_sending;
static bool s_abort_requested;
static uint32_t s_submitters;

/* Only telemetry_task touches these workspaces. A full reflectance record is
 * intentionally kept off the small internal-RAM RTOS stack. */
static uint8_t s_record_buffer[TELEMETRY_RECORD_BUFFER_SIZE];
static uint8_t s_fragment_buffer[TELEMETRY_FRAGMENT_WIRE_MAX_SIZE];
static uint8_t s_ack_stream[TELEMETRY_ACK_STREAM_CAPACITY];
static size_t s_ack_stream_length;

static bool abort_requested(void)
{
    taskENTER_CRITICAL(&s_lock);
    bool requested = s_abort_requested;
    taskEXIT_CRITICAL(&s_lock);
    return requested;
}

static void note_pool_fault(const char *reason)
{
    taskENTER_CRITICAL(&s_lock);
    s_status.healthy = false;
    s_status.reflectance_pool_errors++;
    taskEXIT_CRITICAL(&s_lock);
    ESP_LOGE(TAG, "Telemetry pool invariant failed: %s", reason);
}

static void note_uart_fault(void)
{
    taskENTER_CRITICAL(&s_lock);
    s_status.healthy = false;
    s_status.uart_errors++;
    taskEXIT_CRITICAL(&s_lock);
}

static void note_serialization_fault(void)
{
    taskENTER_CRITICAL(&s_lock);
    s_status.healthy = false;
    s_status.serialization_errors++;
    taskEXIT_CRITICAL(&s_lock);
}

static bool entry_belongs_to_pool(const telemetry_entry_t *entry)
{
    return entry != NULL && s_pool != NULL &&
           entry >= s_pool && entry < s_pool + s_config.pool_length;
}

static telemetry_entry_t *acquire_entry(void)
{
    telemetry_entry_t *entry = NULL;
    if (xQueueReceive(s_free_queue, &entry, 0) != pdTRUE) return NULL;
    if (!entry_belongs_to_pool(entry)) {
        note_pool_fault("free-list pointer outside pool");
        return NULL;
    }
    taskENTER_CRITICAL(&s_lock);
    if (entry->state != ENTRY_FREE) {
        taskEXIT_CRITICAL(&s_lock);
        note_pool_fault("non-free entry on free list");
        return NULL;
    }
    memset((uint8_t *)entry + sizeof(entry->record), 0,
           sizeof(*entry) - sizeof(entry->record));
    entry->state = ENTRY_FILLING;
    taskEXIT_CRITICAL(&s_lock);
    return entry;
}

static void release_entry(telemetry_entry_t *entry)
{
    if (!entry_belongs_to_pool(entry)) {
        note_pool_fault("attempt to release pointer outside pool");
        return;
    }
    taskENTER_CRITICAL(&s_lock);
    if (entry->state == ENTRY_FREE) {
        taskEXIT_CRITICAL(&s_lock);
        note_pool_fault("double release");
        return;
    }
    if (entry->state != ENTRY_FILLING) {
        if (s_status.pool_used == 0) {
            taskEXIT_CRITICAL(&s_lock);
            note_pool_fault("pool-used underflow");
            return;
        }
        s_status.pool_used--;
    }
    if (entry->counted_in_flight) {
        if (s_status.messages_in_flight == 0) {
            taskEXIT_CRITICAL(&s_lock);
            note_pool_fault("in-flight underflow");
            return;
        }
        s_status.messages_in_flight--;
    }
    entry->state = ENTRY_FREE;
    entry->counted_in_flight = false;
    taskEXIT_CRITICAL(&s_lock);
    if (xQueueSend(s_free_queue, &entry, 0) != pdTRUE)
        note_pool_fault("free list overflow");
}

static void mark_admitted(telemetry_entry_t *entry, uint8_t message_type,
                          telemetry_entry_state_t state)
{
    taskENTER_CRITICAL(&s_lock);
    entry->message_type = message_type;
    entry->state = state;
    s_status.pool_used++;
    if (s_status.pool_used > s_status.pool_high_watermark)
        s_status.pool_high_watermark = s_status.pool_used;
    taskEXIT_CRITICAL(&s_lock);
}

static void mark_exhausted(telemetry_entry_t *entry, const char *reason)
{
    bool first_failure = false;
    taskENTER_CRITICAL(&s_lock);
    if (entry->state != ENTRY_EXHAUSTED) {
        entry->state = ENTRY_EXHAUSTED;
        s_status.messages_failed++;
        first_failure = true;
    }
    taskEXIT_CRITICAL(&s_lock);
    if (first_failure) {
        uint32_t sequence = entry->message_type == TELEMETRY_MESSAGE_GPS
            ? entry->record.gps.header.record_sequence
            : entry->record.reflectance.header.record_sequence;
        ESP_LOGW(TAG, "Retaining exhausted type=%u seq=%lu: %s",
                 (unsigned)entry->message_type, (unsigned long)sequence,
                 reason);
    }
}

static void purge_all_entries(void)
{
    /* FILLING/ENQUEUING belong temporarily to a producer. Abort closes
     * admission first; that producer will enqueue or release, and this repeated
     * worker pass will reclaim it without racing a large PSRAM copy or the
     * publication of its ready-queue pointer. */
    for (size_t i = 0; i < s_config.pool_length; i++) {
        taskENTER_CRITICAL(&s_lock);
        uint8_t state = s_pool[i].state;
        taskEXIT_CRITICAL(&s_lock);
        if (state != ENTRY_FREE && state != ENTRY_FILLING &&
            state != ENTRY_ENQUEUING)
            release_entry(&s_pool[i]);
    }
}

static uint32_t entry_sequence(const telemetry_entry_t *entry)
{
    return entry->message_type == TELEMETRY_MESSAGE_GPS
        ? entry->record.gps.header.record_sequence
        : entry->record.reflectance.header.record_sequence;
}

static uint64_t entry_timestamp_us(const telemetry_entry_t *entry)
{
    return entry->message_type == TELEMETRY_MESSAGE_GPS
        ? entry->record.gps.header.timestamp.b_monotonic_us
        : entry->record.reflectance.header.timestamp.b_monotonic_us;
}

static esp_err_t emit_fragment(const uint8_t *fragment, size_t length,
                               uint16_t index, uint16_t count, void *context)
{
    (void)index;
    (void)count;
    telemetry_timing_t *timing = context;
    if (abort_requested()) return ESP_ERR_INVALID_STATE;

    int64_t write_started_us = esp_timer_get_time();
    int written = uart_write_bytes(s_config.uart_port, fragment, length);
    int64_t write_done_us = esp_timer_get_time();
    timing->write_call_us += write_done_us - write_started_us;
    if (written != (int)length) {
        timing->uart_failed = true;
        note_uart_fault();
        return ESP_FAIL;
    }
    esp_err_t result = uart_wait_tx_done(
        s_config.uart_port, pdMS_TO_TICKS(TELEMETRY_UART_TX_TIMEOUT_MS));
    int64_t tx_done_us = esp_timer_get_time();
    timing->tx_wait_us += tx_done_us - write_done_us;
    timing->last_tx_done_us = tx_done_us;
    if (result != ESP_OK) {
        timing->uart_failed = true;
        note_uart_fault();
        return result;
    }
    if (abort_requested()) return ESP_ERR_INVALID_STATE;

    taskENTER_CRITICAL(&s_lock);
    s_status.fragments_sent++;
    s_status.bytes_sent += (uint32_t)length;
    taskEXIT_CRITICAL(&s_lock);
    timing->fragments++;
    timing->bytes += (uint32_t)length;

    /* This is flow control for the DTU's UART packetizer, not MQTT framing.
     * It includes the final fragment so consecutive logical messages also
     * have an idle boundary. DTF2 remains the authoritative framing layer. */
    vTaskDelay(pdMS_TO_TICKS(s_config.fragment_gap_ms));
    int64_t gap_done_us = esp_timer_get_time();
    timing->gap_wait_us += gap_done_us - tx_done_us;
    timing->emit_done_us = gap_done_us;
    return ESP_OK;
}

static bool take_next_ack(telemetry_ack_t *ack)
{
    static const uint8_t magic[] = {0x44, 0x54, 0x41, 0x31};
    while (s_ack_stream_length >= sizeof(magic)) {
        size_t start = 0;
        while (start + sizeof(magic) <= s_ack_stream_length &&
               memcmp(s_ack_stream + start, magic, sizeof(magic)) != 0) {
            start++;
        }
        if (start + sizeof(magic) > s_ack_stream_length) {
            size_t keep = sizeof(magic) - 1U;
            if (s_ack_stream_length > keep) {
                memmove(s_ack_stream,
                        s_ack_stream + s_ack_stream_length - keep, keep);
                s_ack_stream_length = keep;
            }
            return false;
        }
        if (start != 0) {
            memmove(s_ack_stream, s_ack_stream + start,
                    s_ack_stream_length - start);
            s_ack_stream_length -= start;
        }
        if (s_ack_stream_length < TELEMETRY_ACK_WIRE_SIZE) return false;
        esp_err_t result = telemetry_ack_decode(
            s_ack_stream, TELEMETRY_ACK_WIRE_SIZE, ack);
        size_t consume = result == ESP_OK ? TELEMETRY_ACK_WIRE_SIZE : 1U;
        memmove(s_ack_stream, s_ack_stream + consume,
                s_ack_stream_length - consume);
        s_ack_stream_length -= consume;
        if (result == ESP_OK) return true;
    }
    return false;
}

static void append_downlink(const uint8_t *bytes, size_t length)
{
    taskENTER_CRITICAL(&s_lock);
    s_status.downlink_bytes_received += (uint32_t)length;
    taskEXIT_CRITICAL(&s_lock);
    for (size_t i = 0; i < length; i++) {
        if (s_ack_stream_length == sizeof(s_ack_stream)) {
            memmove(s_ack_stream, s_ack_stream + 1,
                    sizeof(s_ack_stream) - 1U);
            s_ack_stream_length--;
        }
        s_ack_stream[s_ack_stream_length++] = bytes[i];
    }
}

static telemetry_entry_t *find_ack_entry(const telemetry_ack_t *ack)
{
    for (size_t i = 0; i < s_config.pool_length; i++) {
        telemetry_entry_t *entry = &s_pool[i];
        taskENTER_CRITICAL(&s_lock);
        bool candidate = entry->counted_in_flight &&
            entry->message_type == ack->message_type &&
            entry_sequence(entry) == ack->message_sequence &&
            entry->payload_crc32 == ack->message_crc32;
        taskEXIT_CRITICAL(&s_lock);
        if (candidate) return entry;
    }
    return NULL;
}

static void note_unmatched_ack(const telemetry_ack_t *ack)
{
    taskENTER_CRITICAL(&s_lock);
    uint32_t count = ++s_status.acknowledgements_mismatched;
    s_status.acknowledgement_rejected++;
    taskEXIT_CRITICAL(&s_lock);
    if (count == 1 || count % 100U == 0) {
        ESP_LOGW(TAG,
                 "Ignoring stale/unmatched ACK #%lu: src=%016llX "
                 "mission=%016llX type=%u seq=%lu crc=%08lX",
                 (unsigned long)count,
                 (unsigned long long)ack->source_id,
                 (unsigned long long)ack->mission_id,
                 (unsigned)ack->message_type,
                 (unsigned long)ack->message_sequence,
                 (unsigned long)ack->message_crc32);
    }
}

static void process_ack(const telemetry_ack_t *ack)
{
    taskENTER_CRITICAL(&s_lock);
    uint64_t mission_id = s_status.mission_id;
    taskEXIT_CRITICAL(&s_lock);
    if (ack->source_id != s_config.source_id ||
        ack->mission_id != mission_id || mission_id == 0) {
        note_unmatched_ack(ack);
        return;
    }

    telemetry_entry_t *entry = find_ack_entry(ack);
    if (entry == NULL) {
        /* This also covers a duplicate positive ACK for an already released
         * entry. Counting it is useful diagnostics; it is never destructive. */
        note_unmatched_ack(ack);
        return;
    }

    if (ack->status != 0) {
        taskENTER_CRITICAL(&s_lock);
        uint32_t count = ++s_status.acknowledgements_negative;
        s_status.acknowledgement_rejected++;
        taskEXIT_CRITICAL(&s_lock);
        if (count == 1 || count % 100U == 0) {
            ESP_LOGW(TAG,
                     "Cloud rejected ACK #%lu: status=%u type=%u seq=%lu",
                     (unsigned long)count, (unsigned)ack->status,
                     (unsigned)ack->message_type,
                     (unsigned long)ack->message_sequence);
        }
        mark_exhausted(entry, "negative cloud acknowledgement");
        return;
    }

    int64_t acknowledged_us = esp_timer_get_time();
    int64_t raw_rtt = acknowledged_us - entry->first_tx_done_us;
    uint32_t rtt_us = raw_rtt > (int64_t)UINT32_MAX ? UINT32_MAX
                       : raw_rtt > 0 ? (uint32_t)raw_rtt : 0;
    uint32_t sequence = entry_sequence(entry);
    uint16_t transmissions = entry->transmissions;
    uint8_t message_type = entry->message_type;
    taskENTER_CRITICAL(&s_lock);
    s_status.acknowledgements_received++;
    s_status.acknowledgement_rtt_last_us = rtt_us;
    if (rtt_us > s_status.acknowledgement_rtt_max_us)
        s_status.acknowledgement_rtt_max_us = rtt_us;
    s_status.acknowledgement_rtt_sum_us += rtt_us;
    if (message_type == TELEMETRY_MESSAGE_GPS)
        s_status.gps_sent++;
    else if (message_type == TELEMETRY_MESSAGE_REFLECTANCE)
        s_status.reflectance_sent++;
    uint32_t pool_used = s_status.pool_used;
    uint32_t in_flight = s_status.messages_in_flight;
    taskEXIT_CRITICAL(&s_lock);

    if (s_config.timing_diagnostics) {
        ESP_LOGI(TAG,
                 "ACK_TIMING type=%u seq=%lu attempts=%u rtt=%luus "
                 "pool=%lu inflight=%lu",
                 (unsigned)message_type, (unsigned long)sequence,
                 (unsigned)transmissions, (unsigned long)rtt_us,
                 (unsigned long)pool_used, (unsigned long)in_flight);
    }
    release_entry(entry);
}

static bool drain_downlink(TickType_t wait_ticks)
{
    uint8_t incoming[TELEMETRY_UART_DRAIN_SIZE];
    int count = uart_read_bytes(s_config.uart_port, incoming,
                                sizeof(incoming), wait_ticks);
    if (count < 0) {
        note_uart_fault();
        return false;
    }
    if (count > 0) append_downlink(incoming, (size_t)count);
    telemetry_ack_t ack;
    while (take_next_ack(&ack)) process_ack(&ack);
    return count > 0;
}

static int64_t acknowledgement_timeout_us(uint16_t transmissions)
{
    unsigned shift = transmissions > 1U ? transmissions - 1U : 0U;
    if (shift > 3U) shift = 3U;
    return (int64_t)s_config.ack_timeout_ms * 1000LL * (1LL << shift);
}

static void update_expired_entries(int64_t now_us)
{
    for (size_t i = 0; i < s_config.pool_length; i++) {
        telemetry_entry_t *entry = &s_pool[i];
        taskENTER_CRITICAL(&s_lock);
        bool expired = entry->state == ENTRY_IN_FLIGHT &&
                       now_us >= entry->acknowledgement_deadline_us;
        uint16_t transmissions = entry->transmissions;
        if (expired) {
            s_status.acknowledgement_timeouts++;
            if (transmissions <= s_config.max_retries)
                entry->state = ENTRY_RETRY_DUE;
        }
        taskEXIT_CRITICAL(&s_lock);
        if (expired && transmissions > s_config.max_retries)
            mark_exhausted(entry, "application ACK timeout");
    }
}

static telemetry_entry_t *take_retry_due(void)
{
    for (size_t i = 0; i < s_config.pool_length; i++) {
        taskENTER_CRITICAL(&s_lock);
        if (s_pool[i].state == ENTRY_RETRY_DUE) {
            s_pool[i].state = ENTRY_SENDING;
            taskEXIT_CRITICAL(&s_lock);
            return &s_pool[i];
        }
        taskEXIT_CRITICAL(&s_lock);
    }
    return NULL;
}

static telemetry_entry_t *take_ready(void)
{
    telemetry_entry_t *entry = NULL;
    if (xQueueReceive(s_ready_queue, &entry, 0) != pdTRUE) return NULL;
    if (!entry_belongs_to_pool(entry)) {
        note_pool_fault("ready-list pointer outside pool");
        return NULL;
    }
    taskENTER_CRITICAL(&s_lock);
    if (entry->state != ENTRY_QUEUED &&
        entry->state != ENTRY_ENQUEUING) {
        taskEXIT_CRITICAL(&s_lock);
        note_pool_fault("non-queued entry on ready list");
        return NULL;
    }
    entry->state = ENTRY_SENDING;
    taskEXIT_CRITICAL(&s_lock);
    return entry;
}

static telemetry_entry_t *take_due_gps(TickType_t now, TickType_t *last_gps,
                                        bool *gps_admitted_once)
{
    TickType_t interval = pdMS_TO_TICKS(s_config.gps_min_interval_ms);
    bool due = !*gps_admitted_once || now - *last_gps >= interval;
    if (!due || uxQueueMessagesWaiting(s_gps_queue) == 0) return NULL;

    telemetry_entry_t *entry = acquire_entry();
    if (entry == NULL) return NULL;
    if (xQueueReceive(s_gps_queue, &entry->record.gps, 0) != pdTRUE) {
        release_entry(entry);
        return NULL;
    }
    mark_admitted(entry, TELEMETRY_MESSAGE_GPS, ENTRY_SENDING);
    *last_gps = now;
    *gps_admitted_once = true;
    return entry;
}

static esp_err_t serialize_entry(telemetry_entry_t *entry, size_t *length)
{
    if (entry->message_type == TELEMETRY_MESSAGE_GPS) {
        return data_record_serialize_gps(&entry->record.gps, s_record_buffer,
                                         sizeof(s_record_buffer), length);
    }
    if (entry->message_type == TELEMETRY_MESSAGE_REFLECTANCE) {
        return data_record_serialize_reflectance(
            &entry->record.reflectance, s_record_buffer,
            sizeof(s_record_buffer), length);
    }
    return ESP_ERR_INVALID_ARG;
}

static esp_err_t transmit_entry(telemetry_entry_t *entry, uint64_t mission_id)
{
    size_t length = 0;
    esp_err_t result = serialize_entry(entry, &length);
    if (result != ESP_OK) {
        note_serialization_fault();
        mark_exhausted(entry, "record serialization failed");
        return result;
    }

    telemetry_fragment_plan_t plan;
    result = telemetry_fragment_plan_init(
        &plan, entry->message_type, s_config.source_id, mission_id,
        entry_sequence(entry), 0, s_record_buffer, length);
    if (result != ESP_OK) {
        note_serialization_fault();
        mark_exhausted(entry, "fragment plan failed");
        return result;
    }
    if (entry->payload_crc32 != 0 &&
        entry->payload_crc32 != plan.payload_crc32) {
        note_pool_fault("immutable retry payload changed");
        mark_exhausted(entry, "retry payload changed");
        return ESP_ERR_INVALID_CRC;
    }
    entry->payload_crc32 = plan.payload_crc32;

    telemetry_timing_t timing = {
        .attempt_started_us = esp_timer_get_time(),
    };
    uint16_t prior_transmissions = entry->transmissions;
    entry->transmissions++;
    taskENTER_CRITICAL(&s_lock);
    s_status.transmission_attempts++;
    if (prior_transmissions != 0) s_status.messages_retried++;
    taskEXIT_CRITICAL(&s_lock);

    result = telemetry_fragment_emit_all(
        &plan, s_fragment_buffer, sizeof(s_fragment_buffer),
        emit_fragment, &timing);
    int64_t finished_us = esp_timer_get_time();
    if (result != ESP_OK && !timing.uart_failed && !abort_requested())
        note_serialization_fault();

    if (s_config.timing_diagnostics) {
        int64_t queue_us = timing.attempt_started_us -
                           (int64_t)entry_timestamp_us(entry);
        int64_t tx_phase_us = timing.emit_done_us != 0
            ? timing.emit_done_us - timing.attempt_started_us : -1;
        taskENTER_CRITICAL(&s_lock);
        uint32_t pool_used = s_status.pool_used;
        uint32_t in_flight = s_status.messages_in_flight;
        taskEXIT_CRITICAL(&s_lock);
        ESP_LOGI(TAG,
                 "TX_TIMING type=%u seq=%lu attempt=%u result=%s "
                 "queue=%lldus fragments=%u bytes=%lu tx_phase=%lldus "
                 "write=%lldus uart_tx=%lldus gaps=%lldus total=%lldus "
                 "pool=%lu inflight=%lu ready=%u",
                 (unsigned)entry->message_type,
                 (unsigned long)entry_sequence(entry),
                 (unsigned)entry->transmissions,
                 esp_err_to_name(result), (long long)queue_us,
                 (unsigned)timing.fragments, (unsigned long)timing.bytes,
                 (long long)tx_phase_us, (long long)timing.write_call_us,
                 (long long)timing.tx_wait_us,
                 (long long)timing.gap_wait_us,
                 (long long)(finished_us - timing.attempt_started_us),
                 (unsigned long)pool_used, (unsigned long)in_flight,
                 (unsigned)uxQueueMessagesWaiting(s_ready_queue));
    }

    if (abort_requested()) return ESP_ERR_INVALID_STATE;
    if (result == ESP_OK) {
        taskENTER_CRITICAL(&s_lock);
        entry->last_tx_done_us = timing.last_tx_done_us;
        if (!entry->counted_in_flight) {
            entry->counted_in_flight = true;
            entry->first_tx_done_us = timing.last_tx_done_us;
            s_status.messages_in_flight++;
            if (s_status.messages_in_flight >
                s_status.messages_in_flight_high_watermark) {
                s_status.messages_in_flight_high_watermark =
                    s_status.messages_in_flight;
            }
        }
        entry->acknowledgement_deadline_us = timing.last_tx_done_us +
            acknowledgement_timeout_us(entry->transmissions);
        entry->state = ENTRY_IN_FLIGHT;
        taskEXIT_CRITICAL(&s_lock);
        return ESP_OK;
    }

    taskENTER_CRITICAL(&s_lock);
    bool can_retry = entry->transmissions <= s_config.max_retries;
    if (can_retry) entry->state = ENTRY_RETRY_DUE;
    taskEXIT_CRITICAL(&s_lock);
    if (!can_retry) mark_exhausted(entry, "UART transmission failed");
    return result;
}

static void telemetry_task(void *unused)
{
    (void)unused;
    TickType_t last_gps = 0;
    bool gps_admitted_once = false;

    while (true) {
        if (abort_requested()) {
            xQueueReset(s_gps_queue);
            xQueueReset(s_ready_queue);
            purge_all_entries();
            taskENTER_CRITICAL(&s_lock);
            s_sending = false;
            taskEXIT_CRITICAL(&s_lock);
            vTaskDelay(pdMS_TO_TICKS(TELEMETRY_POLL_MS));
            continue;
        }

        /* ACK ingestion is independent of the message being sent. Delayed,
         * duplicate and out-of-order DTA1 frames are matched against all live
         * slots, rather than a single stop-and-wait current message. */
        while (drain_downlink(0)) {}
        update_expired_entries(esp_timer_get_time());

        telemetry_entry_t *entry = take_due_gps(
            xTaskGetTickCount(), &last_gps, &gps_admitted_once);
        if (entry == NULL) entry = take_retry_due();
        if (entry == NULL) entry = take_ready();
        if (entry != NULL) {
            taskENTER_CRITICAL(&s_lock);
            s_sending = true;
            uint64_t mission_id = s_status.mission_id;
            taskEXIT_CRITICAL(&s_lock);
            (void)transmit_entry(entry, mission_id);
            taskENTER_CRITICAL(&s_lock);
            s_sending = false;
            taskEXIT_CRITICAL(&s_lock);
            while (drain_downlink(0)) {}
            continue;
        }

        (void)drain_downlink(pdMS_TO_TICKS(TELEMETRY_POLL_MS));
    }
}

static esp_err_t overwrite_gps(const gps_record_t *record)
{
    bool superseded = uxQueueMessagesWaiting(s_gps_queue) != 0;
    if (xQueueOverwrite(s_gps_queue, record) != pdPASS) return ESP_FAIL;
    taskENTER_CRITICAL(&s_lock);
    s_status.gps_submitted++;
    if (superseded) s_status.gps_superseded++;
    taskEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

static esp_err_t enqueue_reflectance(const reflectance_record_t *record)
{
    telemetry_entry_t *entry = acquire_entry();
    if (entry != NULL) {
        entry->record.reflectance = *record;
        /* Abort-side reclamation skips ENQUEUING until the pointer publication
         * is complete. The worker may dequeue immediately and advance it to
         * SENDING before this producer gets CPU again. */
        mark_admitted(entry, TELEMETRY_MESSAGE_REFLECTANCE, ENTRY_ENQUEUING);
        if (xQueueSend(s_ready_queue, &entry, 0) == pdTRUE) {
            taskENTER_CRITICAL(&s_lock);
            if (entry->state == ENTRY_ENQUEUING)
                entry->state = ENTRY_QUEUED;
            s_status.reflectance_submitted++;
            taskEXIT_CRITICAL(&s_lock);
            return ESP_OK;
        }
        note_pool_fault("ready list overflow");
        release_entry(entry);
    }

    taskENTER_CRITICAL(&s_lock);
    uint32_t dropped = ++s_status.reflectance_queue_overflows;
    taskEXIT_CRITICAL(&s_lock);
    if (dropped == 1 || dropped % 100U == 0) {
        ESP_LOGW(TAG, "Telemetry pool full; reflectance dropped=%lu",
                 (unsigned long)dropped);
    }
    return ESP_ERR_NO_MEM;
}

static void delete_resources(bool driver_installed)
{
    if (driver_installed) (void)uart_driver_delete(s_config.uart_port);
    if (s_gps_queue != NULL) vQueueDelete(s_gps_queue);
    if (s_ready_queue != NULL) vQueueDelete(s_ready_queue);
    if (s_free_queue != NULL) vQueueDelete(s_free_queue);
    if (s_pool != NULL) heap_caps_free(s_pool);
    s_gps_queue = s_ready_queue = s_free_queue = NULL;
    s_pool = NULL;
}

esp_err_t telemetry_start(const telemetry_config_t *config)
{
    if (config == NULL || config->uart_port < UART_NUM_0 ||
        config->uart_port >= UART_NUM_MAX || config->tx_gpio < 0 ||
        config->rx_gpio < 0 || config->tx_gpio == config->rx_gpio ||
        config->baud_rate == 0 || config->fragment_gap_ms < 6 ||
        config->gps_min_interval_ms == 0 || config->source_id == 0 ||
        config->ack_timeout_ms == 0 || config->max_retries > 3 ||
        config->pool_length < TELEMETRY_MIN_POOL_LENGTH ||
        config->pool_length > TELEMETRY_MAX_POOL_LENGTH ||
        pdMS_TO_TICKS(config->fragment_gap_ms) == 0 ||
        pdMS_TO_TICKS(config->gps_min_interval_ms) == 0 ||
        pdMS_TO_TICKS(config->ack_timeout_ms) == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_task != NULL) return ESP_ERR_INVALID_STATE;

    s_config = *config;
    s_pool = heap_caps_calloc(config->pool_length, sizeof(*s_pool),
                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_gps_queue = xQueueCreate(1, sizeof(gps_record_t));
    s_ready_queue = xQueueCreate(config->pool_length,
                                 sizeof(telemetry_entry_t *));
    s_free_queue = xQueueCreate(config->pool_length,
                                sizeof(telemetry_entry_t *));
    if (s_pool == NULL || s_gps_queue == NULL || s_ready_queue == NULL ||
        s_free_queue == NULL) {
        delete_resources(false);
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < config->pool_length; i++) {
        telemetry_entry_t *entry = &s_pool[i];
        entry->state = ENTRY_FREE;
        if (xQueueSend(s_free_queue, &entry, 0) != pdTRUE) {
            delete_resources(false);
            return ESP_ERR_NO_MEM;
        }
    }

    const uart_config_t uart_config = {
        .baud_rate = (int)config->baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    bool driver_installed = false;
    esp_err_t result = uart_driver_install(
        config->uart_port, TELEMETRY_UART_RX_BUFFER_SIZE,
        TELEMETRY_UART_TX_BUFFER_SIZE, 0, NULL, 0);
    if (result == ESP_OK) driver_installed = true;
    if (result == ESP_OK)
        result = uart_param_config(config->uart_port, &uart_config);
    if (result == ESP_OK) {
        result = uart_set_pin(config->uart_port, config->tx_gpio,
                              config->rx_gpio, UART_PIN_NO_CHANGE,
                              UART_PIN_NO_CHANGE);
    }
    if (result != ESP_OK) {
        delete_resources(driver_installed);
        return result;
    }

    taskENTER_CRITICAL(&s_lock);
    memset(&s_status, 0, sizeof(s_status));
    s_status.initialized = true;
    s_status.healthy = true;
    s_status.source_id = config->source_id;
    s_status.pool_capacity = config->pool_length;
    s_accepting = false;
    s_sending = false;
    s_abort_requested = false;
    s_submitters = 0;
    s_ack_stream_length = 0;
    taskEXIT_CRITICAL(&s_lock);
    if (xTaskCreatePinnedToCore(
            telemetry_task, "telemetry_tx", TELEMETRY_TASK_STACK_SIZE, NULL,
            TELEMETRY_TASK_PRIORITY, &s_task, TELEMETRY_TASK_CORE) != pdPASS) {
        taskENTER_CRITICAL(&s_lock);
        s_status.initialized = false;
        s_status.healthy = false;
        taskEXIT_CRITICAL(&s_lock);
        delete_resources(true);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG,
             "DTU UART%d TX=GPIO%d RX=GPIO%d %lu 8N1; gap=%lums GPS=%lums "
             "source=%016llX ACK=%lums retries=%u; async_pool=%u entries, "
             "%u bytes PSRAM",
             config->uart_port, config->tx_gpio, config->rx_gpio,
             (unsigned long)config->baud_rate,
             (unsigned long)config->fragment_gap_ms,
             (unsigned long)config->gps_min_interval_ms,
             (unsigned long long)config->source_id,
             (unsigned long)config->ack_timeout_ms, config->max_retries,
             (unsigned)config->pool_length,
             (unsigned)(config->pool_length * sizeof(*s_pool)));
    return ESP_OK;
}

esp_err_t telemetry_begin_mission(uint64_t mission_id)
{
    if (mission_id == 0) return ESP_ERR_INVALID_ARG;
    if (s_task == NULL) return ESP_ERR_INVALID_STATE;
    taskENTER_CRITICAL(&s_lock);
    bool available = s_status.mission_id == 0 && !s_accepting &&
                     s_submitters == 0 && s_status.pool_used == 0;
    taskEXIT_CRITICAL(&s_lock);
    if (!available || uxQueueMessagesWaiting(s_gps_queue) != 0 ||
        uxQueueMessagesWaiting(s_ready_queue) != 0 ||
        uxQueueMessagesWaiting(s_free_queue) != s_config.pool_length) {
        return ESP_ERR_INVALID_STATE;
    }

    xQueueReset(s_gps_queue);
    taskENTER_CRITICAL(&s_lock);
    bool healthy = s_status.healthy;
    uint32_t pool_errors = s_status.reflectance_pool_errors;
    uint32_t serialization_errors = s_status.serialization_errors;
    uint32_t uart_errors = s_status.uart_errors;
    memset(&s_status, 0, sizeof(s_status));
    s_status.initialized = true;
    s_status.healthy = healthy;
    s_status.reflectance_pool_errors = pool_errors;
    s_status.serialization_errors = serialization_errors;
    s_status.uart_errors = uart_errors;
    s_status.source_id = s_config.source_id;
    s_status.mission_id = mission_id;
    s_status.pool_capacity = s_config.pool_length;
    s_accepting = true;
    s_sending = false;
    s_abort_requested = false;
    s_submitters = 0;
    taskEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t telemetry_submit_gps(const gps_record_t *record)
{
    if (record == NULL) return ESP_ERR_INVALID_ARG;
    taskENTER_CRITICAL(&s_lock);
    bool active = s_task != NULL && s_accepting && s_status.mission_id != 0;
    if (active) s_submitters++;
    taskEXIT_CRITICAL(&s_lock);
    if (!active) return ESP_ERR_INVALID_STATE;
    esp_err_t result = overwrite_gps(record);
    taskENTER_CRITICAL(&s_lock);
    s_submitters--;
    taskEXIT_CRITICAL(&s_lock);
    return result;
}

esp_err_t telemetry_submit_reflectance(const reflectance_record_t *record)
{
    if (record == NULL || record->sample_count == 0 ||
        record->sample_count > H1_MAX_SPECTRUM_SAMPLES) {
        return ESP_ERR_INVALID_ARG;
    }
    taskENTER_CRITICAL(&s_lock);
    bool active = s_task != NULL && s_accepting && s_status.mission_id != 0;
    if (active) s_submitters++;
    taskEXIT_CRITICAL(&s_lock);
    if (!active) return ESP_ERR_INVALID_STATE;
    esp_err_t result = enqueue_reflectance(record);
    taskENTER_CRITICAL(&s_lock);
    s_submitters--;
    taskEXIT_CRITICAL(&s_lock);
    return result;
}

static bool mission_delivery_drained(void)
{
    taskENTER_CRITICAL(&s_lock);
    bool idle_before = !s_sending && s_submitters == 0 &&
                       s_status.pool_used == 0;
    taskEXIT_CRITICAL(&s_lock);
    if (!idle_before) return false;
    if (uxQueueMessagesWaiting(s_gps_queue) != 0 ||
        uxQueueMessagesWaiting(s_ready_queue) != 0) {
        return false;
    }
    taskENTER_CRITICAL(&s_lock);
    bool idle_after = !s_sending && s_submitters == 0 &&
                      s_status.pool_used == 0;
    taskEXIT_CRITICAL(&s_lock);
    return idle_after;
}

esp_err_t telemetry_finish_mission(uint32_t timeout_ms)
{
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    if (timeout_ms == 0 || timeout_ticks == 0) return ESP_ERR_INVALID_ARG;
    taskENTER_CRITICAL(&s_lock);
    if (s_task == NULL || s_status.mission_id == 0 || s_abort_requested) {
        taskEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_accepting = false;
    taskEXIT_CRITICAL(&s_lock);

    TickType_t started = xTaskGetTickCount();
    while (true) {
        if (mission_delivery_drained()) return ESP_OK;
        if (xTaskGetTickCount() - started >= timeout_ticks) {
            if (mission_delivery_drained()) return ESP_OK;
            taskENTER_CRITICAL(&s_lock);
            s_status.drain_timeouts++;
            taskEXIT_CRITICAL(&s_lock);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }
}

esp_err_t telemetry_abort_mission(void)
{
    taskENTER_CRITICAL(&s_lock);
    if (s_task == NULL || s_status.mission_id == 0) {
        taskEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_accepting = false;
    s_abort_requested = true;
    s_status.shutdown_aborted = true;
    taskEXIT_CRITICAL(&s_lock);
    xQueueReset(s_gps_queue);
    ESP_LOGI(TAG,
             "Telemetry pool abandoned for power-off; SD finalization has priority");
    return ESP_OK;
}

void telemetry_get_status(telemetry_status_t *out)
{
    if (out == NULL) return;
    taskENTER_CRITICAL(&s_lock);
    *out = s_status;
    taskEXIT_CRITICAL(&s_lock);
}
