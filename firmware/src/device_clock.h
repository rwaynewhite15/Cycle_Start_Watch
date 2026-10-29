// Wall-clock time for log timestamps.
//
// The DS3231 is the source of truth: it is accurate to about 1 minute per
// year and keeps running on its coin cell while the machine is powered off.
// Reading it over I2C for every event would work, but it only has 1-second
// resolution, so instead we note which ESP32 timer value matches a DS3231
// second boundary and count milliseconds from there. Every 6 hours we re-check
// against the DS3231 so the ESP32's less accurate crystal cannot drift far.
//
// All times are UTC. The phone app converts to local time; storing local time
// on the device would break shift reports at every daylight-saving change.
#pragma once
#include <stdint.h>

// Monotonic milliseconds since boot (64-bit: does not wrap).
int64_t monoMs();

class DeviceClock {
 public:
  void begin();  // call after Wire.begin(); blocks up to ~1 s to align to the RTC

  int64_t nowMs() const { return toEpochMs(monoMs()); }
  int64_t toEpochMs(int64_t mono_ms) const { return base_epoch_ms_ + (mono_ms - base_mono_ms_); }

  // Time is "valid" once it came from a running RTC or the phone. Before that
  // timestamps count from 1970-01-01 and events carry EVF_TIME_INVALID.
  bool valid() const { return valid_; }
  bool rtcPresent() const { return rtc_present_; }

  void poll();  // call from loop(); periodic non-blocking re-check against RTC

  // Set the clock (from the phone). Ignored if already within 2 s. Blocks up
  // to 1 s so the DS3231 second boundary lands on the right millisecond.
  // Returns true if the clock was changed; *old_sec gets the previous time.
  bool set(int64_t epoch_ms, uint32_t* old_sec);

 private:
  bool rtc_present_ = false;
  bool valid_ = false;
  int64_t base_epoch_ms_ = 0;
  int64_t base_mono_ms_ = 0;

  int64_t next_check_ms_ = 0;
  bool checking_ = false;
  uint32_t check_sec_ = 0;
  int64_t last_poll_ms_ = 0;

  bool readRtc(uint32_t* sec);
};
