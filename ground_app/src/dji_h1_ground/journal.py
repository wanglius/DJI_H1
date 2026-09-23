"""Append-only SQLite ground evidence, separate from the onboard SD format.

Only the decoder worker writes. A successful commit precedes application ACK.
Read-only replay and JSONL export never open MQTT or send acknowledgement.
"""
from __future__ import annotations

from datetime import datetime, timezone
import json
from pathlib import Path
import sqlite3
import uuid

from .live import _InboundPublication


class GroundJournal:
    def __init__(self, directory: Path):
        directory.mkdir(parents=True, exist_ok=True)
        name = datetime.now(timezone.utc).strftime("GROUND_%Y%m%dT%H%M%SZ_")
        self.path = directory / (name + uuid.uuid4().hex[:8] + ".sqlite3")
        self._db = sqlite3.connect(self.path, check_same_thread=False)
        self._db.execute("PRAGMA journal_mode=WAL")
        self._db.execute("PRAGMA synchronous=FULL")
        self._db.executescript("""
            PRAGMA user_version=1;
            CREATE TABLE publications (
                id INTEGER PRIMARY KEY, topic TEXT NOT NULL, qos INTEGER NOT NULL,
                received_utc_ns INTEGER NOT NULL, received_monotonic REAL NOT NULL,
                payload BLOB NOT NULL);
            CREATE TABLE messages (
                id INTEGER PRIMARY KEY, message_json TEXT NOT NULL);
        """)
        self._db.commit()

    def publication(self, item: _InboundPublication):
        with self._db:
            self._db.execute(
                "INSERT INTO publications VALUES(NULL,?,?,?,?,?)",
                (item.topic, item.qos, item.received_utc_ns,
                 item.received_monotonic, item.payload))

    def message(self, item):
        with self._db:
            self._db.execute("INSERT INTO messages VALUES(NULL,?)",
                             (json.dumps(item.to_dict(), ensure_ascii=False),))

    def close(self):
        self._db.close()


def _open_readonly(path):
    uri = Path(path).resolve().as_uri() + "?mode=ro"
    db = sqlite3.connect(uri, uri=True)
    if db.execute("PRAGMA user_version").fetchone()[0] != 1:
        db.close()
        raise ValueError("unsupported ground journal version")
    return db


def iter_publications(path):
    """Yield wire publications in arrival order, including malformed input."""
    db = _open_readonly(path)
    try:
        for topic, qos, utc, mono, payload in db.execute(
                "SELECT topic,qos,received_utc_ns,received_monotonic,payload "
                "FROM publications ORDER BY id"):
            yield _InboundPublication(topic, payload, qos, mono, utc)
    finally:
        db.close()


def export_jsonl(path, destination):
    """Export accepted decoded messages; refuse to overwrite an existing file."""
    db = _open_readonly(path)
    try:
        with Path(destination).open("x", encoding="utf-8") as output:
            for (record,) in db.execute("SELECT message_json FROM messages ORDER BY id"):
                output.write(record + "\n")
    finally:
        db.close()
