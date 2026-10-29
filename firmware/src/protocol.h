// Wire and storage formats shared by the event log and the BLE service.
//
// Everything here is documented for app authors in docs/ble-protocol.md.
// If you change a layout, bump CSW_PROTOCOL_VERSION and update that doc.
//
// All multi-byte integers are little-endian (the ESP32's native order, and
// the order Dart's ByteData / Python's struct "<" read by default).
#pragma once
#include <stdint.h>

#define CSW_PROTOCOL_VERSION 1

// ---------------------------------------------------------------- UUIDs ----
// A random 128-bit base. Only the 4th-6th hex digits change per characteristic.
#define CSW_SERVICE_UUID     "6c1a0000-5c3e-4b8e-9a3d-2f6e7c1b9a00"
#define CSW_CHR_STATUS_UUID  "6c1a0001-5c3e-4b8e-9a3d-2f6e7c1b9a00"  // read, notify
#define CSW_CHR_TIME_UUID    "6c1a0002-5c3e-4b8e-9a3d-2f6e7c1b9a00"  // read, write
#define CSW_CHR_CURSOR_UUID  "6c1a0003-5c3e-4b8e-9a3d-2f6e7c1b9a00"  // read, write
#define CSW_CHR_PAGE_UUID    "6c1a0004-5c3e-4b8e-9a3d-2f6e7c1b9a00"  // read
#define CSW_CHR_CONFIG_UUID  "6c1a0005-5c3e-4b8e-9a3d-2f6e7c1b9a00"  // read, write (JSON)

// ------------------------------------------------------------- states ----
enum LampState : uint8_t {
  LAMP_STOP = 0,     // lamp steadily off: not in cycle
  LAMP_RUN = 1,      // lamp steadily on: in cycle
  LAMP_BLINK = 2,    // lamp flashing (some MTBs flash it in feed hold / waiting)
  LAMP_UNKNOWN = 0xFF,  // not decided yet (just booted); never logged
};

// --------------------------------------------------------- event types ----
enum EventType : uint8_t {
  EV_STATE = 1,       // value = LampState
  EV_BOOT = 2,        // value = ESP reset reason (esp_reset_reason_t)
  EV_POWER_DOWN = 3,  // ts = last heartbeat before power was lost (<= 60 s early)
  EV_TIME_SET = 4,    // ts = new time; value = device time (UTC s) just before
  EV_CONFIG = 5,      // value = bit0 invert
};

// Event flags
#define EVF_TIME_INVALID 0x01  // device clock was not set when this was logged

// One log record: exactly 16 bytes, both in flash and over BLE.
#pragma pack(push, 1)
struct EventRecord {
  uint32_t seq;    // 1, 2, 3 ... never reused, survives reboots and wrap-around
  uint32_t ts;     // UTC seconds since 1970-01-01 (never local time)
  uint16_t ms;     // milliseconds part of the timestamp, 0..999
  uint8_t type;    // EventType
  uint8_t flags;   // EVF_*
  uint32_t value;  // meaning depends on type
};

// Status characteristic payload: 20 bytes, so it fits one notification even
// at the smallest BLE packet size (MTU 23).
struct StatusPayload {
  uint8_t protocol;     // CSW_PROTOCOL_VERSION
  uint8_t state;        // LampState currently reported
  uint8_t input;        // debounced input level right now, 0/1 (after invert)
  uint8_t flags;        // STF_*
  uint32_t now;         // device clock, UTC seconds
  uint32_t state_since; // UTC seconds when `state` began
  uint32_t first_seq;   // oldest record still in the log (0 if empty)
  uint32_t next_seq;    // seq the next record will get
};
#pragma pack(pop)

static_assert(sizeof(EventRecord) == 16, "EventRecord must stay 16 bytes");
static_assert(sizeof(StatusPayload) == 20, "StatusPayload must stay 20 bytes");

// Status flags
#define STF_TIME_VALID 0x01   // clock has been set (by RTC or phone)
#define STF_RTC_PRESENT 0x02  // DS3231 found on I2C
#define STF_LOG_ERROR 0x04    // last write to flash failed

// Log page characteristic returns up to this many records (32 * 16 = 512 bytes,
// the largest value a BLE attribute may have).
#define CSW_PAGE_RECORDS 32
