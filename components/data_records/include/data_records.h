#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "measurement_records.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize the common header of any record type. */
void data_record_header_init(data_record_header_t *header,
                             data_record_type_t type,
                             uint32_t record_size,
                             uint32_t sequence,
                             uint32_t session_id,
                             uint16_t segment_id,
                             const record_time_t *timestamp);

/** Serialize one complete v01 record, including its trailing IEEE CRC-32.
 * SD recording and MQTT telemetry share these functions so their wire formats
 * cannot diverge. output_length is updated only on success.
 */
esp_err_t data_record_serialize_raw(const raw_spectrum_record_t *record,
                                    void *output, size_t output_capacity,
                                    size_t *output_length);
esp_err_t data_record_serialize_reflectance(
    const reflectance_record_t *record, void *output, size_t output_capacity,
    size_t *output_length);
esp_err_t data_record_serialize_gps(const gps_record_t *record,
                                    void *output, size_t output_capacity,
                                    size_t *output_length);

#ifdef __cplusplus
}
#endif
