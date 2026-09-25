/* Executes the production C checker with deterministic time/UART, no hardware. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "boot_health.h"
#include "dtu_boot_check.h"

static int scenario, queries, exits, writes;
static int64_t now;
static char response[1024], command[256];
static size_t offset;
void mock_log(const char *tag, const char *format, ...) { (void)tag; (void)format; }
int64_t esp_timer_get_time(void) { return now; }
void vTaskDelay(TickType_t ticks) { now += (int64_t)ticks * 1000; }
static bool cancelled(void) { return scenario == 6 && now >= 2000000; }
static void reply(const char *text) { snprintf(response, sizeof(response), "%s", text); offset = 0; }
esp_err_t uart_flush_input(uart_port_t port) { (void)port; response[0] = 0; offset = 0; return ESP_OK; }
esp_err_t uart_wait_tx_done(uart_port_t port, TickType_t ticks) { (void)port; (void)ticks; return ESP_OK; }
int uart_read_bytes(uart_port_t port, void *data, uint32_t length, TickType_t ticks)
{
    (void)port;
    now += (int64_t)ticks * 1000;
    size_t available = strlen(response) - offset;
    /* Split every response into tiny chunks to exercise stream assembly. */
    size_t chunk = scenario == 7 ? 128 : 3;
    if (available > chunk) available = chunk;
    if (available > length) available = length;
    memcpy(data, response + offset, available);
    offset += available;
    return (int)available;
}
int uart_write_bytes(uart_port_t port, const void *data, size_t length)
{
    (void)port;
    ++writes;
    if (length == 3 && !memcmp(data, "+++", 3)) return 3;
    if (length == 1 && !memcmp(data, "a", 1)) {
        if (scenario != 4)
            reply(scenario == 10 ? "a+ok\r\n" : scenario == 11 ? "a+OK\r\n" : "\r\n+ok\r\n");
        return 1;
    }
    if (length != 2 || memcmp(data, "\r\n", 2)) {
        assert(length < sizeof(command));
        memcpy(command, data, length); command[length] = 0;
        /* All queries are read-only; no persistent setter may sneak in. */
        assert(strchr(command, '=') == NULL || !strcmp(command, "AT+SOCKLK=1A"));
        assert(strstr(command, "MQAUTH") == NULL);
        return (int)length;
    }
    if (!strcmp(command, "AT+EXIT")) {
        ++exits;
        if (scenario != 5 && scenario != 4) reply("\r\nOK\r\n");
    } else if (!strcmp(command, "AT+UART1")) {
        ++queries;
        reply(scenario == 1 ? "+UART1:115200,8,1,NONE,485\r\nOK\r\n" :
                             "+UART1:460800,8,1,NONE,485\r\nOK\r\n");
    } else if (!strcmp(command, "AT+SOCK1A")) {
        ++queries;
        if (scenario == 7) { memset(response, 'x', 900); response[900] = 0; offset = 0; }
        else reply("AT+SOCK1A\r\n+SOCK1A:MQTT,mqtt.example.com,1883\r\nOK\r\n");
    } else if (!strcmp(command, "AT+MQPUB1")) {
        ++queries;
        reply("+MQPUB1:1,dji-h1/test/up,1,0\r\nOK\r\n");
    } else if (!strcmp(command, "AT+SOCKLK=1A")) {
        bool offline = scenario == 2 || scenario == 6 || (scenario == 3 && now < 4000000);
        reply(offline ? "+SOCKLK:OFF\r\nOK\r\n" : "+SOCKLK:ON\r\nOK\r\n");
    } else { assert(!"Unexpected command"); }
    return (int)length;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    scenario = atoi(argv[1]);
    if (scenario == 9) {
        char json[1024];
        assert(!boot_health_critical_failed());
        assert(!boot_health_telemetry_failed());
        boot_health_result(BOOT_DTU_PROFILE, ESP_FAIL);
        assert(!boot_health_critical_failed() && boot_health_telemetry_failed());
        boot_health_result(BOOT_SD, ESP_FAIL);
        assert(boot_health_critical_failed());
        boot_health_set(BOOT_H1_A, BOOT_BLOCKED, ESP_ERR_INVALID_STATE);
        assert(boot_health_json(json, sizeof(json)) == ESP_OK);
        assert(strstr(json, "\"h1_ground\":[\"blocked\",259]"));
        assert(boot_health_json(json, 20) == ESP_ERR_NO_MEM && json[0] == 0);
        assert(boot_health_json(NULL, 20) == ESP_ERR_INVALID_ARG);
        boot_health_report();
        return 0;
    }
    telemetry_config_t config = {1, 17, 18, scenario == 8 ? 115200U : 460800U};
    bool allowed = dtu_boot_check(&config, cancelled);
    boot_health_snapshot_t health;
    boot_health_snapshot(&health);
    bool expected = scenario == 0 || scenario == 2 || scenario == 3 || scenario >= 10;
    assert(allowed == expected);
    assert(now <= 20800000); /* global check budget plus bounded cleanup */
    assert(exits == (scenario == 8 ? 0 : 1));
    if (scenario == 8) assert(writes == 0);
    if (scenario == 6) assert(now < 3000000);
    assert(health.checks[BOOT_DTU_PROFILE].state == (expected ? BOOT_PASS : BOOT_FAIL));
    assert(health.checks[BOOT_GROUND_ACK].state == (expected ? BOOT_WAITING : BOOT_BLOCKED));
    assert(health.checks[BOOT_DTU_NETWORK].state ==
           (!expected ? BOOT_BLOCKED : scenario == 2 ? BOOT_FAIL : BOOT_PASS));
    printf("scenario %d passed in simulated %lld ms\n", scenario, (long long)(now / 1000));
    return 0;
}
