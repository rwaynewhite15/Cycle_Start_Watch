"""Run with: cd gateway && python -m unittest -v test_gateway

No Bluetooth needed: a fake device stands in for the ESP32 and answers
reads and writes the way the firmware does.
"""
import asyncio
import struct
import sys
import tempfile
import types
import unittest
from datetime import date, datetime, time as dtime
from pathlib import Path
from zoneinfo import ZoneInfo

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))

import csw_protocol as p  # noqa: E402
import report  # noqa: E402
from store import Store  # noqa: E402

T0 = 1767225600  # 2026-01-01 00:00 UTC


def ev(seq, t, typ, value=0, flags=0, ms=0):
    return p.Event(seq, t, ms, typ, flags, value)


# --------------------------------------------------------- fake ESP32 ----
class FakeDevice:
    """Mimics the firmware's GATT behaviour, including the ring buffer."""

    def __init__(self, events, capacity=10_000):
        self.events = events
        self.capacity = capacity
        self.cursor = 0
        self.clock_set_to = None
        self.address = "AA:BB:CC:DD:EE:01"
        self.name = "OKUMA-07"

    def first_seq(self):
        if not self.events:
            return 0
        return max(self.events[0].seq, self.events[-1].seq - self.capacity + 1)

    def status(self):
        nxt = self.events[-1].seq + 1 if self.events else 1
        return struct.pack("<BBBBIIII", 1, 1, 1, 3, T0, T0, self.first_seq(), nxt)

    def page(self):
        start = max(self.cursor, self.first_seq())
        recs = [e for e in self.events if e.seq >= start][: p.PAGE_RECORDS]
        return b"".join(p.pack_record(e) for e in recs)


class FakeClient:
    device = None

    def __init__(self, dev, timeout=None):
        self.d = dev

    async def __aenter__(self):
        return self

    async def __aexit__(self, *a):
        return False

    async def read_gatt_char(self, uuid):
        return {p.CHR_STATUS: self.d.status, p.CHR_PAGE: self.d.page}[uuid]()

    async def write_gatt_char(self, uuid, data, response=True):
        if uuid == p.CHR_CURSOR:
            self.d.cursor = struct.unpack("<I", data)[0]
        elif uuid == p.CHR_TIME:
            self.d.clock_set_to = struct.unpack("<q", data)[0]


sys.modules["bleak"] = types.SimpleNamespace(BleakClient=FakeClient, BleakScanner=None)
import csw_gateway  # noqa: E402


def cfg(**kw):
    c = dict(csw_gateway.DEFAULTS)
    c["set_device_clock"] = "always"
    c.update(kw)
    return c


class SyncTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.store = Store(str(Path(self.tmp.name) / "t.sqlite"))

    def tearDown(self):
        self.store.close()
        self.tmp.cleanup()

    def sync(self, dev, **kw):
        self.store.upsert_device(dev.address, dev.name)
        asyncio.run(csw_gateway.sync_device(self.store, dev, cfg(**kw)))

    def test_full_then_incremental_download(self):
        dev = FakeDevice([ev(i, T0 + i, 1, i % 2) for i in range(1, 101)])  # 100 records = 4 pages
        self.sync(dev)
        self.assertEqual(len(self.store.events(dev.address)), 100)
        self.assertIsNotNone(dev.clock_set_to)

        dev.events += [ev(i, T0 + i, 1, i % 2) for i in range(101, 106)]
        self.sync(dev)
        self.assertEqual([e.seq for e in self.store.events(dev.address)], list(range(1, 106)))
        log = self.store.db.execute("SELECT ok, new_events FROM sync_log ORDER BY id").fetchall()
        self.assertEqual(log, [(1, 100), (1, 5)])

    def test_exactly_one_full_page(self):
        dev = FakeDevice([ev(i, T0 + i, 1, 1) for i in range(1, 33)])
        self.sync(dev)
        self.assertEqual(len(self.store.events(dev.address)), 32)

    def test_overwritten_records_are_reported(self):
        dev = FakeDevice([ev(i, T0 + i, 1, i % 2) for i in range(1, 51)], capacity=20)
        self.sync(dev)
        seqs = [e.seq for e in self.store.events(dev.address)]
        self.assertEqual(seqs, list(range(31, 51)))
        lost = self.store.db.execute("SELECT lost_from, lost_to FROM sync_log").fetchone()
        self.assertEqual(lost, (1, 30))

    def test_clock_not_pushed_when_untrusted(self):
        dev = FakeDevice([ev(1, T0, 2)])
        self.sync(dev, set_device_clock="never")
        self.assertIsNone(dev.clock_set_to)

    def test_failure_is_logged_not_raised(self):
        dev = FakeDevice([])
        dev.status = lambda: (_ for _ in ()).throw(OSError("Insufficient authentication"))
        self.sync(dev)
        ok, msg = self.store.db.execute("SELECT ok, message FROM sync_log").fetchone()
        self.assertEqual(ok, 0)
        self.assertIn("paired", msg)


