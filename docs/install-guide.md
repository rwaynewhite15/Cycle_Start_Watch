# Install guide: Cycle Start Watch (phase 1)

**For:** a qualified industrial electrician, with a controls engineer
available for step 1.
**Time:** about 1 hour per machine once the survey is done.
**What it does:** watches the cycle start lamp and logs when the machine is
in cycle. **Read-only:** it cannot start the machine or block a start. It
connects in parallel with the lamp through an optocoupler and has no outputs
toward the machine.

> **No ladder/PMC changes, no parameter changes.** If the only way to get
> the signal is to change the ladder, stop. That is out of scope for phase 1.

---

## Before you start

* [ ] The plant sign-off checklist is complete for this machine (or model):
  [plant-signoff-checklist.md](plant-signoff-checklist.md).
* [ ] The unit has been bench tested, and its label shows its **name** and **passkey**.
* [ ] The machine is at a safe stopping point, and production is told.
* [ ] The phone with the Cycle Start Watch app (or nRF Connect) is charged.

### Parts

* Device in its enclosure, with the pluggable terminal blocks J1 (lamp) and J2 (24 V)
* 2 × fused terminal blocks for DIN rail, with fuses: **F1 63 mA fast** (lamp
  tap) and **F2 0.5 A time-lag** (24 V tap)
* 4-core 0.5 mm² (20 AWG) shielded control cable, long enough to reach the mounting spot
* M16 cable gland (or a cord grip that suits the knockout), ferrules, wire
  markers, cable ties
* Mounting: screws or a magnet/VHB plate for the enclosure

---

## Step 1: Survey (controls engineer + electrician, once per machine model)

Do this with the machine **powered and running**, so you can watch the lamp. It
needs no tools inside the cabinet.

1. **Watch the cycle start lamp** while the operator or setup person runs
   through each state below, and record what the lamp does:

   | State | Lamp on / off / flashing? |
   |---|---|
   | Program running (MEM/AUTO) | |
   | Feed hold | |
   | Single block, stopped between blocks | |
   | M00 / M01 stop | |
   | Alarm | |
   | Program end (M30), waiting for load/unload | |
   | MDI running | |
   | Door open / interlock waiting | |

   You want: **ON only while running**. Flashing in some states is fine: it is
   logged as BLINK. Note which states flash.
2. **Find where the lamp is wired.** From the electrical drawings, find the
   PMC output (Y address) and the terminals for the cycle start lamp. Look
   for a terminal strip in the cabinet or on the back of the operator panel.
   The controls engineer can confirm the Y address in the ladder viewer
   **without changing anything**.
   * If the lamp is part of a membrane keypad or circuit board (for example
     the Fanuc standard operator panel) with no lamp wires: use the **green
     stack light** instead, and repeat the table above for it.
3. **Measure the lamp voltage** across the two lamp terminals: lamp on (the
   machine in cycle), then lamp off.
   * Must be **12–30 V AC or DC** when on, and **under 2 V** when off.
   * Note whether it is AC or DC, and the polarity (for the record only; the
     device does not care).
4. **Find a 24 VDC tap** for power: a terminal on the protected 24 V control
   supply that is on whenever the control is on. **Not** a circuit switched
   by the E-stop or a door interlock, and not the I/O supply of a safety
   relay.
5. **Choose the mounting spot** *outside* the cabinet: a side panel near the
   operator, out of coolant and chip spray, reachable by a phone within
   about 5 m. Choose the knockout or gland location.
6. Record everything on the **install record** (end of this guide).

## Step 2: Lock out

1. Notify the operator and supervisor.
2. Power down the machine. **Lock out and tag out** the main disconnect
   following plant procedure. The cabinet contains 480 VAC.
3. Verify zero energy (try-start, then test with a meter that you have
   checked against a known live source).
4. Wait for the servo amplifier DC bus to discharge (the charge LEDs go out,
   typically within 5 minutes).

## Step 3: Wiring in the cabinet

