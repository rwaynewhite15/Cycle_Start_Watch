"""Bench-test client for Cycle Start Watch, for a laptop with Bluetooth.

    pip install bleak
    python tools/csw_client.py scan
    python tools/csw_client.py status   --device CSW-1A2B
    python tools/csw_client.py watch    --device CSW-1A2B      # live state changes
    python tools/csw_client.py synctime --device CSW-1A2B
    python tools/csw_client.py download --device CSW-1A2B --from 1 --csv log.csv
    python tools/csw_client.py config   --device CSW-1A2B --set '{"name":"OKUMA-07"}'

--device takes the advertised name or the Bluetooth address.

Pairing: the first secure read makes the operating system ask for the
6-digit passkey (printed on the device's serial log / label). On Linux you
may need `bluetoothctl` running with `agent on` to get that prompt; on
Windows you can also pair first in Settings > Bluetooth & devices.
"""
from __future__ import annotations

import argparse
import asyncio
import csv
import json
import struct
import sys
import time

from bleak import BleakClient, BleakScanner

import csw_protocol as p


async def find(device: str):
    if ":" in device or ("-" in device and len(device) == 36):  # MAC address, or a macOS device UUID
        return device
    found = await BleakScanner.find_device_by_name(device, timeout=10.0)
    if not found:
        sys.exit(f"device '{device}' not found - is it powered and in range?")
    return found


async def cmd_scan(_args):
    devices = await BleakScanner.discover(timeout=8.0, service_uuids=[p.SERVICE_UUID], return_adv=True)
    if not devices:
        print("no Cycle Start Watch devices found")
    for dev, adv in devices.values():
        print(f"{adv.local_name or dev.name or '?':26} {dev.address}  RSSI {adv.rssi} dBm")


def print_status(s: p.Status):
    flags = []
    if not s.flags & p.STF_TIME_VALID:
        flags.append("TIME NOT SET")
    if not s.flags & p.STF_RTC_PRESENT:
        flags.append("NO RTC")
    if s.flags & p.STF_LOG_ERROR:
        flags.append("FLASH WRITE ERROR")
    print(
        f"state={s.state_name:7} input={s.input} since={p.utc(s.state_since):%Y-%m-%d %H:%M:%S}Z "
        f"device_clock={p.utc(s.now):%H:%M:%S}Z log=[{s.first_seq}..{s.next_seq - 1}] "
        + " ".join(flags)
    )


async def cmd_status(args):
    async with BleakClient(await find(args.device)) as c:
        print_status(p.parse_status(await c.read_gatt_char(p.CHR_STATUS)))


async def cmd_watch(args):
    async with BleakClient(await find(args.device)) as c:
        print_status(p.parse_status(await c.read_gatt_char(p.CHR_STATUS)))
        await c.start_notify(p.CHR_STATUS, lambda _h, data: print_status(p.parse_status(data)))
        print("watching - Ctrl+C to stop")
        while c.is_connected:
            await asyncio.sleep(1)


async def cmd_synctime(args):
    async with BleakClient(await find(args.device)) as c:
        before = struct.unpack("<q", await c.read_gatt_char(p.CHR_TIME))[0]
        now_ms = int(time.time() * 1000)
        await c.write_gatt_char(p.CHR_TIME, struct.pack("<q", now_ms), response=True)
        await asyncio.sleep(1.5)  # device waits for the next whole second before setting the RTC
        after = struct.unpack("<q", await c.read_gatt_char(p.CHR_TIME))[0]
        print(f"device was {before - now_ms:+} ms off; now {after - int(time.time() * 1000):+} ms")


async def cmd_download(args):
    events: list[p.Event] = []
    async with BleakClient(await find(args.device)) as c:
        status = p.parse_status(await c.read_gatt_char(p.CHR_STATUS))
        seq = args.from_seq
        if seq < status.first_seq:
            print(f"note: records {seq}..{status.first_seq - 1} were overwritten (ring buffer full)")
        while True:
            await c.write_gatt_char(p.CHR_CURSOR, struct.pack("<I", seq), response=True)
            page = p.parse_page(await c.read_gatt_char(p.CHR_PAGE))
            events += page
            if len(page) < p.PAGE_RECORDS:
                break
            seq = page[-1].seq + 1
    for e in events:
        flag = " (time not set)" if e.flags & p.EVF_TIME_INVALID else ""
        stamp = e.time.strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]  # trim microseconds to ms
        print(f"#{e.seq:6} {stamp}Z  {e.type_name:10} {e.detail}{flag}")
    print(f"{len(events)} records; next download should use --from {events[-1].seq + 1 if events else args.from_seq}")
    if args.csv:
        with open(args.csv, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["seq", "utc_time", "type", "detail", "time_invalid"])
            for e in events:
                w.writerow([e.seq, e.time.isoformat(), e.type_name, e.detail, int(bool(e.flags & 1))])
        print(f"wrote {args.csv}")


async def cmd_config(args):
    async with BleakClient(await find(args.device)) as c:
        if args.set:
            json.loads(args.set)  # fail early on a typo
            await c.write_gatt_char(p.CHR_CONFIG, args.set.encode(), response=True)
            await asyncio.sleep(0.3)
        print((await c.read_gatt_char(p.CHR_CONFIG)).decode())


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("scan")
    for name in ("status", "watch", "synctime", "download", "config"):
        sp = sub.add_parser(name)
        sp.add_argument("--device", required=True)
        if name == "download":
            sp.add_argument("--from", dest="from_seq", type=int, default=1)
            sp.add_argument("--csv")
        if name == "config":
            sp.add_argument("--set", help='JSON, e.g. \'{"invert": true}\'')
    args = ap.parse_args()
    try:
        asyncio.run(globals()["cmd_" + args.cmd](args))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
