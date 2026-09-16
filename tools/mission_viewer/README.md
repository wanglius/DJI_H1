# DJI H1 mission viewer

A two-mode Python decoder and PyQt6 desktop viewer for measurement record
format v01. Offline mode opens completed SD mission folders; live mode receives
the same canonical records through MQTT. Both modes share record decoding,
timestamp interpretation, GPS interpolation, map/spectrum presentation, and
the read-only localhost API. Long SD files are scanned and CRC-checked once,
while spectrum arrays are read from disk only when requested.

## Run from this repository

Install Python 3.10 or later and the package from the repository root:

```powershell
python -m pip install -e tools/mission_viewer
```

Then launch it with an optional initial mission folder:

```powershell
python tools/mission_viewer/run_viewer.py D:\F_6658
```

Omit the folder to select it in the app. The viewer supports both older
folders containing only `RAW_SPECTRA.BIN` and `REFLECTANCE.BIN`, and complete
folders with `MISSION.JSON`, `GPS_TRACK.BIN`, and `EVENTS.JSONL`.

## Live MQTT mode

Copy the safe template to an ignored local configuration and edit only the
local copy:

```powershell
Copy-Item tools/mission_viewer/credentials/live_mqtt.example.json `
  tools/mission_viewer/credentials/live_mqtt.local.json
```

Then either click **Connect live telemetry…** in the same viewer window or
launch directly:

```powershell
python tools/mission_viewer/run_viewer.py `
  --live-config tools/mission_viewer/credentials/live_mqtt.local.json
```

Every `*.local.json` file in this credential directory is ignored by Git. The
tracked example contains no operational broker address, password, Baidu AK, or
device identity. Optional `source_id` and `mission_id` filters accept decimal
or `0x`-prefixed values; leave them `null` to follow the newest timestamped
mission seen on the subscribed topic.

Live reception is split into three isolated stages. Paho's network callback
only copies each MQTT payload into a bounded ingress queue and returns, keeping
QoS-1 PUBACK independent of decoding and rendering. A telemetry worker performs
DTF2 reassembly, DGB1 expansion, DHR1/CRC validation, deduplication, and live
store updates. A separate egress worker publishes DTA1 acknowledgements. The Qt
thread reads revisioned immutable snapshots once per second and does no MQTT or
record-processing work. A DTA1 is queued only after the complete logical
message has been validated and accepted by the live store. Invalid complete
messages receive a permanent-rejection DTA1; transient receiver failures
withhold acknowledgement.

`ingress_queue_size` and `ack_queue_size` default to 512. If either bounded
queue fills, the receiver increments a visible counter and withholds DTA1 for
that logical message, allowing the B-board's retry/lifetime policy to remain
the end-to-end authority. Queue depth, high-water marks, drops, ACK overflows,
and maximum processing time are displayed in the live information panel.

The route and measurement overlays refresh once per second without resetting
manual zoom or pan. **Follow latest** selects the newest spatially located
reflectance record; clicking an older point disables following until the box
is checked again. Because production bundles ten 5 Hz GPS records per DGB1,
new route points normally land in groups about every two seconds. A spectrum
remains unlocated until GPS fixes bracket its timestamp; live mode uses the
same non-extrapolating interpolation as offline mode.

This live milestone is an in-memory operational display. The onboard SD files
remain the authoritative mission record. MQTT carries GPS, reflectance, and
compact major operation events. The viewer decodes, deduplicates, maps, and
lists those events with their onboard timestamp and severity, including
capture start/stop and A/B link loss/restoration. Events appear in the list
immediately; an event received before a following GPS fix is marked
`location pending` and is added to the map once interpolation becomes possible.
It does not receive raw
spectra, the complete `EVENTS.JSONL` diagnostic history, the complete heartbeat
state, or `MISSION.JSON`. The live information panel states those limitations
and shows receiver connection, assembly, record-type, duplicate,
acknowledgement, expiry, QoS, and error counters. A durable ground journal is
still follow-on work; closing the viewer discards its in-memory live history.

The receiver is also a public Python API and does not require Qt:

```python
from dji_h1_viewer import LiveReceiverConfig, LiveTelemetrySource

config = LiveReceiverConfig.load(
    "tools/mission_viewer/credentials/live_mqtt.local.json")
source = LiveTelemetrySource(config)
source.start()
try:
    mission_snapshot = source.snapshot()
    receiver_counters = source.status()
finally:
    source.stop()
```

The main screen links three views:

- The offline route map draws every valid GPS fix in gray, every located
  reflectance measurement in blue, and protocol/measurement incidents as red
  or orange markers. Use the mouse wheel to zoom, right-drag to pan, and
  double-click to fit the whole route.
- Clicking a blue measurement point loads its reflectance spectrum into the
  measurement panel. Hovering shows its UTC and interpolated coordinates.
- The flight panel shows mission identity, duration, segment and record counts,
  CRC status, and a selectable list of located abnormal events. The readable
  serial is display text; `drone_serial_hex` is shown as the canonical identity.

The local metric map deliberately does not download internet map tiles. That
makes the mission folder fully inspectable on an offline field computer while
preserving real geographic coordinates and distance scale.

