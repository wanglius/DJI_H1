#pragma once

/* Deterministic decoder and record-layout self-test. */

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Exercise the shared record contract and simulated/decoded GPS source. */
esp_err_t data_pipeline_self_test(void);

#ifdef __cplusplus
}
#endif
