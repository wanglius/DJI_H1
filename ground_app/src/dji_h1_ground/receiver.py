"""Public receiver with independent MQTT, decoding, storage, ACK and query APIs."""
from __future__ import annotations

from queue import Empty, Full, Queue
from threading import RLock
import time

from .api import MissionService
from .config import GroundConfig
from .journal import GroundJournal, iter_publications
from .live import LiveTelemetrySource, LiveMissionStore
from .messages import decode_message


class ReceiverService(MissionService):
    """Queries refresh from the worker store even when no GUI event loop runs."""
    def __init__(self, source):
        super().__init__()
        self._source = source
        self._snapshot_revision = -1
        self._snapshot_lock = RLock()

    @property
    def mission(self):
        with self._snapshot_lock:
            revision = self._source.revision
            if revision != self._snapshot_revision:
                self._mission = self._source.snapshot()
                self._snapshot_revision = revision
            return self._mission

    @property
    def mode(self):
        return "live"

    @property
    def live_status(self):
        return self._source.status()

    def overview(self):
        result = super().overview()
        # Status changes without record revisions (e.g. disconnects).
        if "summary" in result:
            result["summary"] = {**result["summary"],
                                 "live_telemetry": self.live_status}
        return result


class GroundReceiver(LiveTelemetrySource):
    """One receiver per MQTT source stream. Consumers poll typed messages.

    Use get_message()/iter_messages() on your own application worker. Slow
    consumers cannot delay network reception; output overflow is counted.
    Construct a new receiver after stop(); objects are deliberately single-use.
    """
    def __init__(self, config: GroundConfig, *, output_enabled: bool = True):
        super().__init__(config.mqtt)
        self.settings = config
        self.output_enabled = output_enabled
        self.service = ReceiverService(self)
        self._output = Queue(maxsize=config.output_queue_size)
        self._journal = None
        self._used = False
        self._replaying = False
        self._lifecycle = RLock()
        self._status.update(output_dropped=0, journal_failures=0,
                            journal_path="", decoded_messages=0, filtered_messages=0)

    @classmethod
    def from_config(cls, path):
        return cls(GroundConfig.load(path))

    def start(self):
        with self._lifecycle:
            if self._running:
                return
            if self._used:
                raise RuntimeError("Create a new GroundReceiver for a new session")
            self._used = True
            try:
                if self.settings.storage_enabled:
                    self._journal = GroundJournal(self.settings.storage_directory)
                    self._status["journal_path"] = str(self._journal.path)
                super().start()
            except Exception:
                self._transport.stop()
                if self._journal is not None:
                    self._journal.close()
                    self._journal = None
                raise

    def _stop_workers(self):
        # Keep references when a worker stalls: do not close a journal still in use.
        self._processor_stop.set()
        if self._processor_thread is not None:
            self._processor_thread.join(self._WORKER_JOIN_SECONDS)
            if self._processor_thread.is_alive():
                raise RuntimeError("Decoder still draining; retry stop() later")
        self._ack_stop.set()
        if self._ack_thread is not None:
            self._ack_thread.join(self._WORKER_JOIN_SECONDS)
            if self._ack_thread.is_alive():
                raise RuntimeError("ACK worker still draining; retry stop() later")
        self._processor_thread = self._ack_thread = None

    def stop(self):
        with self._lifecycle:
            super().stop()
            if self._journal is not None:
                self._journal.close()
                self._journal = None

    def status(self):
        result = super().status()
        result["output_queue_depth"] = self._output.qsize()
        return result

    def _write_journal(self, method, item):
        if self._journal is not None:
            try:
                getattr(self._journal, method)(item)
            except Exception:
                with self._lock:
                    self._status["journal_failures"] += 1
                # Infrastructure failure: no cache/ACK; sender may retry.
                raise OSError("ground journal write failed") from None

    def _process_publication(self, publication):
        self._write_journal("publication", publication)
        return super()._process_publication(publication)

    def _accept_message(self, completed, publication):
        # Apply identity policy before recording any accepted decoded output.
        if ((self.config.source_id is not None and
             completed.source_id != self.config.source_id) or
            (self.config.mission_id is not None and
             completed.mission_id != self.config.mission_id)):
            with self._lock:
                self._status["filtered_messages"] += 1
            return LiveMissionStore.FILTERED
        decoded = decode_message(
            completed, received_utc_ns=publication.received_utc_ns,
            received_monotonic=publication.received_monotonic,
            topic=publication.topic, qos=publication.qos)
        self._write_journal("message", decoded)
        added = self.store.accept(completed)
        with self._lock:
            self._status["decoded_messages"] += 1
        if self.output_enabled:
            try:
                self._output.put_nowait(decoded)
            except Full:
                with self._lock:
                    self._status["output_dropped"] += 1
        return added

    def _queue_ack(self, encoded):
        if not self._replaying:
            super()._queue_ack(encoded)

    def get_message(self, timeout: float | None = None):
        """Return DecodedMessage, or None on timeout. Single-consumer queue."""
        try:
            return self._output.get(timeout=timeout)
        except Empty:
            return None

    def iter_messages(self):
        """Poll from an application worker; ends after stop and queue drainage."""
        while True:
            processor = self._processor_thread
            draining = processor is not None and processor.is_alive()
            if not self._running and not draining and self._output.empty():
                break
            message = self.get_message(timeout=0.2)
            if message is not None:
                yield message

    def replay(self, path, *, speed: float = 0):
        """Yield decoded records from a journal; no network, ACK, or disk writes.

        speed=0 runs as fast as possible, speed=1 follows recorded timing.
        Consume this generator to completion before requesting a final snapshot.
        """
        if speed < 0:
            raise ValueError("speed must be nonnegative")
        with self._lifecycle:
            if self._used:
                raise RuntimeError("Replay requires a fresh GroundReceiver")
            self._used = True
            self._replaying = True
        previous = None
        self._status["state"] = "replaying"
        try:
            for item in iter_publications(path):
                if previous is not None and speed:
                    time.sleep(max(0, item.received_monotonic - previous) / speed)
                previous = item.received_monotonic
                self._process_publication(item)
                while True:
                    message = self.get_message(timeout=0)
                    if message is None:
                        break
                    yield message
        finally:
            self._status["state"] = "replay complete"

    def __enter__(self):
        self.start()
        return self

    def __exit__(self, *_args):
        self.stop()
