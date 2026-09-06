#pragma once

#include "esp_err.h"
#include "measurement_records.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CALCULATION_MAX_SKY_AGE_US 500000U
#define CALCULATION_MIN_SKY_RAW_COUNTS 4U

/** Calculate exposure- and decimal-scale-normalized apparent reflectance.
 * The result timestamp is copied from ground. Inputs must have equal counts. */
esp_err_t calculation_reflectance(const raw_spectrum_record_t *ground,
                                  const raw_spectrum_record_t *sky,
                                  uint32_t calculation_count,
                                  reflectance_record_t *out);

esp_err_t calculation_self_test(void);

#ifdef __cplusplus
}
#endif
