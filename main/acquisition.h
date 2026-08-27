#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Prepare, run, stop, and summarize simultaneous acquisition from both H1s. */
esp_err_t acquisition_run_dual(uint32_t duration_ms);

#ifdef __cplusplus
}
#endif
