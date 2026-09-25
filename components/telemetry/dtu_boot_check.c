#include "dtu_boot_check.h"
#include <string.h>
#include "boot_health.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "dtu_boot_profile.h"

static const char *TAG = "DTU_CHECK";
/* Sole-task scratch storage: bounded even with UART noise or unsolicited URCs.
 * Responses are never logged (modem echoes may contain sensitive text). */
static char s_response[512];

static bool has_line(const char *text, const char *expected)
{
    size_t length = strlen(expected);
    while (*text) {
        while (*text == '\r' || *text == '\n') ++text;
        const char *end = text + strcspn(text, "\r\n");
        if ((size_t)(end - text) == length && !memcmp(text, expected, length))
            return true;
        text = end;
    }
    return false;
}

static bool pause_until(int64_t deadline, bool (*cancelled)(void))
{
    while (esp_timer_get_time() < deadline) {
        if (cancelled && cancelled()) return false;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return !(cancelled && cancelled());
}

static bool entry_confirmed(const char *text)
{
    /* YY-M200 emits an unterminated 'a' challenge after +++. It remains in
     * the RX ring while we wait 500 ms, so the actual reply is "a+ok", not
     * necessarily a standalone "+ok" line. Accept only these exact tokens,
     * including firmware variants that capitalize OK; never a substring of
     * arbitrary downlink data. Query readbacks still require their own lines. */
    return has_line(text, "+ok") || has_line(text, "a+ok") ||
           has_line(text, "+OK") || has_line(text, "a+OK");
}

static bool send(uart_port_t port, const char *text)
{
    size_t length = strlen(text);
    return uart_write_bytes(port, text, length) == (int)length &&
           uart_wait_tx_done(port, pdMS_TO_TICKS(100)) == ESP_OK;
}

static bool receive(uart_port_t port, int64_t deadline, bool entry,
                    bool (*cancelled)(void))
{
    size_t used = 0;
    memset(s_response, 0, sizeof(s_response));
    while (esp_timer_get_time() < deadline) {
        if (cancelled && cancelled()) return false;
        int count = uart_read_bytes(port, s_response + used,
            sizeof(s_response) - 1 - used, pdMS_TO_TICKS(10));
        if (count < 0) return false;
        used += (size_t)count;
        s_response[used] = '\0';
        if (has_line(s_response, "ERROR")) return false;
        if (entry ? entry_confirmed(s_response) : has_line(s_response, "OK"))
            return true;
        if (used == sizeof(s_response) - 1)
            return false;
    }
    return false;
}

static bool query(uart_port_t port, const char *command, int64_t deadline,
                  bool (*cancelled)(void))
{
    if (esp_timer_get_time() >= deadline || (cancelled && cancelled())) return false;
    uart_flush_input(port);
    if (!send(port, command) || !send(port, "\r\n")) return false;
    int64_t command_deadline = esp_timer_get_time() + 600000;
    if (command_deadline > deadline) command_deadline = deadline;
    return receive(port, command_deadline, false, cancelled);
}

bool dtu_boot_check(const telemetry_config_t *config, bool (*cancelled)(void))
{
    uart_port_t port = config->uart_port;
    /* AT checks have a finite total budget, independent of network availability.
     * Shutdown cancellation skips polling; only bounded AT exit is attempted. */
    int64_t deadline = esp_timer_get_time() + 20000000;
    esp_err_t profile = ESP_FAIL;
    bool connected = false;
    bool attempted_entry = false;
    bool exited = true;
    boot_health_set(BOOT_DTU_NETWORK, BOOT_WAITING, ESP_OK);
    boot_health_set(BOOT_GROUND_ACK, BOOT_WAITING, ESP_OK);
    if (config->baud_rate != DTU_PROFILE_BAUD ||
        config->tx_gpio != DTU_PROFILE_TX_GPIO || config->rx_gpio != DTU_PROFILE_RX_GPIO) {
        ESP_LOGE(TAG, "Compiled DTU profile does not match board UART configuration");
        profile = ESP_ERR_INVALID_ARG;
        goto finish;
    }
    if (!pause_until(esp_timer_get_time() + 1200000, cancelled)) goto finish;
    uart_flush_input(port);
    attempted_entry = true;
    if (!send(port, "+++") ||
        !pause_until(esp_timer_get_time() + 500000, cancelled) || !send(port, "a") ||
        !receive(port, esp_timer_get_time() + 600000, true, cancelled)) {
        ESP_LOGE(TAG, "No AT entry confirmation at configured baud; check DTU power/wiring/baud");
        profile = ESP_ERR_TIMEOUT;
        goto finish;
    }
    profile = ESP_OK;
    for (size_t i = 0; i < sizeof(s_profile) / sizeof(s_profile[0]); ++i) {
        bool matched = query(port, s_profile[i].query, deadline, cancelled) &&
                       has_line(s_response, s_profile[i].expected);
        if (!matched) {
            ESP_LOGE(TAG, "%s readback failed or mismatched (value withheld)", s_profile[i].query);
            profile = ESP_FAIL;
        }
        if (esp_timer_get_time() >= deadline || cancelled()) break;
    }
    if (profile == ESP_OK) {
        while (!cancelled() && esp_timer_get_time() < deadline) {
            if (query(port, "AT+SOCKLK=1A", deadline, cancelled) &&
                has_line(s_response, "+SOCKLK:ON")) {
                connected = true;
                break;
            }
            int64_t until = esp_timer_get_time() + 250000;
            if (until > deadline) until = deadline;
            (void)pause_until(until, cancelled);
        }
    }
finish:
    /* Even uncertain entry must attempt EXIT. Never send payloads unless the
     * modem has positively confirmed returning from AT mode. */
    if (attempted_entry)
        exited = query(port, "AT+EXIT", esp_timer_get_time() + 600000, NULL);
    if (!exited) {
        ESP_LOGE(TAG, "AT exit unconfirmed; telemetry disabled until reboot");
        profile = ESP_ERR_INVALID_STATE;
    }
    if (cancelled()) profile = ESP_ERR_INVALID_STATE;
    boot_health_result(BOOT_DTU_PROFILE, profile);
    boot_health_set(BOOT_DTU_NETWORK,
        profile != ESP_OK ? BOOT_BLOCKED : connected ? BOOT_PASS : BOOT_FAIL,
        profile != ESP_OK ? profile : connected ? ESP_OK : ESP_ERR_TIMEOUT);
    if (profile != ESP_OK)
        boot_health_set(BOOT_GROUND_ACK, BOOT_BLOCKED, profile);
    /* A modem can register later. A correct profile with confirmed EXIT may
     * transmit despite an initial network timeout; a matching DTA1 proves recovery. */
    return profile == ESP_OK;
}
