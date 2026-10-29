"""Run with: python -m unittest tools/test_csw_protocol.py"""
import struct
import unittest

from csw_protocol import Event, pack_record, parse_page, parse_status, run_intervals


class ProtocolTest(unittest.TestCase):
    def test_record_round_trip(self):
        e = Event(seq=7, ts=1767225600, ms=250, type=1, flags=0, value=1)
        raw = pack_record(e)
        self.assertEqual(len(raw), 16)
        # Same bytes the firmware would send: seq first, little-endian.
        self.assertEqual(raw[:4], b"\x07\x00\x00\x00")
        self.assertEqual(parse_page(raw), [e])
        self.assertEqual(e.detail, "RUN")

    def test_status(self):
        raw = struct.pack("<BBBBIIII", 1, 1, 1, 0x03, 1767225600, 1767225000, 1, 42)
        s = parse_status(raw)
        self.assertEqual(s.state_name, "RUN")
        self.assertEqual(s.next_seq, 42)

    def test_bad_page_length(self):
        with self.assertRaises(ValueError):
            parse_page(b"\x00" * 15)

    def test_intervals_with_power_cut(self):
        t = 1767225600
        ev = [
            Event(1, t, 0, 2, 0, 1),           # BOOT
            Event(2, t + 2, 0, 1, 0, 1),       # RUN
            Event(3, t + 600, 0, 1, 0, 0),     # STOP
            Event(4, t + 900, 0, 3, 0, 0),     # POWER_DOWN (last heartbeat)
            Event(5, t + 4000, 0, 2, 0, 1),    # BOOT
            Event(6, t + 4002, 0, 1, 0, 0),    # STOP
        ]
        names = [(s, int(a.timestamp()) - t, int(b.timestamp()) - t) for s, a, b in run_intervals(ev)]
        self.assertEqual(
            names,
            [("UNKNOWN", 0, 2), ("RUN", 2, 600), ("STOP", 600, 900), ("OFF", 900, 4000), ("UNKNOWN", 4000, 4002)],
        )


if __name__ == "__main__":
    unittest.main()
