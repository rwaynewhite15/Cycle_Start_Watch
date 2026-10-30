"""Cycle Start Watch gateway: collects every nearby device's log into SQLite.

    python csw_gateway.py run                  # the service: sync all devices every few minutes
    python csw_gateway.py once                 # one sync pass, then exit (good for testing)
    python csw_gateway.py devices              # what the gateway knows about
    python csw_gateway.py report --days 7      # in-cycle % per shift
    python csw_gateway.py export --days 7 --csv out.csv

Options come from gateway.toml (see gateway.example.toml); --config picks another file.
"""
from __future__ import annotations

import argparse
import asyncio
import csv
import logging
import os
import struct
import subprocess
import sys
import time
import tomllib
from datetime import datetime, timedelta
from pathlib import Path
from zoneinfo import ZoneInfo

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "tools"))  # shared protocol decoder

import csw_protocol as p  # noqa: E402
import report  # noqa: E402
from store import Store  # noqa: E402

log = logging.getLogger("gateway")

DEFAULTS = {
    "database": str(HERE / "csw.sqlite"),
    "interval_s": 300,
    "scan_s": 10,
    "connect_timeout_s": 20,
    "set_device_clock": "ntp",  # "ntp" = only when this Pi's clock is NTP-synced; "always"; "never"
    "devices": [],              # empty = every Cycle Start Watch in range; else a list of MAC addresses
    "timezone": "America/Detroit",
    "shifts": [],
}


def load_config(path: Path) -> dict:
    cfg = dict(DEFAULTS)
    if path.exists():
        with open(path, "rb") as f:
            cfg.update(tomllib.load(f))
    cfg["devices"] = [a.upper() for a in cfg["devices"]]
    return cfg


# ------------------------------------------------------------------ clock --
def pi_clock_trusted(mode: str) -> bool:
    """A Raspberry Pi has no battery clock. After a boot without network its
    time can be days off, and pushing that to the devices would corrupt
    every timestamp. So by default we only set device clocks while the Pi
    is synchronized to NTP (or has an RTC HAT that systemd-timesyncd trusts)."""
    if mode == "always":
        return True
    if mode == "never":
        return False
    try:
        out = subprocess.run(["timedatectl", "show", "-p", "NTPSynchronized", "--value"],
                             capture_output=True, text=True, timeout=5).stdout.strip()
        return out == "yes"
    except (OSError, subprocess.SubprocessError):
        return False


# ------------------------------------------------------------------- sync --
async def sync_device(store: Store, dev, cfg: dict) -> None:
    """Connect to one device, set its clock, download new records."""
    from bleak import BleakClient

    address = dev.address.upper()
    try:
        async with BleakClient(dev, timeout=cfg["connect_timeout_s"]) as c:
            status = p.parse_status(await c.read_gatt_char(p.CHR_STATUS))

            if pi_clock_trusted(cfg["set_device_clock"]):
                await c.write_gatt_char(p.CHR_TIME, struct.pack("<q", int(time.time() * 1000)), response=True)
            elif not status.flags & p.STF_TIME_VALID:
                log.warning("%s: device clock not set and Pi clock not trusted - timestamps will be flagged", address)

            seq = store.next_seq(address)
            lost = None
            if status.first_seq and seq < status.first_seq:
                lost = (seq, status.first_seq - 1)
                log.warning("%s: records %d..%d were overwritten before they could be collected", address, *lost)

            total = 0
            while True:
                await c.write_gatt_char(p.CHR_CURSOR, struct.pack("<I", seq), response=True)
                page = p.parse_page(await c.read_gatt_char(p.CHR_PAGE))
                total += store.add_events(address, page)  # commit page by page: a drop loses nothing
                if len(page) < p.PAGE_RECORDS:
                    break
                seq = page[-1].seq + 1

            store.record_sync(address, True, total, status.state, lost)
            log.info("%s (%s): %d new records, state %s", address, dev.name, total, status.state_name)
    except Exception as exc:  # one bad device must not stop the others
        msg = f"{type(exc).__name__}: {exc}"
        if "auth" in msg.lower() or "encrypt" in msg.lower():
            msg += " - is this device paired with the Pi? See gateway/README.md"
        store.record_sync(address, False, message=msg)
        log.warning("%s: sync failed: %s", address, msg)


async def sync_pass(store: Store, cfg: dict) -> None:
    from bleak import BleakScanner

    found = await BleakScanner.discover(timeout=cfg["scan_s"], service_uuids=[p.SERVICE_UUID], return_adv=True)
    targets = []
    for dev, adv in found.values():
        if cfg["devices"] and dev.address.upper() not in cfg["devices"]:
            continue
        store.upsert_device(dev.address.upper(), adv.local_name or dev.name, adv.rssi)
        targets.append(dev)
    missing = set(cfg["devices"]) - {d.address.upper() for d in targets}
    for a in sorted(missing):
        log.warning("%s: not seen in this scan (powered off or out of range?)", a)
    # One at a time: the Pi's Bluetooth adapter is happiest with a single connection.
    for dev in sorted(targets, key=lambda d: d.address):
        await sync_device(store, dev, cfg)


