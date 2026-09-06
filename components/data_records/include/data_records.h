#pragma once

#include <stdint.h>

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

#ifdef __cplusplus
}
#endif
