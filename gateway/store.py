"""SQLite storage for the gateway.

One file holds everything. WAL mode lets you query the database (sqlite3
CLI, DB Browser, Python, Grafana) while the gateway is writing to it.
"""
from __future__ import annotations

import sqlite3
import time
from typing import Iterable

from csw_protocol import Event

SCHEMA = """
CREATE TABLE IF NOT EXISTS device (
  address      TEXT PRIMARY KEY,   -- Bluetooth MAC address
  name         TEXT,               -- machine name the device advertises
  first_seen   INTEGER NOT NULL,   -- UTC seconds (gateway clock)
  last_sync    INTEGER,            -- UTC seconds of last successful sync
  last_state   INTEGER,            -- 0 STOP, 1 RUN, 2 BLINK, 255 UNKNOWN
  last_rssi    INTEGER
);

-- Raw records exactly as the device logged them. (address, seq) is unique,
-- so downloading the same record twice is harmless.
CREATE TABLE IF NOT EXISTS event (
  address  TEXT NOT NULL,
  seq      INTEGER NOT NULL,
  ts       INTEGER NOT NULL,        -- UTC seconds
  ms       INTEGER NOT NULL,
  type     INTEGER NOT NULL,        -- 1 STATE, 2 BOOT, 3 POWER_DOWN, 4 TIME_SET, 5 CONFIG
  flags    INTEGER NOT NULL,        -- bit0 = device clock was not set
  value    INTEGER NOT NULL,
  PRIMARY KEY (address, seq)
) WITHOUT ROWID;

-- One row per sync attempt: the audit trail for "why is data missing?".
CREATE TABLE IF NOT EXISTS sync_log (
  id         INTEGER PRIMARY KEY,
  address    TEXT NOT NULL,
  at         INTEGER NOT NULL,      -- UTC seconds
  ok         INTEGER NOT NULL,
  new_events INTEGER NOT NULL DEFAULT 0,
  lost_from  INTEGER,               -- set if records were overwritten before we got them
  lost_to    INTEGER,
  message    TEXT
);

-- Handy for ad-hoc SQL: readable times and names.
CREATE VIEW IF NOT EXISTS event_readable AS
SELECT e.address, d.name, e.seq,
       strftime('%Y-%m-%d %H:%M:%f', e.ts + e.ms / 1000.0, 'unixepoch') AS utc_time,
       CASE e.type WHEN 1 THEN 'STATE' WHEN 2 THEN 'BOOT' WHEN 3 THEN 'POWER_DOWN'
                   WHEN 4 THEN 'TIME_SET' WHEN 5 THEN 'CONFIG' ELSE e.type END AS type,
       CASE WHEN e.type = 1 THEN
            CASE e.value WHEN 0 THEN 'STOP' WHEN 1 THEN 'RUN' WHEN 2 THEN 'BLINK' ELSE e.value END
       ELSE e.value END AS value,
       e.flags
FROM event e LEFT JOIN device d USING (address);
"""


class Store:
    def __init__(self, path: str):
        self.db = sqlite3.connect(path)
        self.db.execute("PRAGMA journal_mode=WAL")
        self.db.execute("PRAGMA synchronous=NORMAL")  # safe with WAL; fewer SD-card writes
        self.db.executescript(SCHEMA)

    def close(self):
        self.db.close()

    def next_seq(self, address: str) -> int:
        row = self.db.execute("SELECT MAX(seq) FROM event WHERE address = ?", (address,)).fetchone()
        return (row[0] or 0) + 1

    def upsert_device(self, address: str, name: str | None, rssi: int | None = None):
        now = int(time.time())
        self.db.execute(
            """INSERT INTO device (address, name, first_seen, last_rssi) VALUES (?, ?, ?, ?)
               ON CONFLICT(address) DO UPDATE SET
                 name = COALESCE(excluded.name, device.name),
                 last_rssi = COALESCE(excluded.last_rssi, device.last_rssi)""",
            (address, name, now, rssi),
        )
        self.db.commit()

    def add_events(self, address: str, events: Iterable[Event]) -> int:
        cur = self.db.executemany(
            "INSERT OR IGNORE INTO event (address, seq, ts, ms, type, flags, value) VALUES (?, ?, ?, ?, ?, ?, ?)",
            [(address, e.seq, e.ts, e.ms, e.type, e.flags, e.value) for e in events],
        )
        self.db.commit()
        return cur.rowcount

    def record_sync(self, address: str, ok: bool, new_events: int = 0, state: int | None = None,
                    lost: tuple[int, int] | None = None, message: str = ""):
        now = int(time.time())
        self.db.execute(
            "INSERT INTO sync_log (address, at, ok, new_events, lost_from, lost_to, message) VALUES (?, ?, ?, ?, ?, ?, ?)",
            (address, now, int(ok), new_events, lost[0] if lost else None, lost[1] if lost else None, message),
        )
        if ok:
            self.db.execute("UPDATE device SET last_sync = ?, last_state = ? WHERE address = ?", (now, state, address))
        self.db.commit()

    def devices(self) -> list[sqlite3.Row]:
        self.db.row_factory = sqlite3.Row
        try:
            return self.db.execute("SELECT * FROM device ORDER BY name").fetchall()
        finally:
            self.db.row_factory = None

    def events(self, address: str) -> list[Event]:
        rows = self.db.execute(
            "SELECT seq, ts, ms, type, flags, value FROM event WHERE address = ? ORDER BY seq", (address,)
        ).fetchall()
        return [Event(*r) for r in rows]
