# Design notes: the why behind each choice

These notes are for someone strong in manufacturing, Python and SQL but newer
to embedded and mobile work. Each section states the decision, why it was
made, and what would change it.

---

## 1. What we sense: the cycle start lamp

**Decision:** sense the lamp (the indicator inside the cycle start
push button), not the switch contacts.

**Why:** the button contacts close for about half a second when the operator
presses the button. They tell you a start was *requested*, not that the
machine is running. On Fanuc the lamp follows STL (F0.5), the "cycle start
lamp" signal, which the CNC holds on while a program is executing in
MEM/MDI/DNC mode. It drops on:

| Event | STL / lamp |
|---|---|
| Program running, including waiting on an M-code FIN or a bar feeder | ON |
| Feed hold | OFF (SPL, F0.4, comes on instead) |
| Alarm, reset, emergency stop | OFF |
| M00 / M01 optional stop, M30 / M02 end | OFF |
| Single block, between blocks | OFF |

So lamp ON ≈ "the control is in cycle". That is the number you want.

**Caveats. Check these per machine model in the survey (install guide, step 1):**

* **The lamp signal comes from the machine builder's ladder, not from
  Fanuc.** Most builders wire the lamp straight from STL, but some flash it
  during feed hold or while waiting for the door, and some leave it on in
  other modes. The firmware detects flashing and reports BLINK, but you need
  to know what each state means on each model.
* **Some panels have no lamp terminals to tap.** The Fanuc standard machine
  operator panel (the membrane keypad type) drives its LEDs from a circuit
  board over I/O Link, so there are no separate lamp wires. Mori Seiki and
  Okuma panels with discrete 22 mm push buttons usually do have terminals.
  If there is nothing to tap, use a fallback.
* **"In cycle" includes MDI, warm-up programs, dry runs and prove-outs.** In
  phase 1 these look the same as production. Phase 2 (program number)
  separates them.

**Fallbacks, in order of preference:**

1. **Green stack light.** Usually wired to its own terminals and easy to
   reach. Check what green means on that machine: "auto running" or "no
   alarm" are not the same thing.
2. **An existing PMC output that already mirrors STL** (for example one
   driving a spare relay). Tapping an existing output is wiring only.
   *Programming a new output is a ladder change and is out of scope for
   phase 1.* The original plan listed "spare PMC output" as a fallback
   without that condition.

## 2. The input circuit

