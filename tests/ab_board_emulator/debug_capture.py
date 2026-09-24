"""Opt-in USB capture instrumentation; never sends data or resets the MCU.

Only the reader owns port close/open. The observer may cancel a pending read,
but never closes a handle concurrently with it. Reopen preserves DTR/RTS;
driver-level line glitches remain possible, so the mission checks boot banners.
"""
import json
import threading
import time


class DiagnosticCapture:
    def __init__(self, port, log, health, chunks, errors, *, silence=10,
                 interval=1, allow_reopen=False):
        self.port, self.log, self.health = port, log, health
        self.chunks, self.errors = chunks, errors
        self.silence, self.interval = silence, interval
        self.allow_reopen = allow_reopen
        self.stop = threading.Event()
        self.reopen = threading.Event()
        self.lock = threading.Lock()
        self.started = time.monotonic()
        self.last_data = None
        self.phase_started = self.started
        self.phase = 'starting'
        self.reads = self.empty_reads = self.bytes = 0
        self.max_read_ms = self.max_gap_s = 0
        self.reopen_attempts = 0
        self.events = []
        self.cancel_attempted = False
        self.reader = threading.Thread(target=self._read, name='usb-capture', daemon=True)
        self.observer = threading.Thread(target=self._observe, name='usb-health', daemon=True)

    def _phase(self, name):
        with self.lock:
            self.phase, self.phase_started = name, time.monotonic()

    def _event(self, name, **fields):
        with self.lock:
            self.events.append(dict(t=round(time.monotonic()-self.started, 3),
                                    event=name, **fields))

    def snapshot(self):
        with self.lock:
            now = time.monotonic()
            return dict(t=round(now-self.started, 3), reader_alive=self.reader.is_alive(),
                        phase=self.phase, phase_age_s=round(now-self.phase_started, 3),
                        reads=self.reads, empty_reads=self.empty_reads, bytes=self.bytes,
                        last_data_age_s=None if self.last_data is None else round(now-self.last_data, 3),
                        max_read_ms=round(self.max_read_ms, 3), max_gap_s=round(self.max_gap_s, 3),
                        reopen_attempts=self.reopen_attempts, events=list(self.events))

    def _read(self):
        try:
            while not self.stop.is_set():
                if self.reopen.is_set():
                    self.reopen.clear()
                    self._phase('reopening')
                    # Keep the same configured object and line levels. Do not
                    # reset_input_buffer: preserve any evidence arriving on reopen.
                    dtr, rts = self.port.dtr, self.port.rts
                    self.port.close()
                    self.port.dtr, self.port.rts = dtr, rts
                    self.port.open()
                    self._event('reopened', dtr=dtr, rts=rts)
                self._phase('in_waiting')
                size = min(self.port.in_waiting or 1, 4096)
                self._phase('read')
                before = time.monotonic()
                raw = self.port.read(size)
                now = time.monotonic()
                with self.lock:
                    self.reads += 1
                    self.max_read_ms = max(self.max_read_ms, (now-before)*1000)
                    if raw:
                        if self.last_data is not None:
                            self.max_gap_s = max(self.max_gap_s, now-self.last_data)
                        self.last_data = now
                        self.bytes += len(raw)
                    else:
                        self.empty_reads += 1
                if raw:
                    self._phase('file_write_flush')
                    text = raw.decode('latin-1')
                    self.chunks.append(text)
                    self.log.write(text)
                    self.log.flush()
            self._phase('stopped')
        except Exception as exc:
            self.errors.append('USB capture: '+repr(exc))
            self._event('reader_error', error=repr(exc))
            self._phase('error')

    def _observe(self):
        try:
            while not self.stop.wait(self.interval):
                sample = self.snapshot()
                self.health.write(json.dumps(sample)+'\n')
                self.health.flush()
                # A previously stuck endpoint can remain silent from open;
                # absence of the first byte needs the same recovery as a gap.
                age = sample['last_data_age_s'] if sample['last_data_age_s'] is not None else sample['t']
                if (self.allow_reopen and not self.reopen_attempts
                        and age >= self.silence and sample['reader_alive']):
                    with self.lock:
                        self.reopen_attempts += 1
                    self._event('silence_reopen_requested', silence_s=age)
                    self.reopen.set()
                # A blocked read cannot reach the reopen request. Cancel only
                # after it has exceeded the normal 100 ms timeout by seconds.
                if (self.reopen.is_set() and sample['phase'] == 'read'
                        and sample['phase_age_s'] >= self.silence and not self.cancel_attempted):
                    self.cancel_attempted = True
                    self.port.cancel_read()
                    self._event('blocked_read_cancelled')
        except Exception as exc:
            self.errors.append('USB health observer: '+repr(exc))

    def start(self):
        self.reader.start()
        self.observer.start()

    def finish(self):
        self.stop.set()
        self.observer.join(2)
        self.reader.join(2)
        if self.reader.is_alive():
            self.port.cancel_read()
            self.reader.join(2)
        if self.reader.is_alive() or self.observer.is_alive():
            self.errors.append('USB diagnostic worker failed to stop')
        result = self.snapshot()
        self.health.write(json.dumps(dict(final=True, **result))+'\n')
        self.health.flush()
        return result
