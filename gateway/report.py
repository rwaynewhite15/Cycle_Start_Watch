"""Events -> intervals -> in-cycle % per shift. Pure Python, no Bluetooth.

All arithmetic is in UTC epoch milliseconds. Shifts are defined in local
time and converted with zoneinfo, so a daylight-saving change gives a 7.5 h
or 9.5 h shift, not a wrong one.
"""
from __future__ import annotations

from dataclasses import dataclass
from datetime import date, datetime, time, timedelta
from zoneinfo import ZoneInfo

from csw_protocol import EVF_TIME_INVALID, STATES, Event

STATE, BOOT, POWER_DOWN, TIME_SET, CONFIG = 1, 2, 3, 4, 5


@dataclass
class Interval:
    state: str  # RUN, STOP, BLINK, OFF, UNKNOWN
    start: int  # UTC ms
    end: int


@dataclass
class Shift:
    name: str
    start: time
    end: time  # earlier than start = ends the next day
    weekdays: set[int]  # 1 = Monday ... 7 = Sunday


@dataclass
class ShiftResult:
    shift: str
    start: int
    end: int
    seconds: dict[str, float]  # per state
    longest_stop: float        # seconds, STOP or OFF

    @property
    def length(self) -> float:
        return (self.end - self.start) / 1000

    def pct(self, state: str) -> float:
        return 100.0 * self.seconds.get(state, 0.0) / self.length if self.length else 0.0


def event_ms(e: Event) -> int:
    return e.ts * 1000 + e.ms


def corrected_times(events: list[Event]) -> list[int]:
    """Timestamps in ms, with records logged before the clock was set fixed up.

    Within one boot, a TIME_SET record says how far the clock jumped
    (ts - value). Earlier flagged records in that boot get the same shift.
    """
    out = [event_ms(e) for e in events]
    boot_start = 0
    for i, e in enumerate(events):
        if e.type == BOOT:
            boot_start = i
        elif e.type == TIME_SET:
            offset = (e.ts - e.value) * 1000
            for j in range(boot_start, i):
                if events[j].flags & EVF_TIME_INVALID:
                    out[j] += offset
    return out


def intervals(events: list[Event], until_ms: int) -> list[Interval]:
    """Contiguous states. The last one runs to `until_ms` (normally the last sync).

    POWER_DOWN..BOOT is OFF. After a BOOT the state is UNKNOWN until the first
    STATE record (about 1.5 s). Records that still have no valid clock are skipped.
    """
    times = corrected_times(events)
    out: list[Interval] = []
    cur, start = None, None
    for e, t in zip(events, times):
        if e.flags & EVF_TIME_INVALID and t < 1_000_000_000_000:  # never corrected
            continue
        if e.type == STATE:
            new = STATES.get(e.value, "UNKNOWN")
        elif e.type == POWER_DOWN:
            new = "OFF"
        elif e.type == BOOT:
            new = "UNKNOWN"
        else:
            continue
        if new == cur:
            continue
        if cur is not None and t > start:
            out.append(Interval(cur, start, t))
        cur, start = new, t
    if cur is not None and until_ms > start:
        out.append(Interval(cur, start, until_ms))
    return out


def shift_windows(shifts: list[Shift], first: date, last: date, tz: ZoneInfo) -> list[tuple[str, int, int]]:
    """Concrete (name, start_ms, end_ms) for every shift starting between first and last."""
    out = []
    d = first
    while d <= last:
        for s in shifts:
            if d.isoweekday() not in s.weekdays:
                continue
            a = datetime.combine(d, s.start, tz)
            b = datetime.combine(d + timedelta(days=1) if s.end <= s.start else d, s.end, tz)
            out.append((s.name, int(a.timestamp() * 1000), int(b.timestamp() * 1000)))
        d += timedelta(days=1)
    return sorted(out, key=lambda w: w[1])


def per_shift(ivs: list[Interval], windows: list[tuple[str, int, int]]) -> list[ShiftResult]:
    results = []
    for name, ws, we in windows:
        secs: dict[str, float] = {}
        longest = 0.0
        for iv in ivs:
            a, b = max(iv.start, ws), min(iv.end, we)
            if b <= a:
                continue
            secs[iv.state] = secs.get(iv.state, 0.0) + (b - a) / 1000
            if iv.state in ("STOP", "OFF"):
                longest = max(longest, (b - a) / 1000)
        covered = sum(secs.values())
        missing = (we - ws) / 1000 - covered
        if missing > 0.5:
            secs["NO DATA"] = missing  # not synced yet, or before the device was installed
        results.append(ShiftResult(name, ws, we, secs, longest))
    return results


def parse_shifts(cfg: list[dict]) -> list[Shift]:
    days = {"mon": 1, "tue": 2, "wed": 3, "thu": 4, "fri": 5, "sat": 6, "sun": 7}
    out = []
    for s in cfg:
        out.append(Shift(
            name=s["name"],
            start=time.fromisoformat(s["start"]),
            end=time.fromisoformat(s["end"]),
            weekdays={days[d.lower()[:3]] for d in s.get("days", ["mon", "tue", "wed", "thu", "fri"])},
        ))
    return out
