#include "m100m.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "M100M";
static uart_port_t s_uart;
static uint32_t s_baud;
static uint64_t s_source;
static m100m_config_t s_config;
static m100m_ack_fn s_ack;
static bool (*s_cancelled)(void);
static char s_line[512], s_command[768];
static size_t s_used, s_binary_left, s_binary_used;
static uint8_t s_binary[40];
static bool s_ack_topic, s_quarantined, s_waiting, s_matched, s_error;
static bool s_busy, s_publishing, s_attached, s_fallback;
static const char *s_expected;
static unsigned s_step;
static int64_t s_deadline, s_retry_at, s_puback_deadline, s_rx_deadline;

static void quarantine(const char *reason)
{
    s_quarantined = true;
    ESP_LOGE(TAG, "%s; DTU power cycle required (RST not wired)", reason);
}

static void reconnect(void)
{
    s_step = 0;
    s_waiting = false;
    s_expected = NULL;
    s_matched = false;
    s_busy = false;
    s_retry_at = esp_timer_get_time() + 5000000;
    ESP_LOGW(TAG, "Link unavailable; retry in 5 s; acquisition/SD unaffected");
}

static void line_received(void)
{
    s_line[s_used] = 0;
    if (!strcmp(s_line, "PUBACK")) s_busy = false;
    if (!strcmp(s_line, "+CGATT: 1")) s_attached = true;
    if (s_expected && !strcmp(s_line, s_expected)) s_matched = true;
    if (!strcmp(s_line, "ERROR") || strstr(s_line, "+CME ERROR:") ||
        strstr(s_line, "FAIL")) s_error = true;
    if (strstr(s_line, "CLOSED") || !strcmp(s_line, "DISCONNECT")) {
        if (s_step >= 12) reconnect();
    }
    /* Never log arbitrary AT lines: command echo may contain credentials. */
    if (!strncmp(s_line, "AirM2M_", 8)) ESP_LOGI(TAG, "Firmware: %s", s_line);
    s_used = 0;
}

static void receive(void)
{
    uint8_t incoming[256];
    /* Bounded work even under continuous unsolicited traffic. */
    for (unsigned pass = 0; pass < 8; ++pass) {
        int n = uart_read_bytes(s_uart, incoming, sizeof(incoming), 0);
        if (n <= 0) break;
        for (int i = 0; i < n; ++i) {
            uint8_t ch = incoming[i];
            if (s_binary_left) {
                if (s_binary_used < sizeof(s_binary))
                    s_binary[s_binary_used] = ch;
                s_binary_used++;
                if (--s_binary_left == 0 && s_ack_topic &&
                    s_binary_used == sizeof(s_binary)) s_ack(s_binary, sizeof(s_binary));
                continue;
            }
            if (ch == '>' && s_used == 0) {
                if (s_expected && !strcmp(s_expected, ">")) s_matched = true;
                else quarantine("Unexpected data prompt");
                continue;
            }
            if (ch == '\r') continue;
            if (ch == '\n') { if (s_used) line_received(); continue; }
            if (s_used == 0 && ch == ' ') continue;
            if (s_used + 1 >= sizeof(s_line)) {
                if (s_step == 0) { s_used = 0; continue; } /* probing baud */
                quarantine("Oversized UART header"); return;
            }
            s_line[s_used++] = (char)ch;
            s_line[s_used] = 0;
            /* +MSUB binary is length framed, never scanned for CR/LF/OK. */
            if (ch == ',' && !strncmp(s_line, "+MSUB:", 6)) {
                char topic[192]; unsigned count; int consumed = 0;
                if (sscanf(s_line, "+MSUB: \"%191[^\"]\",%u byte,%n",
                           topic, &count, &consumed) == 2 && consumed > 0 &&
                    (size_t)consumed == s_used) {
                    if (count > 4100) { quarantine("Oversized downlink"); return; }
                    s_ack_topic = !strcmp(topic, s_config.ack_topic);
                    s_binary_left = count;
                    s_binary_used = 0;
                    s_used = 0;
                    s_rx_deadline = esp_timer_get_time() + 3000000;
                }
            }
        }
    }
    if (s_binary_left && esp_timer_get_time() > s_rx_deadline)
        quarantine("Truncated binary downlink");
}

static void command(const char *expected, unsigned timeout_ms)
{
    s_expected = expected; s_matched = false; s_error = false;
    s_deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    s_waiting = true;
    size_t length = strlen(s_command);
    if (uart_write_bytes(s_uart, s_command, length) != (int)length)
        quarantine("Short AT command write");
}

void m100m_init(uart_port_t uart, uint32_t baud, uint64_t source,
                const m100m_config_t *config, m100m_ack_fn ack,
                bool (*cancelled)(void))
{
    s_uart = uart; s_baud = baud; s_source = source;
    s_config = *config; s_ack = ack; s_cancelled = cancelled;
    ESP_LOGI(TAG, "Native AT MQTT, no RST GPIO; QoS1 up/QoS0 DTA1 down");
}

bool m100m_ready(void)
{
    return s_step == 14 && !s_busy && !s_quarantined && !s_cancelled();
}

