#include "calculation.h"

#include <string.h>

#include "data_records.h"

static double decimal_factor(int exponent)
{
    double factor = 1.0;
    while (exponent > 0) { factor *= 10.0; exponent--; }
    while (exponent < 0) { factor /= 10.0; exponent++; }
    return factor;
}

esp_err_t calculation_reflectance(const raw_spectrum_record_t *ground,
                                  const raw_spectrum_record_t *sky,
                                  uint32_t calculation_count,
                                  reflectance_record_t *out)
{
    if (ground == NULL || sky == NULL || out == NULL ||
        ground->spectrometer_role != SPECTROMETER_GROUND ||
        sky->spectrometer_role != SPECTROMETER_SKY ||
        ground->sample_count == 0 ||
        ground->sample_count > H1_MAX_SPECTRUM_SAMPLES ||
        ground->sample_count != sky->sample_count ||
        ground->exposure_us == 0 || sky->exposure_us == 0 ||
        /* Real H1 auto-exposure output reaches scale 10. Keep conversion
         * bounded, while allowing ample headroom around observed values. */
        ground->spectrum_scale < -18 || ground->spectrum_scale > 18 ||
        sky->spectrum_scale < -18 || sky->spectrum_scale > 18) {
        return ESP_ERR_INVALID_ARG;
    }
    uint64_t ground_us = ground->header.timestamp.b_monotonic_us;
    uint64_t sky_us = sky->header.timestamp.b_monotonic_us;
    /* "Latest sky" means the newest observation at or before ground time. */
    if (sky_us > ground_us) return ESP_ERR_INVALID_STATE;
    uint64_t age = ground_us - sky_us;
    if (age > CALCULATION_MAX_SKY_AGE_US) return ESP_ERR_TIMEOUT;

    memset(out, 0, sizeof(*out));
    data_record_header_init(&out->header, DATA_RECORD_REFLECTANCE,
                            REFLECTANCE_RECORD_WIRE_SIZE(ground->sample_count),
                            calculation_count, ground->header.session_id,
                            ground->header.segment_id, &ground->header.timestamp);
    out->calculation_count = calculation_count;
    out->ground_frame_count = ground->frame_count;
    out->sky_frame_count = sky->frame_count;
    out->sky_b_monotonic_us = sky_us;
    out->sky_age_us = (uint32_t)age;
    out->sample_count = ground->sample_count;
    out->input_quality_flags = REFLECTANCE_INPUT_GROUND_VALID |
        REFLECTANCE_INPUT_SKY_VALID | REFLECTANCE_INPUT_SKY_FRESH |
        REFLECTANCE_INPUT_SAMPLE_COUNTS_MATCH;

    /* H1 physical value is raw / 10^scale. Normalize both inputs by their
     * exposure time before division. This remains "apparent reflectance"
     * until radiometric/geometry calibration is introduced. */
    double ratio_factor =
        ((double)sky->exposure_us / (double)ground->exposure_us) *
        decimal_factor((int)sky->spectrum_scale - (int)ground->spectrum_scale);
    for (uint16_t i = 0; i < out->sample_count; i++) {
        uint8_t flags = 0;
        if (ground->exposure_status == H1_EXPOSURE_STATUS_OVER)
            flags |= REFLECTANCE_SAMPLE_GROUND_OVER;
        if (sky->exposure_status == H1_EXPOSURE_STATUS_OVER)
            flags |= REFLECTANCE_SAMPLE_SKY_OVER;
        double denominator = (double)sky->samples[i];
        /* Initial conservative raw-count floor; replace with calibrated
         * normalized dark/noise limits for each wavelength. */
        if (sky->samples[i] < CALCULATION_MIN_SKY_RAW_COUNTS) {
            flags |= REFLECTANCE_SAMPLE_SKY_TOO_SMALL;
            out->invalid_denominator_count++;
            out->sample_flags[i] = flags;
            continue;
        }
        double percent = 100.0 * (double)ground->samples[i] /
                         denominator * ratio_factor;
        if (percent < 0.0) {
            percent = 0.0;
            flags |= REFLECTANCE_SAMPLE_CLAMPED_LOW;
            out->clamped_low_count++;
        } else if (percent > 100.0) {
            percent = 100.0;
            flags |= REFLECTANCE_SAMPLE_CLAMPED_HIGH;
            out->clamped_high_count++;
        }
        out->reflectance_0p01_percent[i] = (uint16_t)(percent * 100.0 + 0.5);
        flags |= REFLECTANCE_SAMPLE_VALID;
        out->valid_sample_count++;
        out->sample_flags[i] = flags;
    }
    return ESP_OK;
}

esp_err_t calculation_self_test(void)
{
    /* These production-sized objects cannot live on a small RTOS caller stack. */
    static raw_spectrum_record_t ground, sky;
    static reflectance_record_t result;
    memset(&ground, 0, sizeof(ground));
    memset(&sky, 0, sizeof(sky));
    ground.spectrometer_role = SPECTROMETER_GROUND;
    sky.spectrometer_role = SPECTROMETER_SKY;
    ground.sample_count = sky.sample_count = 3;
    ground.exposure_us = sky.exposure_us = 1000;
    ground.header.timestamp.b_monotonic_us = 1100;
    sky.header.timestamp.b_monotonic_us = 1000;
    ground.samples[0] = 5; ground.samples[1] = 20; ground.samples[2] = 1;
    sky.samples[0] = 10; sky.samples[1] = 10; sky.samples[2] = 0;
    if (calculation_reflectance(&ground, &sky, 1, &result) != ESP_OK ||
        result.reflectance_0p01_percent[0] != 5000 ||
        result.reflectance_0p01_percent[1] != 10000 ||
        !(result.sample_flags[1] & REFLECTANCE_SAMPLE_CLAMPED_HIGH) ||
        !(result.sample_flags[2] & REFLECTANCE_SAMPLE_SKY_TOO_SMALL) ||
        result.valid_sample_count != 2 || result.invalid_denominator_count != 1) {
        return ESP_FAIL;
    }
    sky.spectrum_scale = 19;
    if (calculation_reflectance(&ground, &sky, 2, &result) !=
        ESP_ERR_INVALID_ARG) return ESP_FAIL;
    return ESP_OK;
}