async def run_forever(store: Store, cfg: dict) -> None:
    while True:
        started = time.monotonic()
        try:
            await sync_pass(store, cfg)
        except Exception:
            log.exception("sync pass failed (Bluetooth adapter problem?)")
        await asyncio.sleep(max(5, cfg["interval_s"] - (time.monotonic() - started)))


# ---------------------------------------------------------------- reports --
def fmt_dur(s: float) -> str:
    s = int(s)
    return f"{s // 3600}:{s % 3600 // 60:02d}:{s % 60:02d}"


def cmd_devices(store: Store, _args, _cfg):
    rows = store.devices()
    if not rows:
        print("no devices yet - run `csw_gateway.py once`")
    now = time.time()
    for d in rows:
        ago = f"{(now - d['last_sync']) / 60:.0f} min ago" if d["last_sync"] else "never"
        state = p.STATES.get(d["last_state"], "-") if d["last_state"] is not None else "-"
        n = store.db.execute("SELECT COUNT(*) FROM event WHERE address=?", (d["address"],)).fetchone()[0]
        print(f"{d['name'] or '?':24} {d['address']}  last sync {ago:14} state {state:7} {n} records  RSSI {d['last_rssi']}")


def _period(args, cfg):
    tz = ZoneInfo(cfg["timezone"])
    last = datetime.now(tz).date()  # "today" at the plant, not on the Pi's system clock
    first = last - timedelta(days=args.days - 1)
    return tz, first, last


def cmd_report(store: Store, args, cfg):
    if not cfg["shifts"]:
        sys.exit("no [[shifts]] in the config file - see gateway.example.toml")
    tz, first, last = _period(args, cfg)
    windows = report.shift_windows(report.parse_shifts(cfg["shifts"]), first, last, tz)
    now_ms = int(time.time() * 1000)
    # Drop shifts that have not started; cut the running one off at "now".
    windows = [(n, a, min(b, now_ms)) for n, a, b in windows if a < now_ms]
    for d in store.devices():
        until = (d["last_sync"] or 0) * 1000
        ivs = report.intervals(store.events(d["address"]), until)
        print(f"\n{d['name']} ({d['address']})")
        print(f"  {'shift start':16} {'shift':5} {'in-cycle':>8} {'stop':>6} {'off':>6} {'no data':>7}  longest stop")
        for r in report.per_shift(ivs, windows):
            start = datetime.fromtimestamp(r.start / 1000, tz).strftime("%a %m-%d %H:%M")
            running = "  (in progress)" if r.end == now_ms else ""
            print(f"  {start:16} {r.shift:5} {r.pct('RUN'):7.1f}% {r.pct('STOP'):5.1f}% {r.pct('OFF'):5.1f}%"
                  f" {r.pct('NO DATA'):6.1f}%  {fmt_dur(r.longest_stop)}{running}")


def cmd_export(store: Store, args, cfg):
    tz, first, last = _period(args, cfg)
    lo = int(datetime.combine(first, datetime.min.time(), tz).timestamp() * 1000)
    out = open(args.csv, "w", newline="") if args.csv else sys.stdout
    w = csv.writer(out)
    w.writerow(["machine", "address", "state", "start_local", "end_local", "seconds"])
    for d in store.devices():
        for iv in report.intervals(store.events(d["address"]), (d["last_sync"] or 0) * 1000):
            if iv.end < lo:
                continue
            a = datetime.fromtimestamp(iv.start / 1000, tz).isoformat(timespec="seconds")
            b = datetime.fromtimestamp(iv.end / 1000, tz).isoformat(timespec="seconds")
            w.writerow([d["name"], d["address"], iv.state, a, b, round((iv.end - iv.start) / 1000, 1)])
    if args.csv:
        out.close()
        print(f"wrote {args.csv}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--config", type=Path, default=HERE / "gateway.toml")
    ap.add_argument("-v", "--verbose", action="store_true")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("run")
    sub.add_parser("once")
    sub.add_parser("devices")
    for name in ("report", "export"):
        sp = sub.add_parser(name)
        sp.add_argument("--days", type=int, default=7)
        if name == "export":
            sp.add_argument("--csv")
    args = ap.parse_args()

    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(levelname)s %(message)s")
    cfg = load_config(args.config)
    store = Store(os.path.expanduser(cfg["database"]))
    try:
        if args.cmd == "run":
            log.info("gateway started, database %s, every %d s", cfg["database"], cfg["interval_s"])
            asyncio.run(run_forever(store, cfg))
        elif args.cmd == "once":
            asyncio.run(sync_pass(store, cfg))
        else:
            {"devices": cmd_devices, "report": cmd_report, "export": cmd_export}[args.cmd](store, args, cfg)
    except KeyboardInterrupt:
        pass
    finally:
        store.close()


if __name__ == "__main__":
    main()
