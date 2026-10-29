"""Decoders for the Cycle Start Watch BLE protocol (see docs/ble-protocol.md).

Pure Python, no Bluetooth needed, so it can be unit-tested and reused by
any script that post-processes downloaded logs.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass
from datetime import datetime, timezone

SERVICE_UUID = "6c1a0000-5c3e-4b8e-9a3d-2f6e7c1b9a00"
CHR_STATUS = "6c1a0001-5c3e-4b8e-9a3d-2f6e7c1b9a00"
CHR_TIME = "6c1a0002-5c3e-4b8e-9a3d-2f6e7c1b9a00"
CHR_CURSOR = "6c1a0003-5c3e-4b8e-9a3d-2f6e7c1b9a00"
CHR_PAGE = "6c1a0004-5c3e-4b8e-9a3d-2f6e7c1b9a00"
CHR_CONFIG = "6c1a0005-5c3e-4b8e-9a3d-2f6e7c1b9a00"

STATES = {0: "STOP", 1: "RUN", 2: "BLINK", 0xFF: "UNKNOWN"}
EVENT_TYPES = {1: "STATE", 2: "BOOT", 3: "POWER_DOWN", 4: "TIME_SET", 5: "CONFIG"}
EVF_TIME_INVALID = 0x01
STF_TIME_VALID, STF_RTC_PRESENT, STF_LOG_ERROR = 0x01, 0x02, 0x04

# "<" = little-endian, no padding. I = uint32, H = uint16, B = uint8.
_RECORD = struct.Struct("<IIHBBI")  # seq, ts, ms, type, flags, value
_STATUS = struct.Struct("<BBBBIIII")  # protocol, state, input, flags, now, since, first, next
RECORD_SIZE = _RECORD.size  # 16


def utc(ts: int, ms: int = 0) -> datetime:
    return datetime.fromtimestamp(ts + ms / 1000, tz=timezone.utc)


@dataclass
class Event:
    seq: int
    ts: int
    ms: int
    type: int
    flags: int
    value: int

    @property
    def time(self) -> datetime:
        return utc(self.ts, self.ms)

    @property
    def type_name(self) -> str:
        return EVENT_TYPES.get(self.type, f"TYPE_{self.type}")

    @property
    def detail(self) -> str:
        if self.type == 1:
            return STATES.get(self.value, str(self.value))
        if self.type == 4:
            return f"clock was {utc(self.value).isoformat()}"
        if self.type == 5:
            return "invert=" + ("on" if self.value & 1 else "off")
        if self.type == 2:
            return f"reset reason {self.value}"
        return ""


@dataclass
class Status:
    protocol: int
    state: int
    input: int
    flags: int
    now: int
    state_since: int
    first_seq: int
    next_seq: int

    @property
    def state_name(self) -> str:
        return STATES.get(self.state, str(self.state))


def parse_status(data: bytes) -> Status:
    return Status(*_STATUS.unpack(bytes(data[: _STATUS.size])))


def parse_page(data: bytes) -> list[Event]:
    data = bytes(data)
    if len(data) % RECORD_SIZE:
        raise ValueError(f"page length {len(data)} is not a multiple of {RECORD_SIZE}")
    return [Event(*_RECORD.unpack_from(data, i)) for i in range(0, len(data), RECORD_SIZE)]


def pack_record(e: Event) -> bytes:
    return _RECORD.pack(e.seq, e.ts, e.ms, e.type, e.flags, e.value)


def run_intervals(events: list[Event]) -> list[tuple[str, datetime, datetime]]:
    """Turn the event list into (state, start, end) intervals.

    The gap from a POWER_DOWN to the next BOOT is reported as "OFF". After a
    BOOT the state is "UNKNOWN" until the first STATE event.
    """
    out = []
    cur, start = None, None
    for e in events:
        if e.type == 1:
            new = STATES.get(e.value, str(e.value))
        elif e.type == 3:
            new = "OFF"
        elif e.type == 2:
            new = "UNKNOWN"
        else:
            continue
        if new != cur:
            if cur is not None:
                out.append((cur, start, e.time))
            cur, start = new, e.time
    return out
