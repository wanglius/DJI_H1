#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "h1.h"
#include "measurement_records.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool active;
    bool healthy;
    uint32_t raw_written;
    uint32_t reflectance_written;
    uint32_t gps_written;
    uint32_t events_written;
    uint32_t raw_dropped;
    uint32_t gps_dropped;
    uint32_t events_dropped;
    uint32_t calculation_rejected;
    /** Non-empty A-board serials that disagreed with the first canonical ID. */
    uint32_t identity_mismatches;
    uint32_t write_errors;
    uint32_t flush_count;
    uint32_t flush_errors;
    uint32_t max_flush_us;
    uint32_t queue_high_watermark;
} measurement_recorder_status_t;

typedef enum {
    MEASUREMENT_EVENT_HANDSHAKE = 1,
    MEASUREMENT_EVENT_SEGMENT_START,
    MEASUREMENT_EVENT_STOP_REQUEST,
    MEASUREMENT_EVENT_SEGMENT_END,
    MEASUREMENT_EVENT_POWER_OFF_REQUEST,
    MEASUREMENT_EVENT_PROTOCOL_CRC_ERROR,
    MEASUREMENT_EVENT_PROTOCOL_TIMEOUT,
    MEASUREMENT_EVENT_CLOCK_OBSERVATION_DROP,
    MEASUREMENT_EVENT_REFLECTANCE_REJECTED,
    MEASUREMENT_EVENT_CAPTURE_RESULT,
    MEASUREMENT_EVENT_FLIGHT_CLOSED,
    MEASUREMENT_EVENT_DRONE_IDENTITY_MISMATCH,
} measurement_event_t;

/** Create the fixed pools, sole SD writer task, and a new mission directory.
 * The directory exists before mission_control advertises B ready, so the
 * handshake and all subsequent idle/active realtime data share one flight.
 */
esp_err_t measurement_recorder_init(void);
/** Start a measurement segment. session_id is an opaque A-board value. */
esp_err_t measurement_recorder_begin(uint32_t session_id, uint16_t segment_id);
/** Drain the segment and flush every file. Safe to retry after a timeout. */
esp_err_t measurement_recorder_end(void);
/** Drain and close the mission before SD unmount. Safe to retry after timeout. */
esp_err_t measurement_recorder_shutdown(void);

/** Copy a completed H1 frame into a fixed pool buffer and enqueue its pointer.
 * Never blocks acquisition. ESP_ERR_NO_MEM means this frame was deliberately
 * dropped and counted because the bounded writer queue was under pressure;
 * callers should continue acquisition. Other errors indicate recorder failure.
 */
esp_err_t measurement_recorder_submit(spectrometer_role_t role,
                                      uint32_t frame_count,
                                      const h1_spectrum_frame_t *frame,
                                      int64_t b_timestamp_us);

/** Queue one synchronized A-board realtime sample for GPS_TRACK.BIN.
 * The A/B RX task is the sole GPS producer. A future second producer must add
 * submission serialization so queue order cannot differ from sequence order.
 */
esp_err_t measurement_recorder_submit_gps(const gps_record_t *record);

/** Queue a structured operation event without blocking its producer. */
esp_err_t measurement_recorder_log_event(measurement_event_t event,
                                         uint32_t argument0,
                                         int32_t argument1);

/** Cache A metadata and latch the first non-empty serial as mission identity.
 * A later conflicting non-empty serial is counted and logged, never allowed to
 * silently replace the canonical identity in MISSION.JSON.
 */
void measurement_recorder_note_handshake(const uint8_t drone_serial[32],
                                         uint16_t a_firmware_version,
                                         uint8_t drone_link);

void measurement_recorder_get_status(measurement_recorder_status_t *out);

#ifdef __cplusplus
}
#endif
