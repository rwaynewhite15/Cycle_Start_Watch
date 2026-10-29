# App (planned): Flutter, iOS + Android

Not started yet. The plan is to build it after the firmware passes the
bench test, so the BLE protocol is proven first with nRF Connect and
`tools/csw_client.py`.

## Planned stack

| Need | Package | Why |
|---|---|---|
| BLE | `flutter_blue_plus` | Most-used Flutter BLE plugin; handles iOS/Android pairing and MTU differences |
| Database | `drift` (on SQLite) | Typed SQL. You write real SQL, and it checks it at compile time |
| Charts | `fl_chart` | Timeline and bar charts |
| CSV | `csv` + `share_plus` | Export through the phone's share sheet |

## Sync logic

1. Scan with a filter for service `6c1a0000-…`. List devices by name.
2. Connect, pair (the OS prompts for the passkey), request MTU 517 (Android).
3. Write the phone time to the Time characteristic.
4. Read Status. Download from `MAX(seq) + 1` for this device (see
   [docs/ble-protocol.md](../docs/ble-protocol.md)).
5. Insert the records. `(device_id, seq)` is the primary key, so re-downloading is harmless.

## Schema (draft)

```sql
CREATE TABLE device (
  id          TEXT PRIMARY KEY,      -- BLE address / iOS identifier
  name        TEXT NOT NULL,         -- machine name from Config
  last_sync   INTEGER                -- UTC seconds
);

CREATE TABLE event (
  device_id   TEXT NOT NULL REFERENCES device(id),
  seq         INTEGER NOT NULL,
  ts_ms       INTEGER NOT NULL,      -- UTC epoch ms (ts*1000 + ms), corrected if flagged
  type        INTEGER NOT NULL,      -- 1 STATE, 2 BOOT, 3 POWER_DOWN, 4 TIME_SET, 5 CONFIG
  value       INTEGER NOT NULL,
  flags       INTEGER NOT NULL,
  PRIMARY KEY (device_id, seq)
);

-- Derived on sync: one row per contiguous state. 'OFF' = POWER_DOWN..BOOT.
CREATE TABLE interval (
  device_id   TEXT NOT NULL,
  state       TEXT NOT NULL,         -- RUN, STOP, BLINK, OFF, UNKNOWN
  start_ms    INTEGER NOT NULL,
  end_ms      INTEGER NOT NULL
);

CREATE TABLE shift (                 -- the scheduled-time calendar, local time
  name        TEXT NOT NULL,         -- 'A', 'B', 'C'
  weekday     INTEGER NOT NULL,      -- 1 = Monday
  start_local TEXT NOT NULL,         -- '06:00'
  end_local   TEXT NOT NULL          -- '14:30' (may be past midnight)
);
```

In-cycle % for one shift instance = RUN time overlapping the shift ÷ the
shift's scheduled length. The app expands `shift` into concrete UTC
windows, with the phone's time zone rules, and clips intervals to them:

```sql
SELECT w.shift_name, w.start_ms,
       SUM(MAX(0, MIN(i.end_ms, w.end_ms) - MAX(i.start_ms, w.start_ms))) * 100.0
         / (w.end_ms - w.start_ms) AS in_cycle_pct
FROM shift_window w                -- temp table: one row per concrete shift
JOIN interval i
  ON i.device_id = :device AND i.state = 'RUN'
 AND i.start_ms < w.end_ms AND i.end_ms > w.start_ms
GROUP BY w.shift_name, w.start_ms;
```

Report settings to expose: whether OFF counts as down, and whether BLINK
counts as RUN or STOP (that depends on what the flashing means on that
machine model).

## Screens (first version)

1. **Devices:** nearby and known machines, last sync, current state.
2. **Machine:** today's timeline (RUN green, STOP red, BLINK amber, OFF grey), plus in-cycle % per shift.
3. **Longest stops:** top 10 STOP/OFF intervals in the chosen range.
4. **Export:** CSV of intervals or raw events for a date range.
5. **Settings:** shift calendar, machine name, invert, passkey.
