# Raspberry Pi gateway

A Raspberry Pi collects the logs from every Cycle Start Watch in Bluetooth
range, every 5 minutes, into one SQLite database. Nobody has to walk the
floor with a phone, and nothing is lost when a device's log wraps.

```
 Machine 1 ─┐
 Machine 2 ─┼── BLE (10–30 m) ──▶  Raspberry Pi  ──▶  /var/lib/csw/csw.sqlite
 Machine 3 ─┘                      csw_gateway.py      (query it with SQL, export CSV)
```

The devices need no changes. The gateway uses the same steps as the phone:
set the clock, read from the last record it has, download the new pages.

## What it does every pass

1. Scans for 10 s for devices advertising the Cycle Start Watch service.
2. For each one, in turn:
   * reads Status
   * sets the device clock, but only if the Pi's own clock is trustworthy (see Clock)
   * downloads every record after the last one it stored, committing each
     page as it arrives, so a dropped link loses nothing
   * notes the sync in `sync_log`, including any records that were
     overwritten before it could collect them
3. Waits out the rest of the 5 minutes, then repeats.

A device is connected for a few seconds per pass. A phone can still connect
the rest of the time (the device accepts one connection at a time).

## Hardware

| Item | Notes |
|---|---|
| Raspberry Pi 4 or 5, 2 GB is plenty | Built-in Bluetooth works. A USB Bluetooth adapter with an external antenna improves range on a busy floor |
| Storage: **USB SSD** (recommended) or an industrial-grade microSD card | Cheap SD cards wear out and corrupt on power cuts |
| Official power supply, ideally on a small UPS or a UPS HAT | Avoids SD corruption when the area loses power |
| **RTC HAT with a battery (e.g. DS3231)**, if the Pi will have no network | Without network time the Pi's clock is wrong after every reboot (see Clock) |
| Metal-free enclosure mounted high, with line of sight to the machines | BLE does not pass through machine guarding well |

**Placement:** one Pi per area of about 6–10 machines. Before committing,
check the range on the floor: `python csw_gateway.py once -v` lists the RSSI
of every device it reaches. Better than −85 dBm is reliable.

## Setup

On Raspberry Pi OS Lite (64-bit, Bookworm or newer), as a user with sudo:

```bash
sudo apt update && sudo apt install -y python3-venv bluez sqlite3 git
sudo useradd --system --create-home --groups bluetooth csw
sudo mkdir -p /opt/csw /var/lib/csw /etc/csw
sudo chown csw: /opt/csw /var/lib/csw

sudo -u csw git clone https://github.com/rwaynewhite15/Cycle_Start_Watch.git /opt/csw/src
sudo ln -s /opt/csw/src/gateway /opt/csw/gateway
sudo -u csw python3 -m venv /opt/csw/venv
sudo -u csw /opt/csw/venv/bin/pip install -r /opt/csw/gateway/requirements.txt

sudo cp /opt/csw/gateway/gateway.example.toml /etc/csw/gateway.toml
sudo nano /etc/csw/gateway.toml          # time zone, shifts, device list
```

`/opt/csw/gateway` is a link into the cloned repository, so the git checkout
and the paths in the service file stay together.

### Pair each device once

Every device requires a passkey-authenticated link. The Pi must be paired
with each one once, using the passkey on the device's label. The pairing is
remembered across reboots.

```bash
sudo bluetoothctl
[bluetooth]# agent KeyboardOnly
[bluetooth]# default-agent
[bluetooth]# scan on                   # wait for e.g. "OKUMA-LB-07  AA:BB:CC:DD:EE:01"
[bluetooth]# scan off
[bluetooth]# pair AA:BB:CC:DD:EE:01
[agent] Enter passkey (number in 0-999999): 482913
Pairing successful
[bluetooth]# trust AA:BB:CC:DD:EE:01
[bluetooth]# exit
```

Then test one pass by hand:

```bash
sudo -u csw /opt/csw/venv/bin/python /opt/csw/gateway/csw_gateway.py --config /etc/csw/gateway.toml once -v
```

