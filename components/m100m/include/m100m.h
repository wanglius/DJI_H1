#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "driver/uart.h"
#include "esp_err.h"

/* Sole caller/owner: telemetry task. Strings must outlive that task.
 * No reset GPIO: ambiguous AT data-entry state is quarantined until power cycle. */
typedef struct {
    const char *host, *username, *password, *uplink_topic, *ack_topic;
    uint16_t port;
} m100m_config_t;
typedef void (*m100m_ack_fn)(const uint8_t *, size_t);
void m100m_init(uart_port_t uart, uint32_t baud, uint64_t source,
                const m100m_config_t *config, m100m_ack_fn ack,
                bool (*cancelled)(void));
void m100m_poll(void);
bool m100m_ready(void);
esp_err_t m100m_publish(const uint8_t *payload, size_t length);