```
 Cabinet DIN rail                                   Device (J1, J2)
 ───────────────                                    ───────────────
 Lamp terminal A ──▶ F1 (63 mA) ── core 1 (lamp A) ──▶ J1.1
 Lamp terminal B ───────────────── core 2 (lamp B) ──▶ J1.2
 +24 VDC tap     ──▶ F2 (0.5 A) ── core 3 (+24 V)  ──▶ J2.1
 0 V             ───────────────── core 4 (0 V)    ──▶ J2.2
 Shield: bond to the cabinet PE/ground bar at the cabinet end ONLY.
```

1. Mount F1 and F2 on the DIN rail near the lamp and 24 V terminals.
2. **Lamp tap.** Land a new conductor on each lamp terminal, *alongside*
   the existing wires: use a twin ferrule or the terminal's second clamp
   point. **Do not remove or reroute any existing wire.** Lamp A goes
   through F1.
   * It does not matter which lamp terminal is "A". The input works with
     either polarity, and with AC.
3. **24 V tap.** +24 V goes through F2. 0 V goes directly.
4. Route the cable away from motor/spindle power cables and VFD outputs. Cross
   them at 90° if you must cross. Keep it in the control-wiring duct.
5. Bring the cable out through the gland. Tighten the gland so the cabinet keeps
   its rating (NEMA 12 / IP54).
6. Label both ends: `CSW LAMP A/B`, `CSW +24V/0V`. Follow the plant's
   colour code for 24 VDC control wiring (NFPA 79: blue).
7. **Mark up the machine's electrical drawings** with the added terminals and
   fuses (red-line), and leave a copy in the cabinet.

## Step 4: Connect the device

1. Mount the enclosure.
2. Plug in J1 and J2. Use coded (keyed) plugs so they cannot be swapped,
   and check anyway that the 24 V cable is in J2.
3. Remove fuses F1 and F2 from their holders for now.

## Step 5: Power up and test

1. Remove your lockout following plant procedure. Power up the machine.
2. **Measure** at the open F2 holder: about 24 V. Insert **F2**. The device LED goes
   blue, then red (machine not in cycle).
3. **Measure** across J1 with the lamp off: under 2 V. Insert **F1**.
4. Connect with the phone. Pair with the passkey on the label.
5. **Set the machine name** and the clock (the app does both when you
   connect; with nRF Connect, write Config `{"name":"<asset number>"}`).
6. **Functional test.** The operator runs the machine, and you watch the device LED and app:

   | Test | Expected | OK |
   |---|---|---|
   | Cycle start | Green about 1.5 s after the lamp lights | |
   | Feed hold | Red (or amber, if this model flashes) | |
   | Resume, run to M30 | Green, then red at program end | |
   | Lamp off, machine idle for 1 min | Stays red, **no** flicker to green | |
   | **Cycle start still works normally, every time** | Machine starts | |
   | Any alarm on the control | **None** | |

   If the LED is green when the lamp is **off**: check that J1 is really on
   the lamp terminals, and measure the voltage across them with the lamp
   off. Above about 2 V means output leakage; see the hardware notes (R4).
   Use **invert** in the app only when the signal is genuinely reversed on
   this machine, for example a stack light that is lit when *stopped*. Note
   it on the install record.
7. Download the log in the app. You should see BOOT, the states from the
   test, and no gaps.

## Step 6: Hand-over

* [ ] Install record filled in and filed.
* [ ] Electrical drawings red-lined and copy left in the cabinet.
* [ ] Device label shows the name, passkey, install date and contact person.
* [ ] Operator/supervisor told what the device is ("logs in-cycle time,
  does not control anything") and who to call.

## Removal (fully reversible)

Lock out, take out F1 and F2, unplug J1 and J2, remove the added conductors
from the lamp and 24 V terminals and the fused terminals, and plug the gland
hole. The machine is then exactly as it was.

---

## Install record

| Field | Value |
|---|---|
| Machine / asset no. | |
| Make / model / control (e.g. Okuma LB3000 / OSP-P300) | |
| Signal used (cycle start lamp / stack light green / other) | |
| PMC address (if known) | |
| Lamp terminals (drawing page / terminal numbers) | |
| Lamp voltage ON / OFF, AC or DC | |
| 24 V tap terminal | |
| Lamp behaviour table (step 1) attached? | |
| Device name / passkey | |
| Invert setting | |
| Installed by / date | |
| Functional test passed (signature) | |
