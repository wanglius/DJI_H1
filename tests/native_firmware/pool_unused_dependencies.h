/* Some COFF linkers resolve even unreachable references in the included C
 * translation unit. These guards satisfy those references, but must NEVER be
 * used by the pool tests: calling one fails the test process immediately.
 * None implements or copies modem/serializer logic. */
#define UNREACHED(type, name, args) type name args { abort(); }
UNREACHED(void *, heap_caps_calloc, (size_t n, size_t s, unsigned caps))
UNREACHED(void, heap_caps_free, (void *p))
UNREACHED(QueueHandle_t, xQueueCreate, (unsigned n, unsigned s))
UNREACHED(void, vQueueDelete, (QueueHandle_t q))
UNREACHED(unsigned, uxQueueMessagesWaiting, (QueueHandle_t q))
UNREACHED(int, xQueueReset, (QueueHandle_t q))
UNREACHED(TickType_t, xTaskGetTickCount, (void))
UNREACHED(void, vTaskDelay, (TickType_t ticks))
UNREACHED(BaseType_t, xTaskCreatePinnedToCore,
    (void (*fn)(void *), const char *name, unsigned size, void *arg,
     unsigned priority, TaskHandle_t *handle, int core))
UNREACHED(esp_err_t, uart_driver_install, (uart_port_t p, int a, int b, int c, void *q, int f))
UNREACHED(esp_err_t, uart_driver_delete, (uart_port_t p))
UNREACHED(esp_err_t, uart_param_config, (uart_port_t p, const uart_config_t *c))
UNREACHED(esp_err_t, uart_set_pin, (uart_port_t p, int tx, int rx, int rts, int cts))
UNREACHED(esp_err_t, uart_wait_tx_done, (uart_port_t p, TickType_t t))
UNREACHED(int, uart_read_bytes, (uart_port_t p, void *out, unsigned n, TickType_t t))
UNREACHED(int, uart_write_bytes, (uart_port_t p, const void *in, size_t n))
UNREACHED(void, m100m_init, (uart_port_t p, uint32_t b, uint64_t id,
    const m100m_config_t *c, m100m_ack_fn ack, bool (*cancel)(void)))
UNREACHED(bool, m100m_ready, (void))
UNREACHED(void, m100m_poll, (void))
UNREACHED(esp_err_t, m100m_publish, (const uint8_t *p, size_t n))
UNREACHED(void, gps_batch_reset, (gps_batch_t *b))
UNREACHED(esp_err_t, gps_batch_append, (gps_batch_t *b, const gps_record_t *r))
UNREACHED(bool, gps_batch_is_full, (const gps_batch_t *b, uint16_t n))
UNREACHED(uint64_t, gps_batch_first_timestamp_us, (const gps_batch_t *b))
UNREACHED(esp_err_t, gps_batch_serialize, (const gps_batch_t *b, void *o, size_t c, size_t *n))
UNREACHED(esp_err_t, data_record_serialize_reflectance,
    (const reflectance_record_t *r, void *o, size_t c, size_t *n))
UNREACHED(esp_err_t, data_record_serialize_operation_event,
    (const operation_event_record_t *r, void *o, size_t c, size_t *n))
UNREACHED(uint32_t, telemetry_crc32, (const void *p, size_t n))
UNREACHED(esp_err_t, telemetry_ack_decode, (const uint8_t *p, size_t n, telemetry_ack_t *a))
UNREACHED(esp_err_t, telemetry_message_encode,
    (uint8_t t, uint64_t s, uint64_t m, uint32_t seq, const uint8_t *p,
     size_t n, uint8_t *out, size_t cap, size_t *written))
UNREACHED(esp_err_t, telemetry_fragment_plan_init,
    (telemetry_fragment_plan_t *p, uint8_t t, uint64_t s, uint64_t m,
     uint32_t seq, uint16_t f, const void *in, size_t n))
UNREACHED(esp_err_t, telemetry_fragment_emit_all,
    (const telemetry_fragment_plan_t *p, uint8_t *s, size_t n,
     telemetry_fragment_emit_fn emit, void *ctx))
#undef UNREACHED
