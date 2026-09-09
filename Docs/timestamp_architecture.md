# Timekeeping and timestamp architecture

## Purpose

Every GPS, raw-spectrum, calculated-reflectance, operation-log and MQTT record
must be traceable to a common mission timeline. The B board therefore preserves
its own monotonic event time and correlates it with the monotonic and UTC time
reported by the A board. Synchronization does **not** set or step the ESP32
system clock until the A/B model is both UTC-valid and `LOCKED`. At that point,
the wall clock is set once for the synchronization generation so FatFs can
produce useful file modification times. Record ordering never depends on it.

This design keeps record ordering reliable even before UTC becomes available,
during a temporary A-to-B link interruption, or if the A-board clock restarts.

## Clock domains

`clock_sync` correlates three clock domains:

- B monotonic microseconds from `esp_timer_get_time()` are always authoritative
  for local ordering.
- A `uint32_t mono_ms` is extended across forward wrap to a 64-bit timeline.
- Valid A `utc_sec` and `utc_msec` are combined as
  `uint64_t(utc_sec) * 1000 + utc_msec` and correlated through A monotonic time.

The A-board values are accepted only from a complete realtime-data frame that
has passed framing, length and CRC validation. The protocol's UTC-valid status
bit determines whether a sample may update the UTC relationship.

## Synchronization flow

The A-board UART task timestamps a complete CRC-verified realtime frame and
submits a small observation without blocking. This prevents timekeeping from
delaying UART parsing, heartbeat transmission or acquisition. A priority-6
clock task consumes the queue and:

1. Extends the A-board 32-bit monotonic counter across normal rollover.
2. Maintains a 32-sample linear fit between A milliseconds and B microseconds.
3. Uses the median observed UTC offset to reject individual timing outliers.
4. Rejects fit residuals over 50 ms and UTC-offset jumps over one second.
5. Publishes a compact snapshot that record producers can read safely.
6. Sets the POSIX wall clock once when a UTC-valid generation reaches `LOCKED`.

A backward A timer jump, as opposed to a normal unsigned rollover, resets the
model and begins a new synchronization generation.

## State model

| State | Meaning |
|---|---|
| `UNSYNCED` | No usable relationship has been established. |
| `ACQUIRING` | Valid observations are accumulating; at least five are required. |
| `LOCKED` | A/B monotonic correlation is valid and fresh. |
| `HOLDOVER` | No observation has arrived for 1.5 seconds; the last model is retained temporarily. |
| `INVALID` | No observation has arrived for five seconds, so correlated time must not be used. |

After communication resumes, new valid observations move the model back through
acquisition to `LOCKED`. A generation change tells downstream consumers that a
clock discontinuity occurred; records on opposite sides of that boundary must
not be assumed to share one continuous fitted timeline.

## Universal record timestamp

`clock_sync_timestamp()` converts the exact B timestamp captured by a record
producer. It always returns B time; correlated A/UTC values are marked valid
only in LOCKED or HOLDOVER. `sync_age_ms`, state, generation and validity bits
make uncertainty explicit. The 32-byte in-memory `record_time_t` is:

| Field | Bytes | Description |
|---|---:|---|
| `b_monotonic_us` | 8 | Authoritative local event time. |
| `a_monotonic_ms` | 8 | Extended and correlated A-board time. |
| `utc_ms` | 8 | Unix UTC milliseconds derived from A time. |
| `sync_age_ms` | 4 | Age of the newest observation used by the model. |
| `sync_generation` | 2 | Increments when a discontinuity resets synchronization. |
| `sync_state` | 1 | State at the event timestamp. |
| `valid_flags` | 1 | Explicit validity of the B, A and UTC fields. |

`clock_sync_timestamp()` must be called with the B monotonic timestamp captured
at the actual event, rather than the later time at which a logger serializes the
record. B time is always returned as valid. Correlated A and UTC values are
valid only in `LOCKED` or `HOLDOVER`, and only when the underlying UTC relation
is available.

Consumers must inspect `valid_flags`; a numeric zero in an invalid field is not
a legitimate timestamp.

## Storage and transmission

Persistent files and MQTT must explicitly serialize these fields in little
endian order; never write/send the C object representation. The current UART
timestamp denotes frame completion. At 115200 8N1, a 37-byte realtime frame
takes about 3.2 ms on the wire; fixed A processing/transport delay is not known,
so this milestone does not claim sub-millisecond absolute alignment.

The original A-board timestamp fields and B receive timestamp should also remain
in the GPS/navigation record. They provide an audit trail and allow the clock
relationship to be reconstructed or improved during post-processing.

FatFs obtains modification timestamps from the POSIX wall clock. The firmware
uses the `UTC0` process timezone because FAT stores calendar fields without any
timezone marker and has only two-second resolution. At orderly mission shutdown,
after every handle and the final `MISSION.JSON` checkpoint have closed, the SD
component applies the final synchronized UTC time to all five mission files and
their directory. This best-effort metadata update cannot turn a safely flushed
dataset into a failed shutdown. Original FAT creation times can still show the
boot fallback because the directory is intentionally allocated before A-board
time is available; consumers should sort by modification time.

## Failure behavior

- A missing or invalid UTC flag does not prevent B-monotonic record ordering.
- A full observation queue drops the new synchronization observation rather
  than blocking the UART task; diagnostics report this condition.
- A short link outage enters holdover, allowing explicitly marked extrapolation.
- A long outage invalidates correlated A and UTC values.
- A timer reset, implausible fit or large UTC discontinuity starts a new model.
- No synchronization failure may stop the independent 1 Hz heartbeat task.

## Current scope

Clock synchronization timestamps GPS, spectrum, reflectance and operation-log
records and supplies the filesystem wall clock. Original A timestamps and the B
receive time remain in the GPS record for offline reconstruction. MQTT remains
a future consumer of the same universal record timestamp.
