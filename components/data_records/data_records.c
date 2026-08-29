#include "data_records.h"

void data_record_header_init(data_record_header_t *header,
                             data_record_type_t type,
                             uint32_t record_size,
                             uint32_t sequence,
                             uint32_t session_id,
                             int64_t b_timestamp_us)
{
    if (header == NULL) return;
    header->format_version = DATA_RECORD_FORMAT_VERSION;
    header->record_type = (uint16_t)type;
    header->record_size = record_size;
    header->sequence = sequence;
    header->session_id = session_id;
    header->b_timestamp_us = b_timestamp_us;
}
