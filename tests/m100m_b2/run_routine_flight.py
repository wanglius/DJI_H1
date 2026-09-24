"""Run the existing 10-minute mission with the DTM1 ground receiver ready first.

Does not build/flash or change production settings. Creates new local evidence.
"""
import argparse
from collections import Counter
import json
from pathlib import Path
import re
import secrets
import subprocess
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'ground_app/src'))
from dji_h1_ground import GroundConfig, GroundReceiver
from dji_h1_ground.live import LiveReceiverConfig


def broker_probe(cfg):
    import paho.mqtt.client as mqtt
    ready, received = threading.Event(), threading.Event()
    token = secrets.token_hex(12)
    topic = cfg['uplink_topic'] + '/preflight/' + token
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id='preflight-'+token)
    if cfg['username']: client.username_pw_set(cfg['username'], cfg['password'])
    def connected(c, _u, _f, reason, _p):
        if not reason.is_failure: c.subscribe(topic, qos=1)
    def subscribed(_c, _u, _mid, reasons, _p):
        if reasons and not any(x.is_failure for x in reasons): ready.set()
    def message(_c, _u, msg):
        if msg.topic == topic and msg.payload == token.encode(): received.set()
    client.on_connect = connected
    client.on_subscribe = subscribed
    client.on_message = message
    try:
        client.connect(cfg['host'], cfg['port'], keepalive=30)
        client.loop_start()
        if not ready.wait(15): raise TimeoutError('broker probe SUBACK missing')
        info = client.publish(topic, token, qos=1, retain=False)
        info.wait_for_publish(timeout=5)
        if not info.is_published() or not received.wait(5):
            raise TimeoutError('broker QoS1 loopback failed')
    finally:
        client.disconnect(); client.loop_stop()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--config', type=Path, default=ROOT/'config/m100m.local.json')
    parser.add_argument('--debug-port', default='COM10')
    parser.add_argument('--emulator-port', default='COM11')
    parser.add_argument('--debug-diagnostic', action='store_true',
                        help='240-second normal start/stop/restart with USB capture instrumentation')
    parser.add_argument('--diagnostic-duration', type=int, default=240,
                        help='diagnostic mission duration, 60..600 seconds (default 240)')
    parser.add_argument('--hardware-python', required=True,
                        help='Python with pyserial and esptool for the existing mission runner')
    args = parser.parse_args()
    if not 60 <= args.diagnostic_duration <= 600:
        parser.error('diagnostic duration must be 60..600 seconds')
    cfg = json.loads(args.config.read_text(encoding='utf-8-sig'))
    args.output.mkdir(parents=True, exist_ok=False)
    receiver = GroundReceiver(GroundConfig(LiveReceiverConfig(
        host=cfg['host'], port=cfg['port'], username=cfg['username'], password=cfg['password'],
        uplink_topic=cfg['uplink_topic'], ack_topic=cfg['ack_topic'], ack_qos=0,
        client_id='m100m-routine-'+secrets.token_hex(6)),
        storage_directory=args.output, api_enabled=False), output_enabled=False)
    child = None; error = None; samples = []
    try:
        broker_probe(cfg)
        print('BROKER LOOPBACK PASSED', flush=True)
        receiver.start()
        deadline = time.monotonic()+20
        while receiver.status()['state'] != 'subscribed':
            if time.monotonic() >= deadline: raise TimeoutError('ground not subscribed')
            time.sleep(.05)
        print('GROUND READY: DTM1 + durable journal + QoS0 DTA1; launching mission', flush=True)
        command = [args.hardware_python, str(ROOT/'tests/ab_board_emulator/run_hardware_flight.py'),
            '--port', args.emulator_port, '--debug-port', args.debug_port,
            '--reset', '--report-prefix', str(args.output/'flight')]
        if args.debug_diagnostic:
            command += ['--duration', str(args.diagnostic_duration), '--capture-diagnostics', '--reopen-on-debug-silence']
        else:
            command += ['--endurance']
        with (args.output/'emulator.log').open('x',encoding='utf-8') as log:
            child = subprocess.Popen(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
            started = time.monotonic(); next_report = started
            while child.poll() is None:
                now = time.monotonic()
                if now-started > 750: raise TimeoutError('mission exceeded 750 second bound')
                if now >= next_report:
                    status = receiver.status()
                    item = {'elapsed_s':round(now-started), **{k:status[k] for k in (
                        'state','gps_records','reflectance_records','event_records',
                        'acknowledgements','invalid_messages','ingress_dropped','journal_failures')}}
                    samples.append(item); print(json.dumps(item),flush=True)
                    next_report = now+30
                time.sleep(.2)
            # Keep receiving late publications after the board's shutdown.
            time.sleep(3)
    except Exception as exc:
        error = str(exc)
    finally:
        if child is not None and child.poll() is None:
            child.terminate(); child.wait(timeout=5)
            # The terminated runner cannot execute its own safety finally.
            # Release its UART, then request shutdown; do not claim safe confirmation.
            try:
                import serial
                sys.path.insert(0, str(ROOT/'tests/ab_board_emulator'))
                from ab_board_emulator import encode_frame, CMD_POWER_OFF
                with serial.Serial(args.emulator_port,115200,timeout=.1,write_timeout=1) as uart:
                    uart.write(encode_frame(CMD_POWER_OFF,251,b'\x0a'))
            except Exception:
                error = (error or '') + '; best-effort shutdown request failed'
        receiver.stop()
    status = receiver.status()
    flight_file = args.output/'flight.json'
    flight = json.loads(flight_file.read_text()) if flight_file.exists() else {}
    log_file = args.output/'flight.log'
    text = log_file.read_text(encoding='utf-8') if log_file.exists() else ''
    tx = [(int(t),int(k),int(s)) for t,k,s in re.findall(
        r'I \((\d+)\) TELEMETRY: TX_TIMING type=(\d+) seq=(\d+) attempt=\d+ result=ESP_OK',text)]
    ack = {(int(k),int(s)) for k,s in re.findall(r'ACK_TIMING type=(\d+) seq=(\d+)',text)}
    cutoff_match = re.search(r'I \((\d+)\) AB_LINK: Power-off request:',text)
    cutoff = int(cutoff_match[1])-1000 if cutoff_match else 0
    early_missing = sorted({(k,s) for t,k,s in tx if t < cutoff}-ack)
    rtt = [int(x)/1000 for x in re.findall(r'ACK_TIMING.*rtt=(\d+)us',text)]
    faults = {k:status[k] for k in ('invalid_messages','processing_failures','journal_failures',
        'ingress_dropped','ack_queue_overflows','ack_publish_failures','qos_mismatches') if status[k]}
    passed = (error is None and child is not None and child.returncode == 0 and
        flight.get('passed',False) and not faults and not early_missing and
        status['gps_records'] > 100 and status['reflectance_records'] > 0 and status['event_records'] > 0)
    report = {'passed':passed,'error':error,'hardware_returncode':child.returncode if child else None,
        'hardware_passed':flight.get('passed',False),'hardware_failures':flight.get('hardware_failures',[]),
        'debug_capture':flight.get('debug_capture'),
        'ground':status,'ground_faults':faults,'progress':samples,
        'transmitted_attempts_by_type':dict(Counter(k for _,k,_ in tx)),
        'unique_uart_acks_by_type':dict(Counter(k for k,_ in ack)),
        'early_messages_without_uart_ack':early_missing,
        'ack_rtt_ms':{'count':len(rtt),'mean':sum(rtt)/len(rtt) if rtt else None,
                      'max':max(rtt) if rtt else None},
        'scope':(f'{args.diagnostic_duration}-second USB logging diagnostic' if args.debug_diagnostic else
                 'Routine cloudy-day flight; not a guaranteed saturation qualification')}
    (args.output/'summary.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    print('ROUTINE RESULT:',json.dumps({k:v for k,v in report.items() if k not in ('ground','progress')}),flush=True)
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
