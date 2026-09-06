#include "data_records.h"

#include <string.h>

void data_record_header_init(data_record_header_t *header,
                             data_record_type_t type,
                             uint32_t record_size,
                             uint32_t sequence,
                             uint32_t session_id,
                             uint16_t segment_id,
                             const record_time_t *timestamp)
{
    if (header == NULL) return;
    memset(header, 0, sizeof(*header));
    header->magic = DATA_RECORD_MAGIC;
    header->format_version = DATA_RECORD_FORMAT_VERSION;
    header->record_type = (uint16_t)type;
    header->header_size = DATA_RECORD_WIRE_HEADER_SIZE;
    header->record_size = record_size;
    header->record_sequence = sequence;
    header->session_id = session_id;
    header->segment_id = segment_id;
    if (timestamp != NULL) header->timestamp = *timestamp;
}
