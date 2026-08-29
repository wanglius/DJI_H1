#pragma once

/* Hardware integration test; not part of the production component API. */

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Run dual H1 acquisition while continuously writing to the TF card. */
esp_err_t coexistence_test_run(uint32_t duration_ms);

#ifdef __cplusplus
}
#endif
