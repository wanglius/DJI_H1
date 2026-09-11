#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Little-endian bytes read "DTF2". The transport format is independent of
 * the measurement-record format carried inside its payload. */
#define TELEMETRY_FRAGMENT_MAGIC UINT32_C(0x32465444)
#define TELEMETRY_FRAGMENT_VERSION 0x02U
#define TELEMETRY_FRAGMENT_HEADER_SIZE 44U
#define TELEMETRY_FRAGMENT_TRAILER_SIZE 4U
#define TELEMETRY_FRAGMENT_WIRE_MAX_SIZE 1024U
#define TELEMETRY_FRAGMENT_PAYLOAD_MAX \
    (TELEMETRY_FRAGMENT_WIRE_MAX_SIZE - TELEMETRY_FRAGMENT_HEADER_SIZE - \
     TELEMETRY_FRAGMENT_TRAILER_SIZE)
#define TELEMETRY_MESSAGE_MAX_SIZE \
    ((uint32_t)TELEMETRY_FRAGMENT_PAYLOAD_MAX * UINT16_MAX)

/* Values intentionally match data_record_type_t where one exists. */
typedef enum {
    TELEMETRY_MESSAGE_GPS = 1,
    TELEMETRY_MESSAGE_RAW_SPECTRUM = 2,
    TELEMETRY_MESSAGE_REFLECTANCE = 3,
    TELEMETRY_MESSAGE_OPERATION_LOG = 4,
} telemetry_message_type_t;

/** Immutable description prepared once for a complete logical message. */
typedef struct {
    const uint8_t *payload;
    uint32_t payload_length;
    uint32_t payload_crc32;
    uint64_t source_id;
    uint64_t mission_id;
    uint32_t message_sequence;
    uint16_t fragment_count;
    uint8_t message_type;
    uint16_t flags;
} telemetry_fragment_plan_t;

/** Validated view into one encoded fragment. The payload pointer remains valid
 * only while the encoded input buffer remains valid. */
typedef struct {
    const uint8_t *payload;
    uint32_t message_length;
    uint32_t message_crc32;
    uint64_t source_id;
    uint64_t mission_id;
    uint32_t message_sequence;
    uint16_t fragment_index;
    uint16_t fragment_count;
    uint16_t payload_length;
    uint16_t flags;
    uint8_t message_type;
} telemetry_fragment_view_t;

/** Called synchronously for each encoded fragment. The callback must consume
 * or copy the bytes before returning; the scratch buffer is immediately reused. */
typedef esp_err_t (*telemetry_fragment_emit_fn)(
    const uint8_t *fragment, size_t fragment_length,
    uint16_t fragment_index, uint16_t fragment_count, void *context);

/** Prepare fragmentation metadata and the full-message CRC without allocating. */
esp_err_t telemetry_fragment_plan_init(
    telemetry_fragment_plan_t *plan, uint8_t message_type,
    uint64_t source_id, uint64_t mission_id, uint32_t message_sequence,
    uint16_t flags,
    const void *payload, size_t payload_length);

/** Encode one zero-based fragment into caller-owned storage. */
esp_err_t telemetry_fragment_encode(
    const telemetry_fragment_plan_t *plan, uint16_t fragment_index,
    uint8_t *output, size_t output_capacity, size_t *output_length);

/** Encode and synchronously emit every fragment in ascending index order. */
esp_err_t telemetry_fragment_emit_all(
    const telemetry_fragment_plan_t *plan,
    uint8_t *scratch, size_t scratch_capacity,
    telemetry_fragment_emit_fn emit, void *context);

/** Validate one fragment, including its trailing CRC, without copying payload. */
esp_err_t telemetry_fragment_decode(
    const uint8_t *fragment, size_t fragment_length,
    telemetry_fragment_view_t *view);

/** IEEE CRC-32 used by fragment and complete-message integrity checks. */
uint32_t telemetry_crc32(const void *data, size_t length);

/* A cloud consumer returns one DTA1 acknowledgement only after the complete
 * DTF2 message and its inner record have passed validation. */
#define TELEMETRY_ACK_MAGIC UINT32_C(0x31415444)
#define TELEMETRY_ACK_VERSION 0x01U
#define TELEMETRY_ACK_WIRE_SIZE 40U

typedef struct {
    uint64_t source_id;
    uint64_t mission_id;
    uint32_t message_sequence;
    uint32_t message_crc32;
    uint8_t message_type;
    uint16_t status;
} telemetry_ack_t;

esp_err_t telemetry_ack_encode(const telemetry_ack_t *ack,
                               uint8_t output[TELEMETRY_ACK_WIRE_SIZE]);
esp_err_t telemetry_ack_decode(const uint8_t *input, size_t input_length,
                               telemetry_ack_t *ack);

#ifdef __cplusplus
}
#endif
