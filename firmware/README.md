# Firmware

PlatformIO project: ESP32 Arduino core 2.0.17, NimBLE-Arduino 2.x, RTClib, ArduinoJson.

```bash
pio test -e native            # filter unit tests on your PC (no board needed)
pio run -e esp32c3            # build for ESP32-C3-DevKitM-1
pio run -e esp32s3            # build for ESP32-S3-DevKitC-1
pio run -e esp32c3 -t upload && pio device monitor
```

The first build downloads the ESP32 toolchain (a few hundred MB). The
`.github/workflows/firmware.yml` workflow runs all three commands on every push.

## Source map

| File | Job |
|---|---|
| `src/main.cpp` | Wires everything together: input task, main loop, BLE hooks |
| `src/lamp_filter.*` | Raw samples → RUN/STOP/BLINK (pure logic, unit-tested) |
| `src/event_log.*` | 16-byte records in a LittleFS ring buffer; heartbeat file |
| `src/device_clock.*` | DS3231 + millisecond clock, phone time sync |
| `src/ble_service.*` | GATT server, pairing, log paging |
| `src/settings.*` | Name, passkey, filter settings in NVS |
| `src/protocol.h` | Record and BLE layouts. **Change this together with docs/ble-protocol.md** |
| `partitions.csv` | 4 MB flash: 2 × 1.5 MB app slots (room for future OTA) + 896 KB log |
| `test/test_lamp_filter/` | Unity tests for the filter |

## Pins (set in `platformio.ini`)

| Signal | ESP32-C3 | ESP32-S3 |
|---|---|---|
| Lamp input (active low, from opto) | GPIO4 | GPIO4 |
| I2C SDA / SCL (DS3231) | GPIO5 / GPIO6 | GPIO8 / GPIO9 |
| BOOT button (hold 5 s = forget pairings) | GPIO9 | GPIO0 |
| On-board RGB LED | GPIO8 | GPIO48 |

## Tuning (over BLE, Config characteristic)

| Key | Default | Use |
|---|---|---|
| `debounce_ms` | 200 | Raise if electrical noise causes false edges |
| `settle_ms` | 1500 | Must be longer than one on or off phase of a flashing lamp (default handles flashes faster than about 0.35 Hz); lower it to catch stops shorter than 1.5 s |
| `invert` | false | For a signal that is ON when the machine is stopped |
