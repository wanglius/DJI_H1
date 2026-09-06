#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "h1.h"
#include "measurement_records.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool active;
    bool healthy;
    uint32_t raw_written;
    uint32_t reflectance_written;
    uint32_t raw_dropped;
    uint32_t calculation_rejected;
    uint32_t write_errors;
} measurement_recorder_status_t;

/** Create the fixed pools, pointer queue, and sole SD writer task. */
esp_err_t measurement_recorder_init(void);
/** Open/reuse mission files and start a measurement segment. */
esp_err_t measurement_recorder_begin(uint32_t session_id, uint16_t segment_id);
/** Drain all submitted frames and flush both files. Files stay open for restart. */
esp_err_t measurement_recorder_end(void);
/** Drain, close mission files, and stop accepting records before SD unmount. */
esp_err_t measurement_recorder_shutdown(void);

/** Copy a completed H1 frame into a fixed pool buffer and enqueue its pointer.
 * Never blocks acquisition. Failure means raw data would be lost and should
 * stop the active measurement in a controlled manner. */
esp_err_t measurement_recorder_submit(spectrometer_role_t role,
                                      uint32_t frame_count,
                                      const h1_spectrum_frame_t *frame,
                                      int64_t b_timestamp_us);

void measurement_recorder_get_status(measurement_recorder_status_t *out);

#ifdef __cplusplus
}
#endif
