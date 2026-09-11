#include "telemetry.h"

#include <string.h>

#include "data_records.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "telemetry_transport.h"

static const char *TAG = "TELEMETRY";

#define TELEMETRY_TASK_STACK_SIZE 6144
#define TELEMETRY_TASK_PRIORITY 3
#define TELEMETRY_TASK_CORE 0
#define TELEMETRY_UART_RX_BUFFER_SIZE 2048
#define TELEMETRY_UART_TX_BUFFER_SIZE 4096
#define TELEMETRY_UART_DRAIN_SIZE 128
#define TELEMETRY_REFLECTANCE_QUEUE_LENGTH 8
#define TELEMETRY_POLL_MS 20
#define TELEMETRY_UART_TX_TIMEOUT_MS 1000
#define TELEMETRY_RECORD_BUFFER_SIZE \
    REFLECTANCE_RECORD_WIRE_SIZE(H1_MAX_SPECTRUM_SAMPLES)

static telemetry_config_t s_config;
static telemetry_status_t s_status;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t s_gps_queue;
static QueueHandle_t s_reflectance_queue;
static TaskHandle_t s_task;
static bool s_accepting;
static bool s_sending;
static uint32_t s_submitters;

/* Sole task workspaces are static: a full reflectance record and its encoded
 * form are too large for a production RTOS stack. */
static uint8_t s_record_buffer[TELEMETRY_RECORD_BUFFER_SIZE];
static uint8_t s_fragment_buffer[TELEMETRY_FRAGMENT_WIRE_MAX_SIZE];
static uint8_t s_ack_stream[TELEMETRY_ACK_WIRE_SIZE * 2U];
static size_t s_ack_stream_length;

