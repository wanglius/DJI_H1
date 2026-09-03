#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool capturing;
    uint32_t frames[2];
    uint32_t errors[2];
} acquisition_status_t;

/** Single lifecycle owner only; call before accepting the first mission. */
esp_err_t acquisition_prepare_dual(void);
/** Reset cancellation/status only when the previous run has fully returned. */
void acquisition_arm(void);
/** Nonblocking, safe during preparation, streaming, or repeated stop requests. */
void acquisition_request_stop(void);
void acquisition_get_status(acquisition_status_t *out);
/** Single owner: duration 0 runs until request_stop; nonzero retains bench mode. */
esp_err_t acquisition_run_dual(uint32_t duration_ms);

#ifdef __cplusplus
}
#endif
