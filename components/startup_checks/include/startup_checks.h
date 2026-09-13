#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Run qualification-only deterministic checks when enabled in Kconfig.
 * Production builds return ESP_OK without linking the test implementations. */
esp_err_t startup_checks_run(void);

#ifdef __cplusplus
}
#endif