## Baidu Map layer

The viewer can embed Baidu Map's WebGL satellite layer when internet access and
a browser-side Baidu AK are available. Road/POI vector overlays and live
traffic are explicitly disabled. Copy the safe template and put the real AK
only in the local file:

```powershell
Copy-Item tools/mission_viewer/credentials/baidu_map.example.json `
  tools/mission_viewer/credentials/baidu_map.local.json
```

`baidu_map.local.json` is explicitly ignored by Git. The tracked HTML contains
only a placeholder; Python reads the credential and injects it into the API URL
in memory. If the key, network, or web renderer is unavailable, the viewer
automatically retains the offline metric map.

Opening a mission asks before placing route, measurement, or event positions
into the remotely scripted Baidu page. Declining keeps all mission data in the
offline renderer. Coordinate conversion is intentionally disabled for now, so
recorded WGS84 positions are passed through unchanged and will be visibly
offset from Baidu's BD09 base map inside China.

The app shows sample index on the spectral x-axis because v01 does not record
a wavelength calibration. A future calibration file can map these indices to
physical wavelength without changing the binary decoder.

## Python API

The package can be installed in editable mode:

```powershell
python -m pip install -e tools/mission_viewer
```

Then scripts can use either the mission objects directly:

```python
from dji_h1_viewer import GROUND, open_mission

mission = open_mission(r"D:\F_6658")
ground_indices = mission.raw_indices(role=GROUND)
located = mission.located_raw_spectrum(ground_indices[0])
print(located.spectrum.header.utc_ms, located.spectrum.samples[:8])
print(located.position)
```

or the stable query facade also used by the UI:

```python
from dji_h1_viewer import MissionService

service = MissionService()
service.load(r"D:\F_6658")
print(service.overview())
result = service.reflectance_spectrum(0)
print(result["reflectance_percent"])
print(result["position"])
```

Opening a mission verifies every record CRC by default, and selected spectra
are revalidated when lazily read. If a power loss leaves a truncated or
CRC-damaged tail, the normal viewer/API path exposes every verified record
before that tail and reports the recovery offset, record count, and discarded
byte count. Pass `strict_products=True` to `open_mission()` or
`MissionService.load()` when batch validation should reject the whole damaged
product instead. A damaged product is reported independently so it cannot hide
usable GPS, events, metadata, or the other spectrum file. If `MISSION.JSON` is
missing or invalid, the loader tries the recorder's previous atomic checkpoint
in `MISSION.BAK` and reports which summary was used. The viewer never writes to
the mission folder.

Spectrum locations are interpolated between the valid GPS points immediately
before and after the spectrum timestamp. The default `auto` time domain uses
the synchronized A-board monotonic clock and the GPS sample's original A-board
timestamp, so UART delivery latency does not move the estimate. It falls back
to B-board monotonic receive time when synchronized A time is unavailable.
The result includes both bracketing GPS indices, the interpolation fraction,
gap duration, source sequence numbers, and a quality value of `exact`,
`interpolated`, or `wide_gap`. It never extrapolates beyond the recorded track.
Latitude/longitude and relative altitude use their independent protocol
validity bits: a usable horizontal position may therefore carry
`altitude_relative_m=None` instead of a fabricated altitude.
Pass `max_gap_ms=...` to reject estimates spanning an outage that is too long
for a particular analysis. The measurement panel also displays whether the
position used synchronized A monotonic time (including its sync generation) or
the B monotonic fallback.

## Local HTTP API

While the desktop app runs, it exposes a read-only API at
`http://127.0.0.1:8765/api/v1`. It is bound to the local computer only. Use
`--no-api` to disable it, or `--api-port 0` to select an unused port.

Available `GET` routes:

- `/health`
- `/mission`
- `/raw-index?role=ground&offset=0&limit=100`
- `/raw?index=0&time_domain=auto&max_gap_ms=1000`
- `/reflectance-index?offset=0&limit=100`
- `/reflectance?index=0&time_domain=auto&max_gap_ms=1000`
- `/position?b_monotonic_us=123456789&max_gap_ms=1000`
- `/map`
- `/gps?offset=0&limit=500`
- `/events?offset=0&limit=500`

The same routes operate on the latest one-second live snapshot. `/health`
reports `mode` as `empty`, `offline`, or `live`; `/mission` includes the live
receiver counters under `summary.live_telemetry` when connected to MQTT.

Index routes return compact metadata. Spectrum routes return the selected
sample array plus its interpolated `position` (or `null` when it cannot be
safely derived). The position route supports external data already expressed
in the B-board monotonic time domain. The map route returns the compact GPS,
located-measurement, and abnormal-event layers used by the GUI without loading
all spectrum arrays. The HTTP interface intentionally has no endpoint that
opens an arbitrary path or changes files.

## Tests

From the repository root:

```powershell
python -m unittest discover -s tests/mission_viewer -v
```

The suite includes an offscreen application-shell smoke test using a local map
stub. It loads a synthetic mission and selects a spectrum without starting the
Chromium map process or making a network request.
