#pragma once

/* Destructive SD smoke test: overwrites H1TEST.TXT. */

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Mount, write, read back, and unmount the production-board TF card. */
esp_err_t sd_card_connection_test(void);

#ifdef __cplusplus
}
#endif
