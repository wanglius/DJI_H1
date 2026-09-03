#pragma once

#include "ab_protocol.h"
#include "esp_err.h"

/** Starts the sole hardware lifecycle owner. No acquisition occurs at boot. */
esp_err_t mission_control_init(void);
/** Nonblocking admission; returns the protocol ACK result (0..4).
 * Success means accepted, not completed. Heartbeat is authoritative.
 * A stopped session cannot be resumed: use a NEW session ID to restart.
 */
uint8_t mission_control_start(uint32_t session);
uint8_t mission_control_stop(uint32_t session);
uint8_t mission_control_power_off(void);
bool mission_control_ready(void);
/** Snapshot only: never performs sensor or SD I/O on the UART task. */
void mission_control_get_status(ab_status_report_t *out);
