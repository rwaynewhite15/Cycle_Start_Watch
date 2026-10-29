# BLE protocol (version 1)

The source of truth is `firmware/src/protocol.h`. A Python decoder is in
`tools/csw_protocol.py`. All integers are **little-endian**. All times are
**UTC**.

## Advertising

* Name: the machine name, e.g. `OKUMA-LB-07` (up to 24 characters). Before a
  name is set it is `CSW-xxxx`, where xxxx comes from the chip's MAC address.
* Service UUID `6c1a0000-5c3e-4b8e-9a3d-2f6e7c1b9a00` is in the advertising
  packet, so the app can scan with a filter for it.
* One connection at a time. A second phone is disconnected right away.

## Security

Every characteristic requires an **encrypted, authenticated** link (LE
Secure Connections, passkey entry, bonded). Accessing a characteristic
before pairing returns *Insufficient Authentication*, and iOS and Android
then show the passkey prompt by themselves. The 6-digit passkey is printed
on the serial log at every boot and should be on the unit's label. Holding
BOOT for 5 s deletes all pairings.

## Characteristics

Each UUID is `6c1a000X-5c3e-4b8e-9a3d-2f6e7c1b9a00`.

| X | Name | Props | Format |
|---|---|---|---|
| 1 | Status | read, notify | 20-byte struct below; notified on every state change and clock set |
| 2 | Time | read, write | `int64` epoch milliseconds (8 bytes) |
| 3 | Log cursor | read, write | `uint32` seq |
| 4 | Log page | read | 0–32 records × 16 bytes, starting at the cursor |
| 5 | Config | read, write | UTF-8 JSON |

### Status (20 bytes)

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | protocol version (1) |
| 1 | u8 | state: 0 STOP, 1 RUN, 2 BLINK, 255 UNKNOWN (just booted) |
| 2 | u8 | input: debounced input level right now (after invert) |
| 3 | u8 | flags: bit0 time valid, bit1 RTC present, bit2 flash write error |
| 4 | u32 | device clock, UTC seconds |
| 8 | u32 | time the current state began, UTC seconds |
| 12 | u32 | first_seq: oldest record still stored (0 = log empty) |
| 16 | u32 | next_seq: the seq the next record will get |

### Event record (16 bytes)

| Offset | Type | Field |
|---|---|---|
| 0 | u32 | seq: 1, 2, 3, …, never reused |
| 4 | u32 | ts: UTC seconds |
| 8 | u16 | ms: 0–999 |
| 10 | u8 | type (below) |
| 11 | u8 | flags: bit0 = clock not set when logged |
| 12 | u32 | value (meaning depends on type) |

| type | Name | value | Notes |
|---|---|---|---|
| 1 | STATE | 0 STOP, 1 RUN, 2 BLINK | Timestamp is when the change actually began |
| 2 | BOOT | ESP32 reset reason (1 power-on, 3 software, 4 panic, 6/7 watchdog, 9 brownout, …) | State is unknown until the next STATE record |
| 3 | POWER_DOWN | 0 | ts = last heartbeat before the device lost power. The real power-off is 0–60 s later |
| 4 | TIME_SET | old device time, UTC seconds | ts = the new time |
| 5 | CONFIG | bit0 = invert | The input's meaning changed at this point |

Example: the record bytes
`2a 00 00 00  80 f4 55 69  fa 00  01  00  01 00 00 00` decode as seq 42,
ts 0x6955f480 = 1767240832 (2026-01-01 04:13:52 UTC) + 250 ms, STATE, flags
0, value 1 = RUN.

### Downloading the log

```
read Status                       -> next_seq, first_seq
cursor = last seq the app stored + 1   (or 1 the first time)
loop:
    write Cursor = cursor              (write with response)
    page = read Page                   (0..32 records)
    store page in SQLite (seq is the primary key, so a duplicate is harmless)
    if len(page) < 32: done
    cursor = page[-1].seq + 1
```

* Reading a page does **not** move the cursor. If a read fails, just read
  again.
* If `cursor < first_seq`, those records were overwritten. The page starts
  at `first_seq`, and the app should record a "data lost" gap.
* A page larger than the MTU is fetched as a BLE "long read". Both iOS and
  Android handle this automatically. Requesting a larger MTU (Android:
  `requestMtu(517)`) makes it faster.

### Time sync

Write the phone's current time as `int64` epoch ms. If the device is within
2 s already, nothing happens. Otherwise it waits for the next whole second
(up to 1 s), sets the RTC, logs `TIME_SET` and notifies Status. Do this on
every connection.

**Correcting flagged records:** records with flag bit0 set were logged
before the clock was set. Within one boot (between a BOOT and the next
BOOT), add `(TIME_SET.ts − TIME_SET.value)` to each flagged record's ts to
get the real time.

### Config (JSON)

Read:

```json
{"name":"OKUMA-LB-07","invert":false,"debounce_ms":200,"settle_ms":1500,"fw":"0.1.0","protocol":1}
```

Write any subset of the fields. `passkey` can be written but is never read
back:

```json
{"name":"OKUMA-LB-07"}
{"invert":true}
{"settle_ms":2500}
{"passkey":482913}
```

Limits: `name` 1–24 characters, `debounce_ms` 20–5000, `settle_ms`
200–30000, `passkey` 100000–999999. If a write is rejected, the next read
includes `"error":"…"` and nothing is changed.
