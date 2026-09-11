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
    /** Minimum idle time after every DTU-bound fragment. */
    uint32_t fragment_gap_ms;
    /** Maximum live GPS publication frequency; newer pending data replaces old. */
    uint32_t gps_min_interval_ms;
    /** Stable nonzero identity included in every transport message. */
    uint64_t source_id;
    /** Cloud application acknowledgement deadline and retry count. */
    uint32_t ack_timeout_ms;
    uint8_t max_retries;
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
    uint32_t gps_superseded;
    uint32_t reflectance_submitted;
    uint32_t reflectance_sent;
    uint32_t reflectance_queue_overflows;
    uint32_t fragments_sent;
    uint32_t bytes_sent;
    uint32_t messages_retried;
    uint32_t acknowledgements_received;
    uint32_t acknowledgement_timeouts;
    /** Backward-compatible aggregate of mismatched and explicit negative
     * acknowledgements. Prefer the two diagnostic counters below. */
    uint32_t acknowledgement_rejected;
    /** Valid DTA1 frames that belong to another/stale logical message. */
    uint32_t acknowledgements_mismatched;
    /** Matching DTA1 frames whose application status is nonzero. */
    uint32_t acknowledgements_negative;
    /** Logical messages that exhausted all delivery attempts. */
    uint32_t messages_failed;
    uint32_t serialization_errors;
    uint32_t uart_errors;
    uint32_t drain_timeouts;
    uint32_t downlink_bytes_received;
} telemetry_status_t;

/** Start the sole DTU UART owner and its paced transmit task. The DTU must
 * already contain its persistent MQTT provisioning.
 */
esp_err_t telemetry_start(const telemetry_config_t *config);

/** Bind subsequent records to one nonzero, globally unpredictable flight ID
 * and clear stale pending records. The current firmware has one flight per boot.
 */
esp_err_t telemetry_begin_mission(uint64_t mission_id);

/** GPS is a nonblocking latest-value submission at the configured rate.
 * Reflectance uses a bounded FIFO: it never silently overwrites an older
 * result, and a full queue returns ESP_ERR_NO_MEM and counts a dropped live
 * telemetry record without stopping later submissions or SD recording.
 */
esp_err_t telemetry_submit_gps(const gps_record_t *record);
esp_err_t telemetry_submit_reflectance(
    const reflectance_record_t *record);

/** Stop accepting records and wait until pending GPS/reflectance records have
 * received matching cloud application acknowledgements.
 */
esp_err_t telemetry_finish_mission(uint32_t timeout_ms);

/** Lock-bounded status snapshot suitable for heartbeat health evaluation. */
void telemetry_get_status(telemetry_status_t *out);

#ifdef __cplusplus
}
#endif
