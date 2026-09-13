#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/uart.h"
#include "esp_err.h"
#include "measurement_records.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uart_port_t uart_port;
    int tx_gpio;
    int rx_gpio;
    uint32_t baud_rate;
    /** Idle time after every DTU-bound fragment. Zero deliberately emits
     * consecutive fragments without an application-level delay and relies on
     * the DTU UART packetizer for flow control. Values 1..5 ms are rejected
     * because they are shorter than the previously qualified safe interval. */
    uint32_t fragment_gap_ms;
    /** Stable nonzero identity included in every transport message. */
    uint64_t source_id;
    /** Cloud application acknowledgement deadline and retry count. */
    uint32_t ack_timeout_ms;
    uint8_t max_retries;
    /** Number of retained unacknowledged records allocated in PSRAM. */
    uint16_t pool_length;
    /** Emit one compact microsecond timing record per delivery attempt. */
    bool timing_diagnostics;
} telemetry_config_t;

typedef struct {
    bool initialized;
    /** False only after a local infrastructure/software fault. Delivery
     * pressure and exhausted cloud retries are tracked separately below. */
    bool healthy;
    uint64_t source_id;
    uint64_t mission_id;
    uint32_t gps_submitted;
    uint32_t gps_sent;
    /** Retained for mission-summary compatibility; FIFO GPS never supersedes. */
    uint32_t gps_superseded;
    /** GPS records rejected because the shared retained pool was full. */
    uint32_t gps_queue_overflows;
    uint32_t reflectance_submitted;
    /** GPS/reflectance "sent" counters mean positively acknowledged complete
     * application messages, preserving the pre-windowing status semantics. */
    uint32_t reflectance_sent;
    uint32_t reflectance_queue_overflows;
    uint32_t pool_capacity;
    uint32_t pool_used;
    uint32_t pool_high_watermark;
    uint32_t messages_in_flight;
    uint32_t messages_in_flight_high_watermark;
    uint32_t transmission_attempts;
    uint32_t fragments_sent;
    uint32_t bytes_sent;
    uint32_t messages_retried;
    /** Slow, globally rate-limited sends of messages whose normal retry
     * budget was exhausted but which remain retained for eventual delivery. */
    uint32_t recovery_probes;
    uint32_t acknowledgements_received;
    uint32_t acknowledgement_timeouts;
    /** Backward-compatible aggregate of mismatched and explicit negative
     * acknowledgements. Prefer the two diagnostic counters below. */
    uint32_t acknowledgement_rejected;
    /** Valid DTA1 frames that belong to another/stale logical message. */
    uint32_t acknowledgements_mismatched;
    /** Matching DTA1 frames whose application status is nonzero. */
    uint32_t acknowledgements_negative;
    /** Unique logical messages that exhausted normal delivery attempts or
     * were definitively rejected. Recovery probes do not recount them. */
    uint32_t messages_failed;
    uint32_t serialization_errors;
    /** Internal ownership/free-list failures in the shared PSRAM pool. */
    uint32_t reflectance_pool_errors;
    uint32_t uart_errors;
    uint32_t drain_timeouts;
    uint32_t downlink_bytes_received;
    uint32_t acknowledgement_rtt_last_us;
    uint32_t acknowledgement_rtt_max_us;
    uint64_t acknowledgement_rtt_sum_us;
    /** Records deliberately discarded by prepare-power-off. */
    uint32_t messages_abandoned_shutdown;
    /** True when prepare-power-off deliberately cancelled cloud delivery so
     * the shutdown budget could be reserved for durable SD finalization. */
    bool shutdown_aborted;
} telemetry_status_t;

/** Start the sole DTU UART owner and its paced transmit task. The DTU must
 * already contain its persistent MQTT provisioning. The fixed shared payload
 * pool is allocated from initialized PSRAM during this call.
 */
esp_err_t telemetry_start(const telemetry_config_t *config);

/** Bind subsequent records to one nonzero, globally unpredictable flight ID
 * and clear stale pending records. The current firmware has one flight per boot.
 */
esp_err_t telemetry_begin_mission(uint64_t mission_id);

/** GPS and reflectance are nonblocking bounded FIFO submissions backed by one
 * retained PSRAM pool. Neither silently overwrites an older record. A full
 * pool returns ESP_ERR_NO_MEM and counts a dropped live telemetry record
 * without stopping later submissions or SD recording.
 */
esp_err_t telemetry_submit_gps(const gps_record_t *record);
esp_err_t telemetry_submit_reflectance(
    const reflectance_record_t *record);

/** Stop accepting records and wait until every retained GPS/reflectance record
 * has received a matching positive cloud application acknowledgement.
 */
esp_err_t telemetry_finish_mission(uint32_t timeout_ms);

/** Immediately close admission and cancel queued/in-progress delivery.
 * This idempotent prepare-power-off hook is nonblocking and intentionally
 * sacrifices telemetry so SD recording can be finalized before power loss.
 * Bytes already accepted by the UART hardware may finish shifting, but no
 * later fragment, retry, or queued message is intentionally transmitted.
 * The shutdown latch is terminal: begin/submit calls remain rejected until
 * the expected physical power cycle resets the component.
 */
esp_err_t telemetry_abort_mission(void);

/** Lock-bounded status snapshot suitable for heartbeat health evaluation. */
void telemetry_get_status(telemetry_status_t *out);

#ifdef __cplusplus
}
#endif