class ReportTest(unittest.TestCase):
    def test_intervals_with_power_cut(self):
        events = [ev(1, T0, 2), ev(2, T0 + 2, 1, 1), ev(3, T0 + 600, 1, 0),
                  ev(4, T0 + 900, 3), ev(5, T0 + 4000, 2), ev(6, T0 + 4002, 1, 0)]
        ivs = report.intervals(events, (T0 + 5000) * 1000)
        got = [(i.state, i.start // 1000 - T0, i.end // 1000 - T0) for i in ivs]
        self.assertEqual(got, [("UNKNOWN", 0, 2), ("RUN", 2, 600), ("STOP", 600, 900),
                               ("OFF", 900, 4000), ("UNKNOWN", 4000, 4002), ("STOP", 4002, 5000)])

    def test_unset_clock_records_corrected_by_time_set(self):
        # Device booted with a dead coin cell: clock started at 0 (1970).
        events = [ev(1, 0, 2, flags=1), ev(2, 10, 1, 1, flags=1),
                  ev(3, T0 + 100, 4, value=100)]  # phone set the clock: was 100 s, now T0+100
        times = report.corrected_times(events)
        self.assertEqual([t // 1000 for t in times], [T0, T0 + 10, T0 + 100])

    def test_shift_windows_overnight_and_dst(self):
        tz = ZoneInfo("America/Detroit")
        shifts = report.parse_shifts([{"name": "C", "start": "23:00", "end": "06:00", "days": ["sat"]}])
        # US DST ends Sunday 2026-11-01 at 02:00 -> Saturday night's C shift is 8 h, not 7.
        (name, a, b), = report.shift_windows(shifts, date(2026, 10, 31), date(2026, 10, 31), tz)
        self.assertEqual((b - a) / 3_600_000, 8.0)
        self.assertEqual(datetime.fromtimestamp(b / 1000, tz).time(), dtime(6, 0))

    def test_per_shift_percentages(self):
        # 8 h shift from T0: RUN 6 h, STOP 1.5 h, last 0.5 h not synced yet.
        ivs = [report.Interval("RUN", T0 * 1000, (T0 + 3 * 3600) * 1000),
               report.Interval("STOP", (T0 + 3 * 3600) * 1000, (T0 + 4.5 * 3600) * 1000),
               report.Interval("RUN", (T0 + 4.5 * 3600) * 1000, (T0 + 7.5 * 3600) * 1000)]
        (r,) = report.per_shift(ivs, [("A", T0 * 1000, (T0 + 8 * 3600) * 1000)])
        self.assertAlmostEqual(r.pct("RUN"), 75.0)
        self.assertAlmostEqual(r.pct("STOP"), 18.75)
        self.assertAlmostEqual(r.pct("NO DATA"), 6.25)
        self.assertEqual(r.longest_stop, 1.5 * 3600)


if __name__ == "__main__":
    unittest.main()
