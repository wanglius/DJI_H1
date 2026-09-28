#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool capturing;
    /** True when teardown was requested but runtime objects remain retained. */
    bool cleanup_pending;
    uint32_t frames[2];
    uint32_t errors[2];
    /** Current segment only, frozen before teardown; A=ground, B=sky.
     * Overruns count observed hardware OE indications, not lost bytes.
     * Software drops count bytes rejected by the RX stream buffer. */
    uint32_t rx_overruns[2];
    uint32_t rx_software_drops[2];
} acquisition_status_t;

/** Single lifecycle owner only; call before accepting the first mission. */
esp_err_t acquisition_prepare_dual(void);
/** Reset cancellation/status only when the previous runtime is fully released. */
esp_err_t acquisition_arm(void);
/** Nonblocking, safe during preparation, streaming, or repeated stop requests. */
void acquisition_request_stop(void);
/** Power-off variant with an absolute esp_timer deadline. It never extends an
 * earlier deadline and bounds the wait for reader tasks during cleanup. */
void acquisition_request_stop_before(int64_t deadline_us);
/** Retry an incomplete teardown without creating or overwriting any object. */
esp_err_t acquisition_retry_cleanup(void);
void acquisition_get_status(acquisition_status_t *out);
/** Single owner: duration 0 runs until request_stop; nonzero retains bench mode. */
esp_err_t acquisition_run_dual(uint32_t duration_ms);

#ifdef __cplusplus
}
#endif
