#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/uart.h"
#include "esp_err.h"
#include "measurement_records.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    /** Retain each record until a matching positive DTA1 is received. */
    TELEMETRY_DELIVERY_APPLICATION_ACK = 0,
    /** Release each record after one complete successful UART transmission. */
    TELEMETRY_DELIVERY_FIRE_AND_FORGET = 1,
} telemetry_delivery_mode_t;

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
    /** Zero-initialization deliberately selects the production-compatible
     * application-ACK behavior. Fire-and-forget is currently diagnostic. */
    telemetry_delivery_mode_t delivery_mode;
    /** Number of retained unacknowledged records allocated in PSRAM. */
    uint16_t pool_length;
    /** Maximum time a record may occupy the retained pool, measured from
     * local admission time. Expired records are dropped so delayed cloud
     * delivery cannot turn the pool into an ever-older backlog. Zero disables
     * expiry and is intended only for dedicated transport diagnostics. */
    uint32_t max_residency_ms;
    /** Period of the latest-value reflectance sampler. Production uses
     * 500 ms (2 Hz). Zero bypasses sampling for dedicated transport stress
     * firmware; it should not be used by the production application. */
    uint32_t reflectance_interval_ms;
    /** Maximum GPS records per telemetry-only DGB1 batch. */
    uint16_t gps_batch_max_records;
    /** Seal a partial GPS batch this long after its first record was admitted.
     * This bounds live-position latency even if the 5 Hz source pauses. */
    uint32_t gps_batch_max_delay_ms;
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
    uint16_t gps_batch_max_records;
    uint32_t gps_batch_max_delay_ms;
    uint32_t gps_submitted;
    /** DGB1 messages admitted to the reliable transmit queue. */
    uint32_t gps_batches_submitted;
    /** Submitted DGB1 messages containing fewer than gps_batch_max_records. */
    uint32_t gps_partial_batches;
    uint32_t gps_sent;
    /** Retained for mission-summary compatibility; FIFO GPS never supersedes. */
    uint32_t gps_superseded;
    /** GPS records rejected because the shared retained pool was full. */
    uint32_t gps_queue_overflows;
    /** GPS records deliberately discarded after exceeding max_residency_ms. */
    uint32_t gps_expired;
    uint32_t gps_batches_sent;
    uint32_t gps_batches_expired;
    /** GPS source records contained in pool entries deliberately abandoned by
     * the terminal shutdown forecast. */
    uint32_t gps_records_abandoned_shutdown;
    /** Valid calculated records offered by the measurement pipeline. */
    uint32_t reflectance_offered;
    /** Records admitted by the configured latest-value sampler. */
    uint32_t reflectance_submitted;
    /** Older candidates intentionally replaced before their sampling tick.
     * This is expected downsampling, not delivery degradation. */
    uint32_t reflectance_rate_limited;
    /** Complete messages cleared according to the configured delivery mode:
     * positive DTA1 in application-ACK mode, or successful complete UART
     * transmission in fire-and-forget mode. */
    uint32_t reflectance_sent;
    uint32_t reflectance_queue_overflows;
    /** Reflectance records deliberately discarded after exceeding the
     * configured maximum pool residency. */
    uint32_t reflectance_expired;
    uint32_t max_residency_ms;
    uint32_t pool_capacity;
    uint32_t pool_used;
    uint32_t pool_high_watermark;
    uint32_t messages_in_flight;
    uint32_t messages_in_flight_high_watermark;
    uint32_t transmission_attempts;
    uint32_t fragments_sent;
    uint32_t bytes_sent;
    uint32_t messages_retried;
    /** Globally rate-limited sends of messages whose normal retry budget was
     * exhausted. The rate rises with pool pressure from 1 to at most 10/s. */
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

/** GPS and reflectance are nonblocking submissions backed by one retained
 * PSRAM pool. GPS records remain ordered but are losslessly sealed into DGB1
 * batches before entering their FIFO. Reflectance uses a configured
 * latest-value sampling period before entering the reliable FIFO; intentional
 * replacement is counted separately and is not an error. A full pool returns
 * ESP_ERR_NO_MEM and counts a dropped live telemetry record without stopping
 * later submissions or SD recording. Every admitted record also has a bounded
 * residency; expiration is counted as delivery degradation but never latches
 * the component unhealthy.
 */
esp_err_t telemetry_submit_gps(const gps_record_t *record);
esp_err_t telemetry_submit_reflectance(
    const reflectance_record_t *record);

/** Stop accepting records and wait until every retained GPS/reflectance record
 * has completed according to the configured delivery mode.
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
