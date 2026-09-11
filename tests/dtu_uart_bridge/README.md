# DTU UART bridge test firmware

This standalone ESP-IDF application temporarily turns the ESP32-S3 into a
transparent serial bridge:

```text
PC COM port <-> native USB Serial/JTAG <-> ESP32-S3 UART1 <-> 4G DTU
                                               TX GPIO17 -> DTU RX
                                               RX GPIO18 <- DTU TX
```

The DTU manual specifies default serial framing of **115200 baud, 8 data bits,
no parity, and 1 stop bit**. This test firmware deliberately uses **460800
baud** after the DTU has been migrated, providing enough throughput for full
reflectance records. All bridge parameters are grouped in
`main/dtu_bridge_config.h`.

The bridge neither parses nor changes bytes. It emits one startup banner to the
PC, then disables application logging so text or binary DTU traffic remains
clean. If the PC closes the COM port, unread PC-bound data may be discarded;
the firmware never blocks the DTU receive path indefinitely.

## Build and flash

From the repository root, with the ESP-IDF environment loaded:

```powershell
idf.py -C tests/dtu_uart_bridge -B build-dtu-bridge `
    -DIDF_TARGET=esp32s3 build
idf.py -C tests/dtu_uart_bridge -B build-dtu-bridge -p COM6 flash
```

Open COM6 at 115200 baud with a serial terminal. Commands typed on the PC are
sent unchanged to the DTU, and bytes returned by the DTU are shown unchanged.
AT commands normally require CRLF line endings. The DTU boots in transparent
mode; follow the manual's guarded entry sequence before sending configuration
AT commands.

For a non-destructive status check, install `pyserial` and run:

```powershell
python tests/dtu_uart_bridge/probe_dtu.py --port COM6
```

The probe enters AT mode temporarily, reads cellular and MQTT settings, and
returns to transparent mode. It intentionally does not query `MQAUTH1`, because
that response contains the stored MQTT password.

The EMQX endpoint can be checked independently of the DTU without installing an
MQTT package:

```powershell
python tests/dtu_uart_bridge/mqtt_broker_probe.py `
    --host mqtt.example.com --port 1883 --username device_test --qos 1
```

This creates two short-lived, unique MQTT 3.1.1 clients, subscribes one client,
publishes a unique payload from the other, verifies the received bytes, and
disconnects both clients. It never uses or requests a password.

After validating the broker, copy `dtu_mqtt_config.example.json` to
`dtu_mqtt_config.local.json` and edit the local copy. The local file holds the
server, port, device identity, credentials, topics, QoS, transport behavior,
and MCU/DTU UART settings. It is ignored by Git so deployment credentials do
not enter a commit. The tracked example documents the complete schema without
containing live settings.

The DTU parser requires a non-empty password field even when the broker permits
anonymous clients. Supply the real password for an authenticated listener, or
a literal non-secret placeholder such as `unused` for an anonymous listener.
The `mcu_uart` object documents the production wiring and programs the DTU UART.
The host utility cannot change the running ESP32 bridge, so the bridge firmware
must be built for the same target baud before communication resumes after the
DTU reboot.

Validate the file without opening COM6 or changing the DTU:

```powershell
python tests/dtu_uart_bridge/configure_dtu_mqtt.py `
    --config tests/dtu_uart_bridge/dtu_mqtt_config.local.json `
    --validate-only
```

When the SIM is registered and the settings are ready to apply, run:

```powershell
python tests/dtu_uart_bridge/configure_dtu_mqtt.py `
    --config tests/dtu_uart_bridge/dtu_mqtt_config.local.json
```

The initializer validates the entire JSON before opening the serial port. It
then disables Socket A while changing its settings, enables it only after all
commands succeed, and reboots the DTU because configuration changes take effect
after restart. It never prints the `MQAUTH1` command or password. If any command
fails, it stops immediately and exits AT mode without rebooting.

After Socket A reports online, verify the complete transparent path in both
directions:

```powershell
python tests/dtu_uart_bridge/verify_dtu_mqtt_path.py `
    --serial-port COM6 --host mqtt.example.com --mqtt-port 1883 `
    --username device_test
```

The verifier sends a unique serial payload through the DTU and confirms it on
the configured uplink topic. It then publishes a different unique payload to
the downlink topic and confirms the same bytes arrive on the serial port.
Use `--uplink-bytes 3172` to exercise one full-size v01 reflectance record's
payload volume. Because the DTU packetizer is capped at 1024 bytes, the verifier
reassembles consecutive MQTT payloads and reports their count while requiring
QoS 1 on every uplink publication.

To verify the actual `DTF2` application fragments and receiver reassembly
through the hardware path, run:

```powershell
python tests/dtu_uart_bridge/verify_fragmented_telemetry.py `
    --serial-port COM6 --host mqtt.example.com --mqtt-port 1883 `
    --username device_test --message-bytes 3172
```

This sends the four bounded application fragments separately, recovers them
even if the DTU changes their MQTT boundaries, and requires the final payload
to match the source byte-for-byte. The MQTT receiver runs concurrently so QoS
1 acknowledgements are not delayed while the serial burst is being generated.
For a paced burst test, add `--message-count 12 --fragment-gap-ms 100`. Setting
the fragment gap to zero deliberately floods the DTU and is useful for finding
its buffering limit; it is not the intended production scheduling policy.

During a production-firmware mission, start the validator/acknowledger before the
A-board emulator:

```powershell
python tests/dtu_uart_bridge/monitor_telemetry.py `
    --host mqtt.example.com --mqtt-port 1883 --username device_test `
    --duration 600 --expect-gps-min 500 --expect-reflectance-min 500
```

It does not use COM6. It subscribes to the uplink topic, handles
arbitrary DTU chunk boundaries and QoS 1 duplicates, validates both DTF2 and
DHR1 CRCs, publishes `DTA1` application acknowledgements on the downlink topic,
and reports record counts plus intentionally skipped source-record sequences.
The production firmware deliberately treats a missing application ACK as a
telemetry fault, so MQTTX alone is useful for inspection but cannot replace
this validator (or the future production receiver service).

Flashing this diagnostic image replaces the production application until the
normal project firmware is flashed again.
