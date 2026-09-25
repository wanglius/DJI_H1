#include "boot_health.h"
#include <stdio.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "BOOT_HEALTH";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static boot_health_snapshot_t s_health;
static const char *const s_names[] = {
    "flash", "psram", "sd_mount", "sd_recorder", "sc16_bus",
    "sc16_a", "sc16_b", "h1_ground", "h1_sky", "telemetry_uart",
    "dtu_profile", "dtu_network", "ground_ack"
};
static const char *const s_states[] = {
    "pending", "pass", "fail", "blocked", "waiting"
};
_Static_assert(sizeof(s_names) / sizeof(s_names[0]) == BOOT_CHECK_COUNT,
               "Every boot check needs a stable diagnostic name");

void boot_health_set(boot_check_t check, boot_check_state_t state, esp_err_t error)
{
    if ((unsigned)check >= BOOT_CHECK_COUNT || (unsigned)state > BOOT_WAITING)
        return;
    taskENTER_CRITICAL(&s_lock);
    bool changed = s_health.checks[check].state != state ||
                   s_health.checks[check].error != error;
    s_health.checks[check] = (boot_check_result_t){state, error};
    taskEXIT_CRITICAL(&s_lock);
    /* Never log while holding the cross-core spinlock. */
    if (changed) {
        if (state == BOOT_FAIL || state == BOOT_BLOCKED)
            ESP_LOGE(TAG, "%s: %s (%s)", s_names[check], s_states[state],
                     esp_err_to_name(error));
        else
            ESP_LOGI(TAG, "%s: %s", s_names[check], s_states[state]);
    }
}

void boot_health_result(boot_check_t check, esp_err_t error)
{
    boot_health_set(check, error == ESP_OK ? BOOT_PASS : BOOT_FAIL, error);
}

void boot_health_snapshot(boot_health_snapshot_t *out)
{
    if (out == NULL) return;
    taskENTER_CRITICAL(&s_lock);
    *out = s_health;
    taskEXIT_CRITICAL(&s_lock);
}

static bool failed_between(boot_check_t first, boot_check_t last)
{
    boot_health_snapshot_t health;
    boot_health_snapshot(&health);
    for (int i = first; i <= (int)last; ++i)
        if (health.checks[i].state == BOOT_FAIL ||
            health.checks[i].state == BOOT_BLOCKED) return true;
    return false;
}

bool boot_health_critical_failed(void)
{
    return failed_between(BOOT_FLASH, BOOT_H1_B);
}

bool boot_health_telemetry_failed(void)
{
    return failed_between(BOOT_TELEMETRY_UART, BOOT_GROUND_ACK);
}

esp_err_t boot_health_json(char *out, size_t capacity)
{
    if (out == NULL || capacity < 3) return ESP_ERR_INVALID_ARG;
    boot_health_snapshot_t health;
    boot_health_snapshot(&health);
    size_t used = 0;
    for (int i = 0; i < BOOT_CHECK_COUNT; ++i) {
        int count = snprintf(out + used, capacity - used,
            "%s\"%s\":[\"%s\",%d]", i == 0 ? "{" : ",", s_names[i],
            s_states[health.checks[i].state], (int)health.checks[i].error);
        if (count < 0 || (size_t)count >= capacity - used) {
            out[0] = '\0';
            return ESP_ERR_NO_MEM;
        }
        used += (size_t)count;
    }
    if (capacity - used < 2) { out[0] = '\0'; return ESP_ERR_NO_MEM; }
    out[used++] = '}';
    out[used] = '\0';
    return ESP_OK;
}

void boot_health_report(void)
{
    boot_health_snapshot_t health;
    boot_health_snapshot(&health);
    for (int i = 0; i < BOOT_CHECK_COUNT; ++i)
        ESP_LOGI(TAG, "summary %s=%s (%s)", s_names[i],
                 s_states[health.checks[i].state],
                 esp_err_to_name(health.checks[i].error));
}
