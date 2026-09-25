#pragma once
#include "telemetry.h"

/* Sole telemetry UART owner only, before any binary transmission. No writes
 * to persistent modem configuration. False forbids binary TX for this boot. */
bool dtu_boot_check(const telemetry_config_t *config, bool (*cancelled)(void));