static void note_message_failure(void)
{
    taskENTER_CRITICAL(&s_lock);
    s_status.messages_failed++;
    taskEXIT_CRITICAL(&s_lock);
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

static esp_err_t emit_fragment(const uint8_t *fragment, size_t length,
                               uint16_t index, uint16_t count, void *context)
{
    (void)index;
    (void)count;
    bool *uart_failed = context;
    int written = uart_write_bytes(s_config.uart_port, fragment, length);
    if (written != (int)length) {
        *uart_failed = true;
        note_uart_fault();
        return ESP_FAIL;
    }
    esp_err_t result = uart_wait_tx_done(
        s_config.uart_port, pdMS_TO_TICKS(TELEMETRY_UART_TX_TIMEOUT_MS));
    if (result != ESP_OK) {
        *uart_failed = true;
        note_uart_fault();
        return result;
    }

    taskENTER_CRITICAL(&s_lock);
    s_status.fragments_sent++;
    s_status.bytes_sent += (uint32_t)length;
    taskEXIT_CRITICAL(&s_lock);

    /* The DTU's length and idle-gap packetizer may choose different MQTT
     * boundaries. This pause is conservative flow control, not framing. */
    vTaskDelay(pdMS_TO_TICKS(s_config.fragment_gap_ms));
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
                memmove(s_ack_stream, s_ack_stream + s_ack_stream_length - keep,
                        keep);
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

static esp_err_t wait_for_ack(const telemetry_fragment_plan_t *plan)
{
    TickType_t timeout = pdMS_TO_TICKS(s_config.ack_timeout_ms);
    TickType_t started = xTaskGetTickCount();
    uint8_t incoming[TELEMETRY_UART_DRAIN_SIZE];
    while (xTaskGetTickCount() - started < timeout) {
        telemetry_ack_t ack;
        while (take_next_ack(&ack)) {
            bool matches = ack.source_id == plan->source_id &&
                ack.mission_id == plan->mission_id &&
                ack.message_type == plan->message_type &&
                ack.message_sequence == plan->message_sequence &&
                ack.message_crc32 == plan->payload_crc32;
            if (!matches) {
                taskENTER_CRITICAL(&s_lock);
                s_status.acknowledgement_rejected++;
                taskEXIT_CRITICAL(&s_lock);
                continue;
            }
            if (ack.status != 0) {
                taskENTER_CRITICAL(&s_lock);
                s_status.acknowledgement_rejected++;
                taskEXIT_CRITICAL(&s_lock);
                return ESP_ERR_INVALID_RESPONSE;
            }
            taskENTER_CRITICAL(&s_lock);
            s_status.acknowledgements_received++;
            taskEXIT_CRITICAL(&s_lock);
            return ESP_OK;
        }
        TickType_t elapsed = xTaskGetTickCount() - started;
        if (elapsed >= timeout) break;
        TickType_t remaining = timeout - elapsed;
        TickType_t wait = pdMS_TO_TICKS(50);
        if (wait == 0 || wait > remaining) wait = remaining;
        int count = uart_read_bytes(s_config.uart_port, incoming,
                                    sizeof(incoming), wait);
        if (count > 0) append_downlink(incoming, (size_t)count);
        if (count < 0) {
            note_uart_fault();
            return ESP_FAIL;
        }
    }
    taskENTER_CRITICAL(&s_lock);
    s_status.acknowledgement_timeouts++;
    taskEXIT_CRITICAL(&s_lock);
    return ESP_ERR_TIMEOUT;
}

static esp_err_t send_serialized(uint8_t message_type, uint64_t mission_id,
                                 uint32_t message_sequence, size_t length)
{
    telemetry_fragment_plan_t plan;
    esp_err_t result = telemetry_fragment_plan_init(
        &plan, message_type, s_config.source_id, mission_id,
        message_sequence, 0,
        s_record_buffer, length);
    if (result != ESP_OK) {
        note_serialization_fault();
        return result;
    }
    for (unsigned attempt = 0; attempt <= s_config.max_retries; attempt++) {
        bool uart_failed = false;
        result = telemetry_fragment_emit_all(
            &plan, s_fragment_buffer, sizeof(s_fragment_buffer),
            emit_fragment, &uart_failed);
        if (result != ESP_OK && !uart_failed) note_serialization_fault();
        if (result == ESP_OK) result = wait_for_ack(&plan);
        if (result == ESP_OK) return ESP_OK;
        if (attempt < s_config.max_retries) {
            taskENTER_CRITICAL(&s_lock);
            s_status.messages_retried++;
            taskEXIT_CRITICAL(&s_lock);
        }
    }
    return result;
}

static esp_err_t send_gps(const gps_record_t *record, uint64_t mission_id)
{
    size_t length = 0;
    esp_err_t result = data_record_serialize_gps(
        record, s_record_buffer, sizeof(s_record_buffer), &length);
    if (result != ESP_OK) note_serialization_fault();
    if (result == ESP_OK) {
        result = send_serialized(
            TELEMETRY_MESSAGE_GPS, mission_id,
            record->header.record_sequence, length);
    }
    if (result == ESP_OK) {
        taskENTER_CRITICAL(&s_lock);
        s_status.gps_sent++;
        taskEXIT_CRITICAL(&s_lock);
    }
    return result;
}

static esp_err_t send_reflectance(const reflectance_record_t *record,
                                  uint64_t mission_id)
{
    size_t length = 0;
    esp_err_t result = data_record_serialize_reflectance(
        record, s_record_buffer, sizeof(s_record_buffer), &length);
    if (result != ESP_OK) note_serialization_fault();
    if (result == ESP_OK) {
        result = send_serialized(
            TELEMETRY_MESSAGE_REFLECTANCE, mission_id,
            record->header.record_sequence, length);
    }
    if (result == ESP_OK) {
        taskENTER_CRITICAL(&s_lock);
        s_status.reflectance_sent++;
        taskEXIT_CRITICAL(&s_lock);
    }
    return result;
}

static void telemetry_task(void *unused)
{
    (void)unused;
    static gps_record_t gps;
    static reflectance_record_t reflectance;
    TickType_t last_gps = 0;
    bool gps_sent_once = false;
    const TickType_t gps_interval =
        pdMS_TO_TICKS(s_config.gps_min_interval_ms);

    while (true) {
        /* Mark the entire dequeue/send phase active before removing a queued
         * item, so finish_mission cannot observe an empty queue in the tiny
         * interval before physical transmission begins. */
        taskENTER_CRITICAL(&s_lock);
        s_sending = true;
        taskEXIT_CRITICAL(&s_lock);
        TickType_t now = xTaskGetTickCount();
        bool gps_due = !gps_sent_once || now - last_gps >= gps_interval;
        if (gps_due && xQueueReceive(s_gps_queue, &gps, 0) == pdTRUE) {
            taskENTER_CRITICAL(&s_lock);
            uint64_t mission_id = s_status.mission_id;
            taskEXIT_CRITICAL(&s_lock);
            esp_err_t result = send_gps(&gps, mission_id);
            taskENTER_CRITICAL(&s_lock);
            s_sending = false;
            taskEXIT_CRITICAL(&s_lock);
            last_gps = xTaskGetTickCount();
            gps_sent_once = true;
            if (result != ESP_OK) {
                note_message_failure();
                ESP_LOGE(TAG, "GPS transmission failed: %s",
                         esp_err_to_name(result));
            }
            continue;
        }
        if (xQueueReceive(s_reflectance_queue, &reflectance, 0) == pdTRUE) {
            taskENTER_CRITICAL(&s_lock);
            uint64_t mission_id = s_status.mission_id;
            taskEXIT_CRITICAL(&s_lock);
            esp_err_t result = send_reflectance(&reflectance, mission_id);
            taskENTER_CRITICAL(&s_lock);
            s_sending = false;
            taskEXIT_CRITICAL(&s_lock);
            if (result != ESP_OK) {
                note_message_failure();
                ESP_LOGE(TAG, "Reflectance transmission failed: %s",
                         esp_err_to_name(result));
            }
            continue;
        }
        uint8_t incoming[TELEMETRY_UART_DRAIN_SIZE];
        int count = uart_read_bytes(s_config.uart_port, incoming,
                                    sizeof(incoming), 0);
        if (count > 0) append_downlink(incoming, (size_t)count);
        if (count < 0) note_uart_fault();
        taskENTER_CRITICAL(&s_lock);
        s_sending = false;
        taskEXIT_CRITICAL(&s_lock);
        vTaskDelay(pdMS_TO_TICKS(TELEMETRY_POLL_MS));
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
    if (xQueueSend(s_reflectance_queue, record, 0) != pdPASS) {
        taskENTER_CRITICAL(&s_lock);
        uint32_t dropped = ++s_status.reflectance_queue_overflows;
        taskEXIT_CRITICAL(&s_lock);
        if (dropped == 1 || dropped % 100U == 0) {
            ESP_LOGW(TAG, "Reflectance telemetry queue full; dropped=%lu",
                     (unsigned long)dropped);
        }
        return ESP_ERR_NO_MEM;
    }
    taskENTER_CRITICAL(&s_lock);
    s_status.reflectance_submitted++;
    taskEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t telemetry_start(const telemetry_config_t *config)
{
    if (config == NULL || config->uart_port < UART_NUM_0 ||
        config->uart_port >= UART_NUM_MAX || config->tx_gpio < 0 ||
        config->rx_gpio < 0 || config->tx_gpio == config->rx_gpio ||
        config->baud_rate == 0 || config->fragment_gap_ms < 6 ||
        config->gps_min_interval_ms == 0 || config->source_id == 0 ||
        config->ack_timeout_ms == 0 || config->max_retries > 3 ||
        pdMS_TO_TICKS(config->fragment_gap_ms) == 0 ||
        pdMS_TO_TICKS(config->gps_min_interval_ms) == 0 ||
        pdMS_TO_TICKS(config->ack_timeout_ms) == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_task != NULL) return ESP_ERR_INVALID_STATE;

    s_gps_queue = xQueueCreate(1, sizeof(gps_record_t));
    s_reflectance_queue = xQueueCreate(TELEMETRY_REFLECTANCE_QUEUE_LENGTH,
                                       sizeof(reflectance_record_t));
    if (s_gps_queue == NULL || s_reflectance_queue == NULL) {
        if (s_gps_queue != NULL) vQueueDelete(s_gps_queue);
        if (s_reflectance_queue != NULL) vQueueDelete(s_reflectance_queue);
        s_gps_queue = s_reflectance_queue = NULL;
        return ESP_ERR_NO_MEM;
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
    if (result == ESP_OK) result = uart_param_config(config->uart_port,
                                                     &uart_config);
    if (result == ESP_OK) {
        result = uart_set_pin(config->uart_port, config->tx_gpio,
                              config->rx_gpio, UART_PIN_NO_CHANGE,
                              UART_PIN_NO_CHANGE);
    }
    if (result != ESP_OK) {
        if (driver_installed) (void)uart_driver_delete(config->uart_port);
        vQueueDelete(s_gps_queue);
        vQueueDelete(s_reflectance_queue);
        s_gps_queue = s_reflectance_queue = NULL;
        return result;
    }

    s_config = *config;
    taskENTER_CRITICAL(&s_lock);
    memset(&s_status, 0, sizeof(s_status));
    s_status.initialized = true;
    s_status.healthy = true;
    s_status.source_id = config->source_id;
    s_accepting = false;
    s_sending = false;
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
        (void)uart_driver_delete(config->uart_port);
        vQueueDelete(s_gps_queue);
        vQueueDelete(s_reflectance_queue);
        s_gps_queue = s_reflectance_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG,
             "DTU telemetry UART%d TX=GPIO%d RX=GPIO%d %lu 8N1; gap=%lums "
             "GPS interval=%lums source=%016llX ACK=%lums retries=%u",
             config->uart_port, config->tx_gpio, config->rx_gpio,
             (unsigned long)config->baud_rate,
             (unsigned long)config->fragment_gap_ms,
             (unsigned long)config->gps_min_interval_ms,
             (unsigned long long)config->source_id,
             (unsigned long)config->ack_timeout_ms, config->max_retries);
    return ESP_OK;
}

esp_err_t telemetry_begin_mission(uint64_t mission_id)
{
    if (mission_id == 0) return ESP_ERR_INVALID_ARG;
    if (s_task == NULL) return ESP_ERR_INVALID_STATE;
    taskENTER_CRITICAL(&s_lock);
    /* Before the only mission of this boot, the worker may transiently mark
     * its empty dequeue window as sending. mission_id/admission/submitters are
     * the authoritative ownership guards here. */
    bool available = s_status.mission_id == 0 && !s_accepting &&
                     s_submitters == 0;
    taskEXIT_CRITICAL(&s_lock);
    if (!available || uxQueueMessagesWaiting(s_gps_queue) != 0 ||
        uxQueueMessagesWaiting(s_reflectance_queue) != 0) {
        return ESP_ERR_INVALID_STATE;
    }
    xQueueReset(s_gps_queue);
    xQueueReset(s_reflectance_queue);
    taskENTER_CRITICAL(&s_lock);
    bool healthy = s_status.healthy;
    uint32_t serialization_errors = s_status.serialization_errors;
    uint32_t uart_errors = s_status.uart_errors;
    memset(&s_status, 0, sizeof(s_status));
    s_status.initialized = true;
    s_status.healthy = healthy;
    /* Preserve infrastructure evidence detected by the idle UART owner before
     * the recorder allocates and binds the flight. Mission counters start at 0. */
    s_status.serialization_errors = serialization_errors;
    s_status.uart_errors = uart_errors;
    s_status.source_id = s_config.source_id;
    s_status.mission_id = mission_id;
    s_accepting = true;
    s_sending = false;
    s_submitters = 0;
    taskEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t telemetry_submit_gps(const gps_record_t *record)
{
    if (record == NULL) return ESP_ERR_INVALID_ARG;
    taskENTER_CRITICAL(&s_lock);
    bool active = s_task != NULL && s_accepting &&
                  s_status.mission_id != 0;
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
    bool active = s_task != NULL && s_accepting &&
                  s_status.mission_id != 0;
    if (active) s_submitters++;
    taskEXIT_CRITICAL(&s_lock);
    if (!active) return ESP_ERR_INVALID_STATE;
    esp_err_t result = enqueue_reflectance(record);
    taskENTER_CRITICAL(&s_lock);
    s_submitters--;
    taskEXIT_CRITICAL(&s_lock);
    return result;
}

esp_err_t telemetry_finish_mission(uint32_t timeout_ms)
{
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    if (timeout_ms == 0 || timeout_ticks == 0) return ESP_ERR_INVALID_ARG;
    taskENTER_CRITICAL(&s_lock);
    if (s_task == NULL || s_status.mission_id == 0) {
        taskEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_accepting = false;
    taskEXIT_CRITICAL(&s_lock);

    TickType_t started = xTaskGetTickCount();
    while (true) {
        taskENTER_CRITICAL(&s_lock);
        bool sending = s_sending;
        bool submitters = s_submitters != 0;
        taskEXIT_CRITICAL(&s_lock);
        bool empty = uxQueueMessagesWaiting(s_gps_queue) == 0 &&
                     uxQueueMessagesWaiting(s_reflectance_queue) == 0;
        if (empty && !sending && !submitters) return ESP_OK;
        if (xTaskGetTickCount() - started >= timeout_ticks) {
            taskENTER_CRITICAL(&s_lock);
            s_status.drain_timeouts++;
            taskEXIT_CRITICAL(&s_lock);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }
}

void telemetry_get_status(telemetry_status_t *out)
{
    if (out == NULL) return;
    taskENTER_CRITICAL(&s_lock);
    *out = s_status;
    taskEXIT_CRITICAL(&s_lock);
}
