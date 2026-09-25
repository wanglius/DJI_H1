#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* Always-on production checks, distinct from optional qualification self-tests.
 * PASS is evidence at one point in time, not a promise of future availability. */
typedef enum {
    BOOT_FLASH, BOOT_PSRAM, BOOT_SD, BOOT_RECORDER,
    BOOT_SC16, BOOT_SC16_A, BOOT_SC16_B, BOOT_H1_A, BOOT_H1_B,
    BOOT_TELEMETRY_UART, BOOT_DTU_PROFILE, BOOT_DTU_NETWORK, BOOT_GROUND_ACK,
    BOOT_CHECK_COUNT
} boot_check_t;
typedef enum {
    BOOT_PENDING, BOOT_PASS, BOOT_FAIL, BOOT_BLOCKED, BOOT_WAITING
} boot_check_state_t;
typedef struct {
    boot_check_state_t state;
    esp_err_t error;
} boot_check_result_t;
typedef struct {
    boot_check_result_t checks[BOOT_CHECK_COUNT];
} boot_health_snapshot_t;

void boot_health_set(boot_check_t check, boot_check_state_t state, esp_err_t error);
void boot_health_result(boot_check_t check, esp_err_t error);
void boot_health_snapshot(boot_health_snapshot_t *out);
bool boot_health_critical_failed(void);
bool boot_health_telemetry_failed(void);
/* Bounded JSON object of {check:[state,error]}; symbolic state names. */
esp_err_t boot_health_json(char *out, size_t capacity);
void boot_health_report(void);