See [hardware/README.md](https://github.com/rwaynewhite15/Cycle_Start_Watch/blob/main/hardware/README.md) for the schematic and the
calculations. The main choices:

* **Bidirectional-LED optocoupler (PC814 or equivalent).** It works with AC
  or DC of either polarity, so the electrician cannot wire it backwards, and
  it does not matter whether the PMC output sources or sinks current. The
  machine side and the ESP32 side are joined only by light, so there is no
  electrical path from the device back into the machine.
* **Connected across the lamp terminals, in parallel.** The existing
  wiring is not cut or rerouted. Our circuit draws about 7 mA at 24 V. A
  Fanuc DO is typically rated around 200 mA per point, so the extra load is
  small.
* **Single-fault safe.** The current limit is two resistors in series. If
  either one fails shorted, the current is still under 20 mA. A fused
  terminal at the tap point protects the cable run.
  **No part of the device can make the lamp circuit carry more than a few
  mA, and nothing in it can drive a signal into the machine.**
* **Isolated DC-DC converter for power.** The ESP32's ground stays separate
  from the machine's 0 V. A laptop plugged into the device's USB port during
  commissioning cannot then create a ground loop through the control.

## 3. The firmware filter: why "debounce 200 ms" became three stages

A plain 200 ms debounce assumes the signal is a clean DC level with some
bounce. Real lamps are not always like that:

| Real-world signal | What a plain debounce does | Stage that handles it |
|---|---|---|
| 24 VAC lamp: the opto turns off 100–120 times a second at each zero crossing | Sees no steady level, so it never settles or chatters | **1. Pulse stretch**: any "on" in the last 25 ms counts as on |
| LED dimmed with PWM, or a scanned panel matrix | Same as above | 1 (plus the RC filter on the board) |
| Noise from spindle drives or a lamp-test flash | Fine | **2. Debounce**, 200 ms |
| Builder flashes the lamp at 1 Hz in feed hold | Logs RUN/STOP every 0.5 s, which floods the log and makes in-cycle % wrong | **3. Settle + blink detection** |

Stage 3 only reports a change after the level has held for 1.5 s. If instead
it flips 4 or more times within 4 s, it reports BLINK. Reports are
*back-dated* to when the change really began, so the delay does not reduce
timestamp accuracy. The cost: a stop shorter than 1.5 s is not logged. That
has no effect on in-cycle %, and real load/unload gaps are longer than that.
`settle_ms` can be changed over BLE if a machine needs it.

These stages are pure logic in `firmware/src/lamp_filter.cpp`, unit-tested
on the PC (`pio test -e native`). The tests cover AC input, blinking, short
stops, invert, and the 49-day millisecond-counter wrap. Writing those tests
caught a real bug: after a flashing lamp went steady, the filter flipped
between BLINK and STOP on every sample.

## 4. The event log

* **Log state changes only**, as 16-byte records: `{seq, ts, ms, type, flags,
  value}`. A machine with 60-second cycles produces about 2,900 records a
  day. 32,768 records (512 KB) is about 11 days at that rate, and months on
  a machine with long cycles.
* **`seq` is a counter that never resets or repeats.** The app remembers the last
  seq it stored and asks for "everything after N". If the device's clock is
  ever wrong, sync still works, because it never depends on timestamps.
* **LittleFS, not raw flash or SPIFFS.** LittleFS is built to survive power
  loss at any instant: a record is either fully written or not written at
  all. It also spreads writes across the flash so no block wears out early.
  Machines get powered off mid-cycle, so this matters.
* **No separate "head pointer" file.** At boot the firmware scans for the
  highest seq (about 0.5 s). A separate pointer file could get out of step
  with the data after a badly timed power cut.
* **Extra record types explain gaps:** `BOOT` (with the ESP32 reset reason,
  so a watchdog reset is visible), `POWER_DOWN` (the last 60-second
  heartbeat before power was lost), `TIME_SET` (old and new clock) and
  `CONFIG` (invert changed).

### Change: power-off time is not simply "not downtime"

The original plan used a boot marker "so power-off gaps aren't counted as
downtime". The device records the gap explicitly (`POWER_DOWN` … `BOOT`)
and the app shows it as **OFF**. Whether OFF counts against in-cycle % is a
report setting, not something hidden in the data. A machine powered off
during a scheduled shift *is* lost capacity, and that is often the biggest
loss on the list.

## 5. Time

* **DS3231 RTC with a coin cell** keeps time while the machine is off, to
  within about a minute a year. Use a breakout *without* a charging circuit;
  see the hardware notes.
* **Everything is stored in UTC.** The app converts to local time. If the
  device stored local time, every daylight-saving change would make one
  shift an hour long or an hour short.
* **The phone sets the clock on every sync** if it is more than 2 s off. A
  `TIME_SET` record keeps the old time. Records made while the clock was not
  set are flagged, so the app can correct them.
* The ESP32 counts milliseconds from a DS3231 second boundary and re-checks
  every 6 hours. That gives millisecond timestamps without reading the RTC
  for every event.

## 6. Bluetooth

* **NimBLE-Arduino rather than the default Bluedroid stack.** It uses about
  half the RAM and flash, and its API is simpler.
* **Binary characteristics, plus JSON for config.** Status and log records
  are fixed binary layouts: compact, fast, and easy to parse in Dart and
  Python. Config is JSON because a person types it in nRF Connect, and it
  changes rarely.
* **Paged log download (write cursor, read page) instead of notification
  streaming.** Each read is repeatable, so a dropped packet is simply
  re-read. There is no flow control to get wrong, and it can be tested by
  hand in nRF Connect. It is fast enough: incremental syncs are small, and a
  full 512 KB download takes about 1–2 minutes.
* **The passkey is fixed per unit, which limits its security.** The ESP32
  has no display, so it cannot show a fresh random code each time. A fixed
  passkey is a door lock, not a vault. A determined attacker within radio
  range could recover it. That is acceptable here, because the worst they
  can do is read run/stop history or rename the device; they cannot affect
  the machine. Say this plainly to plant IT (see the sign-off checklist). All
  characteristics require an encrypted, authenticated link, and bonds can be
  wiped by holding the BOOT button for 5 s.

## 7. Choice of ESP32 chip

* **ESP32-C3:** cheapest, has BLE 5, and is plenty for phase 1.
* **ESP32-S3:** more RAM and a second core. Choose it if you expect to add
  Wi-Fi gateway duties later.
* Both build from the same code (`pio run -e esp32c3` / `-e esp32s3`).
* For the production unit, use a **pre-certified module** (ESP32-C3-MINI-1,
  ESP32-S3-WROOM-1, or the `-1U` versions with an external antenna
  connector), not a bare chip. The modules carry FCC/IC/CE radio approval,
  which plant EHS or IT may ask for.

## 8. Raspberry Pi gateway (chosen as the main data path)

Phone-only sync has two problems: data arrives only when someone walks the
floor, and a busy machine's log wraps after about 11 days. A Raspberry Pi
per area syncs every device in range every 5 minutes into SQLite. See
[gateway/README.md](https://github.com/rwaynewhite15/Cycle_Start_Watch/blob/main/gateway/README.md).

* **Python + `bleak`**, the same library as the bench client, so the sync
  logic is tested once and reused. SQLite because the data is small (about
  1 MB per machine per year) and you can query it directly.
* **One connection at a time, a few seconds each.** The device only accepts
  one client, so a phone still gets in between passes.
* **No plant network, so the Pi has an RTC HAT.** A bare Pi has no battery
  clock. After a reboot it could push a wrong time to every device. With a
  DS3231 HAT, set once from a laptop, the Pi is the plant's time source for
  its devices. (On a networked site, `set_device_clock = "ntp"` would use NTP
  instead.)
* **Fully offline.** Data leaves the Pi on a USB stick or over a laptop
  plugged straight into it with an Ethernet cable.
* **Pairing:** each device is paired with the Pi once, with `bluetoothctl`
  and the passkey from its label.

## 9. App (planned)

* **Flutter**: one codebase for iOS and Android. BLE through
  `flutter_blue_plus` (widely used, handles iOS and Android pairing
  differences). Storage through `sqflite` / `drift`.
* **The phone holds the raw events.** The app derives intervals and reports
  from them, so a change to the shift calendar recomputes history.
* The schema and an example in-cycle-by-shift query are in
  [app/README.md](https://github.com/rwaynewhite15/Cycle_Start_Watch/blob/main/app/README.md).

## 10. Open risks

| Risk | Mitigation |
|---|---|
| A lamp behaves differently from STL on some model | Survey every model; BLINK state; `invert` option; stack light fallback |
| No lamp terminals on the panel | Stack light, or an existing PMC output |
| Steel cabinet blocks BLE | Device mounted outside the cabinet; external-antenna module variant |
| Nobody syncs, so the ring buffer wraps and data is lost | Raspberry Pi gateway syncs every 5 minutes; `sync_log` flags any loss |
| Gateway out of BLE range of some machines | Range check with `csw_gateway.py once -v` before placing; USB adapter with external antenna |
| Plant wireless policy forbids BLE devices | Raise it early (sign-off checklist) |
| Machine warranty or builder concerns about added wiring | Parallel tap only, documented, reversible; ask the builder if under warranty |

## 11. Phase 2: why the program, N number, tool and alarm need a data link

The lamp is one bit. Program number, N number, tool and alarm number live
inside the CNC, so they need a data connection, not more wiring.

* **Fanuc and Mori Seiki (Fanuc-based controls):** **FOCAS2** over the
  control's Ethernet port. On 0i-D/F and 30i-series embedded Ethernet,
  FOCAS is usually available; older controls (0i-C, 18i) may need an option
  or an Ethernet board. FOCAS is a closed library for Windows and Linux
  (x86 and ARM). It does not run on an ESP32, hence a small Linux board such
  as a Raspberry Pi 4/5 or an industrial equivalent.
* **Okuma (OSP-P):** **THINC API** runs on the control's own Windows PC and
  needs a license. Okuma's **MTConnect** adapter is often simpler: it serves
  XML over HTTP to any client.
* **Mori Seiki / DMG MORI with MAPPS or CELOS** may already offer MTConnect.
  Check before paying for FOCAS work.
* **Network policy.** Putting a device on the machine's network is an OT
  security conversation. Start it during phase 1.
* **The gateway Pi is the natural host for FOCAS/MTConnect.** Phase 2 adds
  tables next to `event` in the same database. Once a Pi reads the run state
  directly from a control, the lamp device on that machine becomes the
  backup, as planned.
