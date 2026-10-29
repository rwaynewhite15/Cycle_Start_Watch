# Bench test plan: phase 1

**Goal:** prove the hardware, firmware and BLE interface on the bench, with
a switch standing in for the cycle start lamp, before any machine install
and before writing the app.

**Time needed:** about half a day, plus an overnight soak test.

## Equipment

| Item | Notes |
|---|---|
| ESP32-C3-DevKitM-1 (or S3-DevKitC-1) + USB cable | |
| Input circuit on a breadboard or perfboard | R1, R2, R4, PC814, R3, C1 from [hardware/README.md](../hardware/README.md) |
| DS3231 breakout + coin cell | |
| 24 VDC bench supply or DIN-rail supply, with current limit set to about 0.5 A | |
| Toggle switch + a 24 V pilot lamp (an LED type if you have one) | Stands in for the PMC output + cycle start lamp |
| Optional: 24 VAC transformer | For test B5 |
| Multimeter | |
| Laptop with PlatformIO, and Python 3 with `pip install bleak` | |
| Phone with **nRF Connect** (free, Nordic Semiconductor; iOS and Android) | |

Bench wiring: `24 V+ → switch → lamp → 0 V`, with the device input (J1)
connected **across the lamp**, as it will be on the machine.

Record each result in the **Result** column of the copy you work from.

---

## A. Build and flash

| # | Step | Expected | Result |
|---|---|---|---|
| A1 | `cd firmware && pio test -e native` | All filter tests pass | |
| A2 | `pio run -e esp32c3 -t upload`, then `pio device monitor` | Banner shows fw version, machine name `CSW-xxxx` and a **6-digit passkey**. Write the passkey down | |
| A3 | Serial log at boot | `[clock] RTC time …` (or "lost power" on a new RTC), `[log] 0 records…`, `[ble] advertising as "CSW-xxxx"` | |
| A4 | RGB LED | Blue at boot, then red about 1.5 s later (switch off → STOP) | |

## B. Input circuit and filter

Use `python tools/csw_client.py watch --device CSW-xxxx`, or just the serial log.

| # | Step | Expected | Result |
|---|---|---|---|
| B1 | Switch ON (lamp lit). Measure the current into J1 | About 7 mA at 24 V. LED goes green about 1.5 s later. Serial: `type=1 value=1` | |
| B2 | Switch OFF | Red after about 1.5 s; `value=0`. The record's timestamp is when you flipped the switch, not 1.5 s later | |
| B3 | Flick the switch on and off quickly (< 0.2 s) | No record | |
| B4 | Flash the switch about once a second for 10 s, then leave it OFF | One BLINK record (amber), then one STOP. **No** RUN/STOP flood | |
| B5 | *(If you have 24 VAC.)* Replace the DC supply with AC; switch ON for 30 s | Steady RUN, with no chatter in the log | |
| B6 | Lower the supply to 12 V; switch ON | Still detects RUN (minimum design voltage) | |
| B7 | Swap the J1 wires | Works the same (the opto is bidirectional) | |
| B8 | Leakage check: switch OFF, then put a 100 kΩ resistor across the switch (about 0.24 mA of "leakage") | Stays STOP | |
| B9 | Simulate a cycle: ON 20 s, OFF 3 s, repeated 5 times | 10 records, each RUN ≈ 20 s, each STOP ≈ 3 s | |

## C. BLE with nRF Connect (phone)

| # | Step | Expected | Result |
|---|---|---|---|
| C1 | Scan | `CSW-xxxx` listed, with service `6C1A0000-…` | |
| C2 | Connect, open the service, read **Status** (…0001) | Phone shows the pairing prompt. Enter the passkey. The read returns 20 bytes | |
| C3 | Decode the Status bytes by hand using [ble-protocol.md](ble-protocol.md) | byte 0 = 01, byte 1 = current state | |
| C4 | Pair with a **wrong** passkey (first remove the bond in phone Bluetooth settings) | Pairing fails; serial shows "pairing failed"; no data is readable | |
| C5 | Enable notifications on Status; flip the switch | A notification arrives about 1.5 s after each flip | |
| C6 | Read **Config** (…0005), shown as UTF-8 | JSON with name, invert, debounce_ms, settle_ms, fw | |
| C7 | Write Config (as text) `{"name":"BENCH-01"}`; disconnect and scan again | Advertises as `BENCH-01` | |
| C8 | Write Config `{"settle_ms":5}` | Read back shows `"error":"settle_ms must be 200-30000"`; nothing changed | |
| C9 | Write **Log cursor** (…0003) = `01000000` (uint32 1); read **Log page** (…0004) | Up to 512 bytes = 32 records of 16 bytes; the first starts `01 00 00 00` | |
| C10 | Connect a second phone (or nRF Connect on a second device) while the first is connected | Second connection is dropped | |

## D. Laptop client (end-to-end download)

| # | Step | Expected | Result |
|---|---|---|---|
| D1 | `python tools/csw_client.py scan` | Device listed with RSSI | |
| D2 | `… synctime --device BENCH-01` | Reports the offset; afterwards the device is within ±50 ms. Serial: `[clock] set from phone`, or no change if already within 2 s | |
| D3 | `… download --device BENCH-01 --csv bench.csv` | Every record so far; BOOT first, then STATE, TIME_SET, … ; seq has no gaps | |
| D4 | `… download --from <next>` after a few more flips | Only the new records | |

## E. Power and time

| # | Step | Expected | Result |
|---|---|---|---|
| E1 | Switch ON (RUN). Wait 2 min. Unplug the device for 5 min, then plug it back in | Log: RUN, then **POWER_DOWN** (time ≤ 60 s before the unplug), **BOOT** (reason 1 = power-on), then the state | |
| E2 | Pull power *while* flipping the switch, 10 times | The log is still readable after every reboot; seq continues; no garbage records | |
| E3 | Remove the RTC coin cell, power cycle, reconnect the cell | Status flags show time **not** valid; records carry the "time not set" flag; after `synctime` a TIME_SET record appears and flags clear | |
| E4 | Disconnect the DS3231 entirely and boot | Runs; status shows "NO RTC"; time valid only after a phone sync | |
| E5 | Clock drift: sync, leave for 24 h+, then read the Time characteristic | Within ±1 s of the phone | |

## F. Robustness

| # | Step | Expected | Result |
|---|---|---|---|
| F1 | Hold BOOT for 5 s | Serial: "all pairings deleted"; the phone must pair again | |
| F2 | Overnight soak: drive the switch from a timer relay or a second ESP32 (e.g. 40 s on / 20 s off) | Next morning: record count ≈ hours × 120; no BOOT records other than the first (no watchdog resets); in-cycle ≈ 66.7% | |
| F3 | Walk away with the phone until the link drops, then come back | Reconnects; download resumes from the cursor | |
| F4 | Range test: device inside a closed steel box (a stand-in for the cabinet), then outside it | Note the RSSI and the distance at which the connection drops, for each case | |
| F5 | Fill test (optional, slow): set `settle_ms` 200 and drive 1 Hz for about 10 h to pass 32,768 records | first_seq advances; a download from 1 starts at first_seq; nothing breaks at the wrap | |

## Pass criteria before going near a machine

* All of A–E pass. F1–F3 pass.
* F2 soak: in-cycle % from the downloaded log is within 0.5% of the duty
  cycle of the timer.
* You know the BLE range through a steel enclosure (F4). This decides where
  the device gets mounted.
