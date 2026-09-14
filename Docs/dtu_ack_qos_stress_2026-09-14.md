# DTU application-ACK QoS stress comparison (2026-09-14)

## Purpose

Measure whether publishing the ground-to-B-board `DTA1` application ACK at
MQTT QoS 0, instead of QoS 1, provides enough capacity for the production
telemetry load.

Only the ACK publish QoS changed between runs. The DTU uplink remained QoS 1.
Both runs generated 600 seconds of deterministic, full-size production-format
traffic:

- GPS: 5 Hz, 3,000 records;
- reflectance: 2 Hz, 1,200 records, 711 samples per record;
- UART: 460,800 baud;
- application fragment gap: zero;
- B-board ACK timeout: 3 seconds, one retry;
- pool: 512 entries in PSRAM, maximum residence time 10 seconds.

## ACK QoS results

| Metric | ACK QoS 1 | ACK QoS 0 | Change |
|---|---:|---:|---:|
| Ground GPS records | 582 / 3,000 (19.4%) | 605 / 3,000 (20.2%) | +23 |
| Ground reflectance records | 57 / 1,200 (4.8%) | 92 / 1,200 (7.7%) | +35 |
| B-board ACK-cleared GPS | 525 | 574 | +49 |
| B-board ACK-cleared reflectance | 52 | 83 | +31 |
| Total B-board ACKs | 577 | 657 | +80 (13.9%) |
| ACK timeouts | 7,548 | 7,404 | -144 (1.9%) |
| Expired flight records | 3,623 | 3,543 | -80 (2.2%) |
| Send attempts | 8,125 | 8,061 | -64 (0.8%) |
| UART bytes sent | 6,500,492 | 6,419,756 | -1.2% |
| Ground MQTT messages | 1,715 | 1,730 | +0.9% |
| Ground MQTT bytes | 1,016,569 | 1,018,331 | +0.2% |
| Pool high-water mark | 72 / 512 | 71 / 512 | effectively unchanged |
| Mean ACK RTT, all types | 2.812 s | 2.640 s | -6.1% |
| UART errors / drain timeouts | 0 / 0 | 0 / 0 | unchanged |

The separate shutdown-abort probe passed in both runs. Its 24 deliberately
staged reflectance records are not included in the 1,200-record flight result.

## Conclusion

QoS 0 is the better default for `DTA1`: it removes unnecessary broker-PUBACK
work and produced a small improvement without weakening the existing B-board
timeout/retry safety net. It does **not**, however, recover enough throughput to
make one application ACK per record viable at the target load.

The almost unchanged ground MQTT byte count is the strongest result. The
limiting path is not primarily the ground publisher waiting for its QoS 1
PUBACK. Continuous per-record ACK/retry traffic and the DTU/MQTT forwarding
capacity remain the dominant constraints. Retries roughly doubled the UART
load while most original records still expired.

## Fire-and-forget follow-up

The proposed no-application-ACK/no-retry baseline was run with mission ID
`0x5354523209140103`. It used the identical input workload and transmitted each
record once. The ground validator published no DTA1 packets.

| Metric | ACK QoS 1 | ACK QoS 0 | Fire-and-forget |
|---|---:|---:|---:|
| Ground GPS records | 582 | 605 | 614 |
| Ground reflectance records | 57 | 92 | 22 |
| Total complete ground records | 639 | 697 | 636 |
| Ground MQTT messages | 1,715 | 1,730 | 1,857 |
| Ground MQTT bytes | 1,016,569 | 1,018,331 | 924,735 |
| Incomplete messages at receiver exit | 32 | 26 | 43 |
| B-board send attempts | 8,125 | 8,061 | 4,200 |
| B-board UART bytes | 6,500,492 | 6,419,756 | 3,290,400 |
| B-board pool high-water | 72 | 71 | 2 during generation |
| Retries / ACK timeouts / expirations | 3,925 / 7,548 / 3,623 | 3,861 / 7,404 / 3,543 | 0 / 0 / 0 |
| UART faults | 0 | 0 | 0 |

The B-board fire-and-forget implementation passed: all 3,000 GPS and 1,200
reflectance records left UART once, with no admission failure or backlog. The
ground result did not improve overall, however. GPS increased slightly, while
complete reflectance delivery fell to 22/1,200 because losing any one of a
record's three DTF2 fragments makes that record incomplete and there is no
retry to fill the hole.

Pure fire-and-forget is therefore not a production solution for the current
multi-fragment reflectance format. The result isolates the remaining problem
to DTU forwarding/packetization rather than MCU queue capacity. The next useful
direction is a paced DTU-input-rate test and/or a lightweight selective ACK
(sequence base plus bitmap), not a return to one blocking ACK per record.

Raw logs from these runs are local build artifacts:

- `build-dtu-stress-qos1/qos1-ack-run.log`
- `build-dtu-stress-qos1/qos0-ack-run.log`
- `build-dtu-stress-qos1/fire-forget-run.log`
