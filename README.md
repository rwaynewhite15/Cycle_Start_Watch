# Cycle Start Watch

A small Bluetooth device that watches a CNC machine's **cycle start lamp** and
logs when the machine is in cycle. A **Raspberry Pi gateway** collects every
device's log into a SQLite database automatically, and reports in-cycle time
by shift, the longest stops, and a CSV export. A phone app can also connect
to a device directly, for setup and spot checks.

Target machines: Fanuc, Okuma and Mori Seiki CNCs in a gear and transmission
machining plant.

> **Status:** phase 1. The firmware builds for the ESP32-C3 and -S3 in CI,
> and its input filter has unit tests that pass. The Raspberry Pi gateway is
> written and tested against a simulated device. Next: the bench test on real
> hardware. The phone app is not started yet.

## Hard requirements

1. **Simple to install.** An electrician makes 4 connections. A controls engineer
   may be needed to identify the lamp terminals. **No ladder/PMC changes** in phase 1.
2. **Read-only.** The device can never start the machine or block a start.
   This is guaranteed by the hardware, not by software: the only connection to
   the machine's signal circuit is the input side of an optocoupler, through a
   current-limiting resistor. No relay or output is wired toward the machine.
3. **Bluetooth Low Energy** to a phone app on iOS and Android.

## How it works

```
 Machine cabinet                         Device (outside the cabinet)        Phone
 ───────────────                         ────────────────────────────        ─────
 Cycle start lamp ──(fused tap)──▶ PC814 optocoupler ─▶ ESP32 ─── BLE ───▶ Raspberry Pi gateway
 24 VDC control supply ─(fused tap)──▶ isolated DC-DC ─┘   │          │       SQLite, shift reports, CSV
                                                   DS3231 RTC     └─▶ Phone app (setup, spot checks)
                                                   + coin cell
```

* **What is sensed:** the cycle start button's *lamp*, not its contacts. On
  Fanuc the lamp follows STL (F0.5): it is on while the control is executing a
  program, and goes out on feed hold, alarm, reset, M00/M01 and M30. That
  gives *time in cycle*. The button contacts would only give start presses.
  Fallbacks: the green stack light, or an existing PMC output that already
  mirrors STL.
* **Firmware** (ESP32-C3 or -S3, Arduino core + NimBLE-Arduino, PlatformIO):
  * samples the input every 2 ms
  * filters it in three stages: pulse stretch (handles AC lamps and PWM-dimmed
    LEDs), a 200 ms debounce, and a 1.5 s settle stage with blink detection
  * logs only state changes, as 16-byte records with a UTC timestamp, in a
    power-loss-safe ring buffer in flash (LittleFS, 32,768 records)
  * logs boot, power-down, clock-set and config-change records, so gaps can be
    explained
  * serves everything over a passkey-protected BLE GATT service
* **App** (Flutter, planned): syncs new records into SQLite and shows a
  run/stop timeline, in-cycle % by shift, the longest stops, and CSV export.

## Repository layout

| Path | What |
|---|---|
| [`firmware/`](firmware/) | PlatformIO project. `pio run -e esp32c3` builds it, `pio test -e native` runs the PC unit tests |
| [`hardware/`](hardware/README.md) | Schematic, parts list and the component-value calculations |
| [`gateway/`](gateway/README.md) | Raspberry Pi gateway: BLE → SQLite collector, shift reports, CSV export |
| [`app/`](app/README.md) | Flutter app (plan and database schema so far) |
| [`tools/`](tools/) | Python bench client (`csw_client.py`, uses `bleak`) and protocol decoder |
| [`docs/design.md`](docs/design.md) | Why each decision was made, open risks, phase 2 |
| [`docs/ble-protocol.md`](docs/ble-protocol.md) | Byte-level BLE interface, for the app |
| [`docs/bench-test-plan.md`](docs/bench-test-plan.md) | Bench test with a dev board, 24 V supply and a switch |
| [`docs/install-guide.md`](docs/install-guide.md) | Install guide for the electrician |
| [`docs/plant-signoff-checklist.md`](docs/plant-signoff-checklist.md) | Approvals to collect before the first machine install |

## Quick start (firmware)

```bash
pip install platformio
cd firmware
pio test -e native             # filter logic tests on your PC
pio run -e esp32c3 -t upload   # build and flash a dev kit
pio device monitor             # shows the BLE passkey at boot
```

Then follow [the bench test plan](docs/bench-test-plan.md).

## Changes to the original design (summary)

The full reasoning is in [docs/design.md](docs/design.md).

* **Survey every machine before installing.** On some panels the cycle start
  lamp has no terminals to tap. The Fanuc standard operator panel, for
  example, drives its LEDs from a circuit board. Some machine builders also
  flash the lamp. A 10-minute survey per machine model settles both.
* **Debounce alone was not enough.** AC lamps, PWM-dimmed LEDs and flashing
  lamps would all have produced bogus RUN/STOP records. The three-stage
  filter handles them.
* **The "spare PMC output" fallback is a ladder change.** It is only allowed
  in phase 1 if an existing output already mirrors STL.
* **Power-off time is logged as its own state (OFF), not dropped.** If a
  machine is off during a scheduled shift, that is lost time. The app decides
  whether OFF counts as down, so the data never hides it.
* **Call the metric "in-cycle %", not "uptime" or "availability".** It is not
  OEE availability: it does not know planned downtime reasons or part counts.
  The new name avoids an argument with the plant's OEE numbers.
* **A Raspberry Pi gateway collects the data, instead of relying on someone
  walking past with a phone.** Each device's log holds only about 11 days
  at a busy machine. The gateway syncs every device in range every 5 minutes.
  The same Pi is the natural home for phase 2 (FOCAS/MTConnect).
