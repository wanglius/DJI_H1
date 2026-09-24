/* Compile the real driver in this translation unit: no copied state machine.
 * UART bytes and monotonic time are controlled; static state is fixture-only. */
#include "../../components/m100m/m100m.c"
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)
static int64_t now_us, cancel_at;
static unsigned writes, ack_calls, scenario;
static bool prompt_pending, completion_pending;
static unsigned rx_chunk;
static uint8_t rx[256];
static size_t rx_len, rx_pos;
static const m100m_config_t cfg = {"broker", "", "", "up", "down", 1883};

int64_t esp_timer_get_time(void) { return now_us; }
void vTaskDelay(TickType_t ticks) { now_us += ticks * 1000; }
static bool cancelled(void) { return cancel_at && now_us >= cancel_at; }
static void ack(const uint8_t *p, size_t n) { if (n == 40 && p[0] == 'D') ack_calls++; }
esp_err_t uart_wait_tx_done(uart_port_t p, TickType_t t) { return ESP_OK; }
esp_err_t uart_set_baudrate(uart_port_t p, uint32_t b) { return ESP_OK; }
esp_err_t uart_flush_input(uart_port_t p) { return ESP_OK; }
int uart_write_bytes(uart_port_t p, const void *data, size_t n)
{
    writes++;
    if (writes == 1) prompt_pending = true;
    else if (writes == 2) completion_pending = true;
    return scenario == 6 && writes == 2 ? (int)n - 1 : (int)n;
}
int uart_read_bytes(uart_port_t p, void *out, unsigned capacity, TickType_t t)
{
    const char *text = NULL;
    if (rx_pos < rx_len) {
        size_t n = rx_len-rx_pos;
        if (n > capacity) n = capacity;
        if (rx_chunk && n > rx_chunk) n = rx_chunk;
        memcpy(out, rx+rx_pos, n); rx_pos += n; return (int)n;
    }
    if (prompt_pending) {
        prompt_pending = false;
        if (scenario == 4) text = "\r\nERROR\r\n";
        else if (scenario == 8) text = ">\r\n+MSUB: \"down\",4101 byte,";
        else if (scenario != 3) text = ">";
    } else if (completion_pending) {
        completion_pending = false;
        if (scenario == 1) text = "\r\nOK\r\nPUBACK\r\n";
        if (scenario == 5) text = "\r\n+CME ERROR: 767\r\n";
        if (scenario == 7) text = "\r\nOK\r\n";
    }
    if (!text) return 0;
    size_t n = strlen(text); memcpy(out, text, n); return (int)n;
}
static void reset_case(unsigned which)
{
    now_us = 1; cancel_at = 0; writes = ack_calls = 0; scenario = which;
    prompt_pending = completion_pending = false;
    rx_len = rx_pos = rx_chunk = 0;
    s_used = s_binary_left = s_binary_used = 0;
    s_ack_topic = s_quarantined = s_waiting = s_matched = s_error = false;
    s_busy = s_publishing = s_attached = s_fallback = false;
    s_expected = NULL; s_step = 14;
    s_deadline = s_retry_at = s_puback_deadline = s_rx_deadline = 0;
    m100m_init(1, 460800, 11, &cfg, ack, cancelled);
}

static int modem_case(unsigned which)
{
    reset_case(which);
    uint8_t payload[2273] = {0};
    if (which == 2) cancel_at = 10001; /* Abort during post-payload wait. */
    esp_err_t result = m100m_publish(payload, sizeof(payload));
    if (which == 1) {
        CHECK(result == ESP_OK && m100m_ready() && writes == 2);
    } else if (which == 4 || which == 5) {
        CHECK(result == ESP_FAIL && !s_quarantined && s_step == 0);
        now_us += 6000000; m100m_poll();
        CHECK(writes == (which == 4 ? 2 : 3));
    } else if (which == 7) {
        CHECK(result == ESP_OK && s_busy && !m100m_ready());
        now_us += 11000000; m100m_poll();
        CHECK(!s_quarantined && s_step == 0); /* PUBACK loss: command mode known. */
    } else {
        CHECK(result != ESP_OK && s_quarantined && !m100m_ready());
        if (which == 8) CHECK(writes == 1); /* Match before quarantine is not authorization. */
        unsigned before = writes;
        now_us += 60000000; m100m_poll();
        CHECK(writes == before); /* No reconnect AT bytes in ambiguous input. */
        if (which == 2) CHECK(now_us - 60000000 < 50000);
    }
    return 0;
}

static int modem_binary_case(unsigned chunk)
{
    reset_case(0);
    const char *header = "+MSUB: \"down\",40 byte,";
    rx_len = strlen(header); memcpy(rx, header, rx_len);
    memset(rx+rx_len, 0, 40);
    memcpy(rx+rx_len, "D\r\nOK\r\n>\r\n", 10); rx_len += 40;
    memcpy(rx+rx_len, "\r\nPUBACK\r\n", 10); rx_len += 10;
    rx_chunk = chunk; s_busy = true; s_puback_deadline = 1000000;
    while (rx_pos < rx_len) m100m_poll();
    CHECK(ack_calls == 1 && !s_busy && !s_quarantined);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    int line = atoi(argv[1]) ? modem_binary_case(atoi(argv[2])) : modem_case(atoi(argv[2]));
    if (line) fprintf(stderr, "C assertion failed at line %d\n", line);
    return line ? 1 : 0;
}
