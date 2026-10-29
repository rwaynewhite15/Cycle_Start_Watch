#include "device_clock.h"

#include <Arduino.h>
#include <RTClib.h>
#include <Wire.h>
#include <esp_timer.h>

static RTC_DS3231 rtc;

static const int64_t kRecheckMs = 6LL * 3600 * 1000;
static const uint32_t kMinValidEpoch = 1735689600;  // 2025-01-01: anything earlier is a reset RTC

int64_t monoMs() { return esp_timer_get_time() / 1000; }

bool DeviceClock::readRtc(uint32_t* sec) {
  if (!rtc_present_) return false;
  *sec = rtc.now().unixtime();
  return true;
}

void DeviceClock::begin() {
  base_mono_ms_ = monoMs();
  base_epoch_ms_ = 0;

  rtc_present_ = rtc.begin(&Wire);
  if (!rtc_present_) {
    Serial.println("[clock] DS3231 not found - time invalid until set from phone");
    return;
  }
  // lostPower() reads the DS3231's oscillator-stop flag: set if the coin cell
  // died or was removed, meaning the stored time is meaningless.
  if (rtc.lostPower()) {
    Serial.println("[clock] DS3231 lost power (check coin cell) - time invalid until set from phone");
    return;
  }

  // Wait for the RTC's seconds to tick over so we know the millisecond phase.
  uint32_t s0, s1;
  readRtc(&s0);
  int64_t deadline = monoMs() + 1100;
  do {
    delay(2);
    readRtc(&s1);
  } while (s1 == s0 && monoMs() < deadline);

  base_mono_ms_ = monoMs();
  base_epoch_ms_ = (int64_t)s1 * 1000;
  valid_ = s1 >= kMinValidEpoch;
  next_check_ms_ = base_mono_ms_ + kRecheckMs;
  Serial.printf("[clock] RTC time %lu (%s)\n", (unsigned long)s1, valid_ ? "valid" : "INVALID, too old");
}

void DeviceClock::poll() {
  if (!rtc_present_ || !valid_) return;
  int64_t now = monoMs();
  if (!checking_) {
    if (now < next_check_ms_) return;
    checking_ = readRtc(&check_sec_);
    last_poll_ms_ = now;
    return;
  }
  // Watch for the next second boundary, polling every 10 ms.
  if (now - last_poll_ms_ < 10) return;
  last_poll_ms_ = now;
  uint32_t s;
  if (!readRtc(&s) || s == check_sec_) return;

  checking_ = false;
  next_check_ms_ = now + kRecheckMs;
  int64_t drift = (int64_t)s * 1000 - toEpochMs(now);
  if (drift > 50 || drift < -50) {
    base_mono_ms_ = now;
    base_epoch_ms_ = (int64_t)s * 1000;
    Serial.printf("[clock] corrected drift of %lld ms\n", (long long)drift);
  }
}

bool DeviceClock::set(int64_t epoch_ms, uint32_t* old_sec) {
  int64_t diff = epoch_ms - nowMs();
  if (valid_ && diff > -2000 && diff < 2000) return false;

  *old_sec = (uint32_t)(nowMs() / 1000);
  int64_t received_mono = monoMs();

  if (rtc_present_) {
    // Writing the DS3231 seconds register restarts its second, so wait for
    // the phone's next whole second and write that.
    uint32_t wait = 1000 - (uint32_t)(epoch_ms % 1000);
    delay(wait);
    uint32_t sec = (uint32_t)(epoch_ms / 1000) + 1;
    rtc.adjust(DateTime(sec));  // also clears the oscillator-stop flag
    base_mono_ms_ = monoMs();
    base_epoch_ms_ = (int64_t)sec * 1000;
  } else {
    base_mono_ms_ = received_mono;
    base_epoch_ms_ = epoch_ms;
  }
  valid_ = true;
  checking_ = false;
  next_check_ms_ = monoMs() + kRecheckMs;
  return true;
}