If a sync fails with an authentication error, the device is not paired.
This also happens after someone holds the device's BOOT button for 5 s,
which wipes its pairings. In that case run `remove AA:BB:…` in
`bluetoothctl`, then pair again.

### Run it as a service

```bash
sudo cp /opt/csw/gateway/csw-gateway.service /etc/systemd/system/
sudo systemctl enable --now csw-gateway
journalctl -u csw-gateway -f            # live log
```

`systemd` starts it at boot and restarts it if it crashes.

## Looking at the data

```bash
cd /opt/csw/gateway
python csw_gateway.py --config /etc/csw/gateway.toml devices
python csw_gateway.py --config /etc/csw/gateway.toml report --days 7
python csw_gateway.py --config /etc/csw/gateway.toml export --days 30 --csv /tmp/intervals.csv
```

Example report:

```
OKUMA-LB-07 (AA:BB:CC:DD:EE:01)
  shift start      shift in-cycle   stop    off no data  longest stop
  Wed 10-28 23:00  C        76.9%  23.1%   0.0%    0.0%  0:00:15
  Thu 10-29 06:00  A        76.9%  23.1%   0.0%    0.0%  0:00:15
  Thu 10-29 14:30  B        76.9%  23.0%   0.0%    0.1%  0:00:15  (in progress)
```

* **off:** the device had no power (the machine was off).
* **no data:** time the gateway has no records for. Either it has not synced
  yet, or the device was not installed then.

**Plain SQL.** `sqlite3 /var/lib/csw/csw.sqlite`, or open a copy of the file
in *DB Browser for SQLite* on your PC. Tables: `device`, `event` (raw
records), `sync_log`; view: `event_readable`.

```sql
-- Last 20 records, readable
SELECT * FROM event_readable ORDER BY utc_time DESC LIMIT 20;

-- Devices that have not synced in the last hour
SELECT name, datetime(last_sync, 'unixepoch') FROM device
WHERE last_sync < strftime('%s', 'now') - 3600;

-- Failed syncs today, with the reason
SELECT d.name, datetime(s.at, 'unixepoch'), s.message
FROM sync_log s JOIN device d USING (address)
WHERE s.ok = 0 AND s.at > strftime('%s', 'now', 'start of day');
```

## Clock

Timestamps on the devices are only as good as the clock that sets them.
A Raspberry Pi **has no battery-backed clock**. After a reboot without network
access it can be hours or days off, and pushing that to the devices would
corrupt every record.

So by default (`set_device_clock = "ntp"`) the gateway only sets device
clocks while the Pi reports `NTPSynchronized=yes`. Otherwise it leaves them
alone; their own DS3231 RTCs keep good time for months.

* **Pi on the plant network:** NTP works out of the box (point it at the
  plant's NTP server in `/etc/systemd/timesyncd.conf` if the internet is blocked).
* **Pi with no network:** fit an RTC HAT, set its time once, and set
  `set_device_clock = "always"`.

## Storage and backups

* The database is small: about 16 bytes per state change. A busy machine
  produces about 1 MB a year.
* WAL mode lets you read the database while the gateway writes.
* Back up by copying the file with SQLite's own command (safe while running):
  `sqlite3 /var/lib/csw/csw.sqlite ".backup /mnt/usb/csw-$(date +%F).sqlite"`

## Network and IT

The gateway **does not need a network to collect data**. With an RTC HAT
it works fully offline, and you fetch the database with a USB stick or over
SSH. Putting it on the plant network (for NTP, remote SQL access or a
dashboard) is an IT/OT decision. See section D of
[the sign-off checklist](../docs/plant-signoff-checklist.md).

## Tests

```bash
cd gateway && python -m unittest -v test_gateway
```

The tests use a fake device that behaves like the firmware, including
paging and the ring buffer wrapping. They cover incremental download,
detection of overwritten records, clock-trust rules, intervals across a
power cut, and shift windows across a daylight-saving change.
