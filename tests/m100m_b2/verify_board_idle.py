"""Bounded nighttime integration: no START command, GPS/events/ACK/shutdown only."""
import argparse
from collections import Counter
import json
import re
from pathlib import Path
import struct
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / 'ground_app/src'), str(ROOT / 'tests/ab_board_emulator')]
from dji_h1_ground import GroundConfig, GroundReceiver
from dji_h1_ground.live import LiveReceiverConfig
from ab_board_emulator import (Parser, encode_frame, handshake_payload,
    realtime_payload, CMD_HANDSHAKE, CMD_HANDSHAKE_RESPONSE, CMD_REALTIME,
    CMD_STATUS, CMD_ACK, CMD_POWER_OFF)


def main():
    import serial
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--debug-port', default='COM10')
    p.add_argument('--emulator-port', default='COM11')
    p.add_argument('--config', type=Path, default=ROOT/'config/m100m.local.json')
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--duration', type=float, default=30)
    p.add_argument('--reset', action='store_true')
    p.add_argument('--link-interruption', action='store_true',
                   help='pause A traffic at 8..12 s, then re-handshake without acquisition')
    args = p.parse_args()
    if args.debug_port == args.emulator_port or not 10 <= args.duration <= 120:
        p.error('different ports and duration 10..120 seconds required')
    cfg = json.loads(args.config.read_text(encoding='utf-8-sig'))
    args.output.mkdir(parents=True, exist_ok=False)
    settings = GroundConfig(LiveReceiverConfig(
        host=cfg['host'], port=cfg['port'], username=cfg['username'], password=cfg['password'],
        uplink_topic=cfg['uplink_topic'], ack_topic=cfg['ack_topic'],
        client_id='M100M-night-'+str(time.time_ns()), ack_qos=0),
        storage_directory=args.output, api_enabled=False)
    receiver = GroundReceiver(settings)
    stop = threading.Event()
    chunks, failures, heartbeats = [], [], []
    counts = Counter(); event_codes = Counter()
    error = None; power_ack = False; safe = False; gps_sent = 0
    thread = None
    try:
        receiver.start()
        until = time.monotonic() + 20
        while receiver.status().get('state') != 'subscribed':
            if time.monotonic() > until:
                raise TimeoutError('ground subscriber not READY: '+str(receiver.status()))
            time.sleep(.05)
        print('GROUND READY (DTM1; journaling and DTA1 enabled)', flush=True)
        with serial.Serial(args.debug_port,115200,timeout=.05) as debug, \
             serial.Serial(args.emulator_port,115200,timeout=.01,write_timeout=1) as uart, \
             (args.output/'board.log').open('x',encoding='utf-8') as log:
            debug.dtr = False
            def capture():
                try:
                    while not stop.is_set():
                        text = debug.read(debug.in_waiting or 1).decode('latin-1')
                        if text: chunks.append(text); log.write(text); log.flush()
                except Exception as exc: failures.append(str(exc))
            thread = threading.Thread(target=capture); thread.start()
            try:
                if args.reset:
                    # ESP32-S3 native USB Serial/JTAG hard reset sequence.
                    debug.rts = True; debug.dtr = False; time.sleep(.2)
                    debug.rts = False; time.sleep(.2)
                until = time.monotonic() + 90
                while 'MQTT READY' not in ''.join(chunks):
                    if time.monotonic() > until: raise TimeoutError('M100M MQTT READY missing')
                    time.sleep(.1)
                print('MODEM READY; starting idle handshake/GPS test (no spectra)',flush=True)
                parser = Parser(); boot = time.monotonic(); linked = None
                next_handshake = next_nav = boot; seq = 1; power_seq = 240
                power_at = None; last_power = 0; power_attempts = 0
                recovered = False
                while time.monotonic() - boot < args.duration + 25:
                    now = time.monotonic()
                    if linked is None and now >= next_handshake:
                        uart.write(encode_frame(CMD_HANDSHAKE, 0, handshake_payload('M100M-NIGHT-IDLE')))
                        next_handshake = now + 1
                    elapsed = now-linked if linked is not None else 0
                    paused = args.link_interruption and 8 <= elapsed < 12
                    recovering = args.link_interruption and elapsed >= 12 and not recovered
                    if recovering and now >= next_handshake:
                        uart.write(encode_frame(CMD_HANDSHAKE, 239, handshake_payload('M100M-NIGHT-IDLE')))
                        next_handshake = now + 1
                    if linked is not None and not paused and not recovering and power_at is None and now >= next_nav:
                        uart.write(encode_frame(CMD_REALTIME, seq, realtime_payload(boot)))
                        seq = (seq + 1) % 240; next_nav = now + .2; gps_sent += 1
                    if linked is not None and now-linked >= args.duration and power_at is None:
                        power_at = now
                    if power_at is not None and not power_ack and now-last_power >= .2 and power_attempts < 4:
                        uart.write(encode_frame(CMD_POWER_OFF, power_seq, b'\x0a'))
                        last_power = now; power_attempts += 1
                    for frame in parser.feed(uart.read(uart.in_waiting or 1), now):
                        if frame.command == CMD_HANDSHAKE_RESPONSE:
                            version, ready, _, echo = struct.unpack('<BBHB',frame.payload)
                            if version == 1 and ready == 1 and echo == 0:
                                linked = linked or now
                            if version == 1 and ready == 1 and echo == 239:
                                recovered = True
                        elif frame.command == CMD_ACK:
                            power_ack |= frame.payload == bytes((CMD_POWER_OFF,power_seq,0))
                        elif frame.command == CMD_STATUS:
                            status = struct.unpack('<BBBBIIBB', frame.payload)
                            heartbeats.append({'t':now-boot,'status':status})
                            if status[1] or status[4] or status[5]:
                                raise AssertionError('Unexpected spectrum acquisition in idle test')
                            safe |= bool(status[6])
                    while True:
                        message = receiver.get_message(timeout=0)
                        if message is None: break
                        counts[str(message.message_type)] += len(message.records)
                        if message.message_type == 4:
                            for record in message.records:
                                event_codes[str(record.info.event_code)] += 1
                    if power_at and now-power_at > 3 and safe: break
                    if now-boot > 15 and linked is None: raise TimeoutError('A/B handshake timeout')
                    if power_at and now-power_at > 12: raise TimeoutError('safe shutdown timeout')
                if not linked or not safe or not power_ack:
                    raise AssertionError('Missing handshake / power-off ACK / safe heartbeat')
            finally:
                stop.set(); thread.join(2)
    except Exception as exc:
        error = str(exc)
    finally:
        receiver.stop()
    text = ''.join(chunks)
    gps_received = counts['1'] + counts['5']
    status = receiver.status()
    ack_count = len(re.findall(r'ACK_TIMING type=', text))
    # A shutdown forecast intentionally cancels in-flight DTA1 waits. Require
    # confirmation of earlier traffic, not messages still arriving at shutdown.
    shutdown = re.search(r'I \((\d+)\) AB_LINK: Power-off request:', text)
    cutoff = int(shutdown[1])-1000 if shutdown else 0
    early_tx = {(int(kind),int(seq)) for tick,kind,seq in re.findall(
        r'I \((\d+)\) TELEMETRY: TX_TIMING type=(\d+) seq=(\d+) attempt=\d+ result=ESP_OK',text)
        if int(tick) < cutoff}
    matched = {(int(kind),int(seq)) for kind,seq in re.findall(
        r'ACK_TIMING type=(\d+) seq=(\d+)',text)}
    early_missing = sorted(early_tx-matched)
    passed = (error is None and not failures and gps_received >= 20 and counts['4'] >= 1
              and 'ACK_TIMING' in text and 'panic' not in text.lower()
              and not early_missing
              and not any(status[k] for k in ('invalid_messages','processing_failures', 'qos_mismatches',
                  'journal_failures','ingress_dropped','ack_queue_overflows','ack_publish_failures'))
              and (not args.link_interruption or (event_codes['13'] > 0 and event_codes['14'] > 0)))
    report = {'passed':passed,'error':error,'capture_errors':failures,
              'gps_offered':gps_sent,'gps_received':gps_received,
              'uart_dta1_matches':ack_count,'received_records_by_message_type':dict(counts),
              'early_messages_without_uart_ack':early_missing,
              'received_without_uart_ack_before_shutdown':status['complete_messages']-ack_count,
              'event_codes':dict(event_codes),'power_ack':power_ack,'safe':safe,
              'heartbeats':heartbeats,'ground':receiver.status(),
              'scope':'No START command; real spectral acquisition intentionally not tested'}
    (args.output/'summary.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    print(json.dumps({k:v for k,v in report.items() if k not in ('heartbeats','ground')},indent=2),flush=True)
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