void m100m_poll(void)
{
    if (s_quarantined || s_cancelled()) return;
    receive();
    if (s_quarantined || s_publishing) return;
    int64_t now = esp_timer_get_time();
    if (s_step == 14) {
        if (s_busy && now > s_puback_deadline) reconnect();
        return;
    }
    if (s_waiting) {
        bool cleanup = s_step == 5 || s_step == 6;
        if (s_matched || (cleanup && s_error)) {
            if (s_step == 3) {
                uart_wait_tx_done(s_uart, pdMS_TO_TICKS(100));
                uart_set_baudrate(s_uart, s_baud);
            }
            if (s_step == 7 && !s_attached) { reconnect(); return; }
            s_step++; s_waiting = false; s_expected = NULL;
            if (s_step == 14) {
                ESP_LOGI(TAG, "MQTT READY: DTM1 single-message transport"); return;
            }
        } else if (s_error || now >= s_deadline) {
            if (s_step == 0) {
                s_fallback = !s_fallback;
                uart_set_baudrate(s_uart, s_fallback ? 115200 : s_baud);
                uart_flush_input(s_uart); s_used = 0;
            }
            ESP_LOGW(TAG, "AT setup step %u failed/timed out", s_step);
            reconnect(); return;
        } else return;
    }
    if (now < s_retry_at) return;
    const char *expected = "OK"; unsigned timeout = 3000;
    switch (s_step) {
    case 0: snprintf(s_command, sizeof(s_command), "AT\r\n"); break;
    case 1: snprintf(s_command, sizeof(s_command), "ATE0\r\n"); break;
    case 2: snprintf(s_command, sizeof(s_command), "ATI\r\n"); break;
    case 3: snprintf(s_command, sizeof(s_command), "AT+IPR=%lu\r\n", (unsigned long)s_baud); break;
    case 4: snprintf(s_command, sizeof(s_command), "AT\r\n"); break;
    case 5: snprintf(s_command, sizeof(s_command), "AT+MDISCONNECT\r\n"); break;
    case 6: snprintf(s_command, sizeof(s_command), "AT+MIPCLOSE\r\n"); break;
    case 7: s_attached = false; snprintf(s_command, sizeof(s_command), "AT+CGATT?\r\n"); break;
    case 8: snprintf(s_command, sizeof(s_command), "AT+MQTTMODE=0\r\n"); break;
    case 9: snprintf(s_command, sizeof(s_command), "AT+MQTTMSGSET=0\r\n"); break;
    case 10: snprintf(s_command, sizeof(s_command),
        "AT+MCONFIG=\"DJI_%016llX\",\"%s\",\"%s\"\r\n",
        (unsigned long long)s_source, s_config.username, s_config.password); break;
    case 11: snprintf(s_command, sizeof(s_command), "AT+MIPSTART=\"%s\",\"%u\"\r\n",
        s_config.host, s_config.port); expected = "CONNECT OK"; timeout = 30000; break;
    case 12:
        /* Connection and subscription are separate asynchronous steps below. */
        snprintf(s_command, sizeof(s_command), "AT+MCONNECT=1,120\r\n");
        expected = "CONNACK OK"; timeout = 15000; break;
    case 13: snprintf(s_command, sizeof(s_command), "AT+MSUB=\"%s\",0\r\n",
        s_config.ack_topic); expected = "SUBACK"; timeout = 15000; break;
    default: return;
    }
    command(expected, timeout);
}

/* Called only while ready; polls binary downlink while awaiting local prompt/OK.
 * It never waits for a ground ACK, and cancellation bounds shutdown latency. */
static esp_err_t wait_response(void)
{
    while (!s_cancelled() && !s_quarantined && esp_timer_get_time() < s_deadline) {
        receive();
        /* receive() may quarantine after an earlier matching line in the same
         * UART burst. Never let that stale match authorize another write. */
        if (s_cancelled() || s_quarantined) return ESP_ERR_TIMEOUT;
        if (s_error) return ESP_FAIL;
        if (s_matched) return ESP_OK;
        vTaskDelay(pdMS_TO_TICKS(10) ? pdMS_TO_TICKS(10) : 1);
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t m100m_publish(const uint8_t *payload, size_t length)
{
    if (!m100m_ready() || !payload || length == 0 || length > 4100)
        return ESP_ERR_INVALID_STATE;
    s_publishing = true;
    snprintf(s_command, sizeof(s_command), "AT+MPUBEX=\"%s\",1,0,%u\r\n",
             s_config.uplink_topic, (unsigned)length);
    command(">", 3000);
    esp_err_t result = wait_response();
    if (result == ESP_OK) {
        s_matched = false; s_error = false; s_expected = "OK";
        s_deadline = esp_timer_get_time() + 3000000;
        s_busy = true; s_puback_deadline = esp_timer_get_time() + 10000000;
        if (uart_write_bytes(s_uart, payload, length) != (int)length) {
            quarantine("Short binary payload write"); result = ESP_FAIL;
        } else {
            result = wait_response();
            if (result == ESP_ERR_TIMEOUT) {
                /* A full local UART write does not prove that the modem saw
                 * every byte. Without its terminal response it may still be
                 * in fixed-length data input; AT reconnect commands would then
                 * become payload. No RST/verified escape is available. */
                quarantine("Publish completion timeout/cancellation");
            }
        }
    } else if (result == ESP_ERR_TIMEOUT) {
        /* A late prompt could consume AT commands as payload. Never guess. */
        quarantine("Publish prompt timeout/cancellation");
    }
    s_expected = NULL; s_waiting = false; s_publishing = false;
    if (result != ESP_OK && !s_quarantined && !s_cancelled()) reconnect();
    return result;
}
