# DTU telemetry stress firmware

This standalone firmware exercises the real production telemetry queue,
serializer, DTF2 fragmentation, UART sender, downlink ACK parser, and PSRAM
retention pool without the spectrometers, SD card, or A-board emulator.

After a 10-second monitor-start window it generates, for exactly 600 seconds:

- one complete 711-sample v01 reflectance record every 500 ms (1200 records);
- one moving, time-valid v01 GPS record every 200 ms (3000 records);
- UART1 traffic on GPIO17/18 at 460800 baud with no deliberate application
  gap between DTF2 fragments.

The no-gap mode does not remove DTF2 framing. It emits those independently
CRC-protected frames contiguously and lets the DTU's 1024-byte packet limit and
5 ms UART timeout decide MQTT packet boundaries. The existing desktop monitor
reconstructs records independently of those boundaries and returns DTA1
application acknowledgements.

The stress profile matches production retention policy: a 512-entry PSRAM pool,
a 3-second application-ACK timeout, one retry, and a 10-second maximum entry
residency. The DTU must first be provisioned for 460800 baud, a 1024-byte
packet limit, QoS 1 uplink, and a QoS 1 subscription to the downlink topic.

For an ACK-policy A/B comparison, build each run with a distinct mission ID.
Start the matching ground monitor before resetting the stress firmware. First
run QoS 1 ACKs:

```powershell
python tests/dtu_uart_bridge/monitor_telemetry.py `
    --host mqtt-mgnt.torchbearer.tech --mqtt-port 1883 `
    --username DJI_H1_001 --duration 720 --quiet-records `
    --mission-id 0x5354523209140101 `
    --expect-qos 1 --ack-qos 1 `
    --expect-gps-min 3000 --expect-reflectance-min 1200
```

Then repeat with mission ID `0x5354523209140102` and `--ack-qos 0`. The firmware
prints status every minute, waits up to 30 seconds after generation for retained
messages to be acknowledged or expire, then reports delivery, retries, expiry,
pool high-water mark, and ACK RTT. It finally queues a 24-spectrum backlog and
verifies that the production shutdown-abort path reclaims it within one second.

For the one-shot fire-and-forget comparison, configure with
`-D DTU_STRESS_FIRE_AND_FORGET=1`, assign a fresh mission ID, and run the
validator with `--no-ack`. The validator still CRC-checks, reassembles, and
deduplicates complete messages, but produces no downlink traffic. In this mode
the B board releases an entry only after every fragment has successfully left
its UART; it performs no application retry.

This diagnostic image replaces the production application until normal
firmware is flashed again. Reflash `build-review` after the comparison.

## Earlier 1 Hz hardware results (2026-09-13)

The tested YY-M200 firmware rejected
`AT+UART1=921600,8,1,NONE,485` and the manual's `NFC` variant with
`+ERROR:ARGS`, although the vendor manual advertises 921600 bps. The DTU was
therefore restored to its proven 460800-baud configuration before testing.

With no application fragment gap and one retry, generation admitted all 68
reflectance and 10 GPS records without a pool overflow or UART error. The host
validated all 10 GPS records and 66 reflectance records; two reflectance
messages remained unacknowledged after the 30-second drain. A full 711-sample
message occupied approximately 53.4 ms of UART time, of which only 10-17 us
was zero-gap software overhead. Pool high-watermark was 21 of 128.

A follow-up run with three retries was contaminated by delayed/offline-cached
traffic from the first run and produced a retry storm: it again ended at 66 of
68 reflectance acknowledgements, with pool high-watermark 43 and no UART
errors. This is evidence that raw UART capacity is adequate, but it is not a
qualification of zero-gap delivery. Before production adoption, repeat from a
drained DTU with offline caching disabled or isolated per mission, and use
retry backoff long enough not to amplify the DTU's cellular backlog.

The next 5 Hz controlled run uses mission ID `0x5354523209130003`, leaves the baud
rate fixed at 460800, disables the DTU offline cache, and filters receiver
statistics to that mission. The receiver still validates and acknowledges
complete stale records, allowing any earlier traffic to drain harmlessly.

## Controlled hardware result (2026-09-13)

With the receiver connected before generation, the isolated run passed. The
firmware submitted and acknowledged all 68 reflectance records and all 10 GPS
records, with zero admission drops, UART errors, or drain timeouts. The pool
high-watermark was 20 of 128. Nineteen records required retransmission; all
were recovered within the three-retry diagnostic budget. The final ACK RTT was
9.32 s and the maximum was 9.40 s, demonstrating that retained asynchronous
delivery absorbs substantial cellular/MQTT latency without blocking the
148 ms producer.

The independent broker receiver decoded all 78 records for the selected
mission (68 reflectance and 10 GPS), including every reflectance sequence from
1 through 68 and GPS sequence from 1 through 10. It received 233 MQTT packets
totalling 180759 bytes and finished with no partial record buffered.

That controlled result predates the 5 Hz FIFO correction. The current image
expects 50 GPS records in the same 10-second window; its hardware qualification
is recorded below.

## 5 Hz GPS and shutdown-abort result (2026-09-13)

The corrected FIFO implementation submitted and acknowledged all 50 GPS
records and all 68 reflectance records. There were no admission drops, UART
errors, drain timeouts, failed messages, or negative/mismatched ACKs. The pool
high-watermark was 32 of 128. The independent broker receiver decoded exactly
50 GPS and 68 reflectance records for the selected mission, reported zero
source-sequence gaps, and finished with no partial record buffered.

After delivery completed, the firmware admitted a deliberate backlog of 24
additional reflectance records and immediately invoked the production abort
path. The abort call returned in 264 us, counted all 24 retained records as
abandoned, reclaimed the pool, rejected a post-abort submission with
`ESP_ERR_INVALID_STATE`, and emitted none of those backlog records to the
broker. The firmware reported `TEST PASS`.
