# Hardware: phase 1

## Schematic

```
 MACHINE SIDE (12-30 V AC/DC)        │ ISOLATION │        DEVICE SIDE (3.3 V)
                                     │  barrier  │
 Lamp terminal A ─[F1 63 mA]─ J1.1 ─[R1 1.6k 1W]─[R2 1.6k 1W]─┬──────┐          3.3V
   (fused terminal at the tap)                                │  ┌───┴─────┐     │
                                                       [R4 4.7k] │ PC814   │   [R3 22k]
                                                              │  │ ▶|  |◀  │C ───┼──── GPIO4 (CSW_PIN_LAMP)
                                                              │  │ (bidir) │E ─┐ │
 Lamp terminal B ─────────────────── J1.2 ────────────────────┴──┴─────────┘   │[C1 0.47µF]
                                                                               │  │
                                                                              GND─┴── GND

 Cabinet +24 VDC ─[F2 0.5 A T]─ J2.1 ─[D1 SS14]─┬─[U1 isolated DC-DC 24→5 V, 2 W]── 5V ─▶ dev board 5V pin
 Cabinet 0 V ─────────────────── J2.2 ──────────┴─[TVS1 SMBJ33A]                   GND ─▶ dev board GND

 DS3231 breakout:  VCC─3.3V  GND─GND  SDA─GPIO5  SCL─GPIO6   (ESP32-C3 pins; S3: SDA 8, SCL 9)
```

On the ESP32-S3 the input is also GPIO4. The pins are set in
`firmware/platformio.ini`.

## Parts list (one unit)

| Ref | Part | Example | Why |
|---|---|---|---|
| — | ESP32 dev board | Espressif ESP32-C3-DevKitM-1 (or ESP32-S3-DevKitC-1) | Pre-certified radio module; USB for flashing |
| U2 | Optocoupler, AC input | Sharp PC814 / Vishay SFH620A / Everlight EL814 | Bidirectional LED: AC or DC, either polarity |
| R1, R2 | 1.6 kΩ, 1 W, metal film | any | Two in series: if one shorts, the current is still limited (below) |
| R3 | 22 kΩ, ¼ W | any | Pull-up; the opto pulls GPIO low when the lamp is on |
| R4 | 4.7 kΩ, ¼ W | any | Shunt across the opto LED: absorbs leakage from an output that is "off" |
| C1 | 0.47 µF ceramic | any | Smooths AC and PWM ripple (about 7 ms time constant) |
| U1 | Isolated DC-DC, 18–36 V in, 5 V out, ≥2 W | TRACO TMR 2-2411, RECOM REM2-2405S or similar | Keeps device ground separate from machine 0 V |
| D1 | Schottky diode | SS14 | Reverse-polarity protection |
| TVS1 | 33 V unidirectional TVS | SMBJ33A | Clamps 24 V supply spikes; behind our own fuse |
| — | DS3231 RTC breakout **with no charging circuit** | Adafruit 3013 (CR1220) | See the coin cell warning below |
| J1, J2 | 2-pole pluggable terminal blocks, 5.08 mm | Phoenix MSTB | The unit can be unplugged without touching the machine side |
| F1 | Fused terminal block, 5×20 mm fast fuse 63 mA | Phoenix UT 4-HESI or similar | In the cabinet **at the lamp tap**; protects the cable run |
| F2 | Fused terminal block, 5×20 mm 0.5 A time-lag fuse | same | In the cabinet at the 24 V tap |
| — | Enclosure, polycarbonate, IP65, about 100×70×40 mm | Hammond 1554 series | Plastic so BLE gets out; mounted outside the steel cabinet |
| — | Cable gland M16 + 4-core cable, 0.5 mm² (20 AWG), shielded | | Gland keeps the cabinet's NEMA 12 / IP54 rating |

## Calculations

**Input current** (opto LED drops about 1.2 V; R = R1 + R2 = 3.2 kΩ; R4
takes about 0.25 mA of it, ignored below):

| Lamp voltage | Current | Power in R1+R2 |
|---|---|---|
| 12 V DC (minimum) | (12 − 1.2) / 3200 = **3.4 mA** | 0.04 W |
| 24 V DC (normal) | **7.1 mA** | 0.16 W |
| 30 V DC / 24 VAC (34 V peak) | 9.0 mA (peak 10 mA) | 0.27 W, so each 1 W resistor runs cool |
| **Fault:** R1 or R2 shorted, 30 V | 18 mA | 0.55 W in the remaining 1 W resistor; PC814 is rated 50 mA |

**Does the opto pull the input low reliably?** The PC814's current transfer
ratio (CTR) is at least 20%. At 12 V the transistor can sink 3.4 mA × 0.2 =
0.68 mA. The pull-up needs only 3.3 V / 22 kΩ = 0.15 mA. That is a 4.5×
margin, enough to cover the CTR's slow decline over years.

**Leakage when the lamp is off:** a PMC transistor output that is "off"
can still leak a small current (typically well under 0.1 mA; triac or AC
outputs can leak 1–2 mA). With an incandescent lamp in parallel, the lamp
soaks that up. An LED lamp does not, so the leakage would flow through our
opto. An opto CTR can be up to 300%, so 0.1 mA in could sink 0.3 mA out,
enough to pull the input low and read **RUN with the lamp off**. R4 fixes
this: the opto LED does not conduct below about 0.9 V, and up to 0.9 V /
4.7 kΩ ≈ 0.19 mA flows harmlessly through R4 instead. When the lamp is on,
R4 takes only about 0.25 mA of the 3.4–9 mA. If a triac-output machine still
reads RUN with the lamp off (a bench and install test step checks for this),
reduce R4 to 1 kΩ. That absorbs about 0.9 mA, and the on-current is still
fine above 18 V.

**Power:** the ESP32-C3 averages under 0.3 W with BLE active, so the 24 V
side draws about 15–30 mA. F2 at 0.5 A only blows on a real fault.

## Why each protective part is where it is

* **F1 at the tap point, not at the device:** a short in the cable (pinched
  in a door hinge, cut by a chip conveyor) is then limited by the fuse right
  where it joins the machine's lamp circuit. The device's own resistors
  already limit normal current to under 10 mA.
* **No TVS or capacitor across the machine's lamp terminals:** a failed
  component there could short the PMC output. On the machine side we add
  only high-resistance series parts and the fuse.
* **The TVS on the power input is behind our own fuse F2:** if it fails
  shorted, it blows F2 and the machine's 24 V supply is unaffected.

## ⚠ Coin cell warning (DS3231 modules)

The cheap blue "ZS-042" DS3231 modules include a charging circuit (a 200 Ω
resistor and a diode) that charges the coin cell. A standard **CR2032 is not
rechargeable**: charging it can make it swell or leak. Either:

* use a breakout without charging, such as the Adafruit 3013 (recommended), or
* remove the resistor/diode on the ZS-042 (or use an LIR2032 rechargeable
  cell, which then holds time for weeks rather than years).

## Antenna and mounting

A steel electrical cabinet is a Faraday cage: BLE from inside it will
barely reach the operator's position. Mount the plastic enclosure **outside**
the cabinet, on its side wall, out of coolant and chip spray, with the cable
through a gland. If it must go inside, use a `-1U` module with an external
antenna on a bulkhead connector.

## Status LED (dev kit RGB)

| Colour | Meaning |
|---|---|
| Blue | Booting / input state not yet decided |
| Green | RUN (lamp on) |
| Red | STOP (lamp off) |
| Amber | BLINK |
