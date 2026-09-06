#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "ab_protocol.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CLOCK_SYNC_UNSYNCED = 0,
    CLOCK_SYNC_ACQUIRING,
    CLOCK_SYNC_LOCKED,
    CLOCK_SYNC_HOLDOVER,
    CLOCK_SYNC_INVALID,
} clock_sync_state_t;

enum {
    RECORD_TIME_VALID_B_MONOTONIC = 1U << 0,
    RECORD_TIME_VALID_A_MONOTONIC = 1U << 1,
    RECORD_TIME_VALID_UTC         = 1U << 2,
};

/** Universal in-memory timestamp. Persistent/MQTT formats serialize fields
 * explicitly; they must not transmit the compiler representation directly. */
typedef struct {
    uint64_t b_monotonic_us;
    uint64_t a_monotonic_ms;
    uint64_t utc_ms;
    uint32_t sync_age_ms;
    uint16_t sync_generation;
    uint8_t sync_state;
    uint8_t valid_flags;
} record_time_t;

_Static_assert(sizeof(record_time_t) == 32, "record_time_t must be 32 bytes");

/** Create the observation queue/task. Call once before accepting A frames. */
esp_err_t clock_sync_init(void);

/** Nonblocking producer API for the A-board UART task. b_receive_us denotes
 * completion of the CRC-verified realtime frame, not its first UART byte. */
esp_err_t clock_sync_submit(const ab_realtime_data_t *data,
                            int64_t b_receive_us);

/** Correlate an arbitrary B event time using a short immutable model snapshot. */
esp_err_t clock_sync_timestamp(int64_t b_monotonic_us, record_time_t *out);

/** Pure arithmetic checks; does not require the task or mutate live state. */
esp_err_t clock_sync_self_test(void);

const char *clock_sync_state_name(clock_sync_state_t state);

#ifdef __cplusplus
}
#endif
