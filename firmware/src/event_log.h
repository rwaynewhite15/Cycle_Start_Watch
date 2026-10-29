// Ring buffer of EventRecords in a LittleFS file.
//
// Why LittleFS: it is designed for power loss at any instant. A write becomes
// visible only when the file is closed, so a machine powered off mid-write
// leaves the previous complete record, never half of one. It also spreads
// writes across the flash so no block wears out early.
//
// Layout: record number `seq` lives in slot (seq - 1) % kCapacity of
// /events.bin. The file grows until it holds kCapacity records, then the
// oldest slot is overwritten. There is no separate header: at boot the file
// is scanned for the highest seq, so there is nothing to get out of step.
//
// Thread-safe: append() runs on the main loop, readFrom() on the BLE task.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "protocol.h"

class EventLog {
 public:
  // 32768 x 16 bytes = 512 KB. At a busy cell (2 changes per 60 s cycle,
  // 24/7) that is about 11 days of history between phone syncs.
  static const uint32_t kCapacity = 32768;

  bool begin();
  bool append(EventRecord* r);  // assigns r->seq
  size_t readFrom(uint32_t seq, EventRecord* out, size_t max);
  bool last(EventRecord* out);

  uint32_t firstSeq();  // 0 if empty
  uint32_t nextSeq();
  bool error() const { return error_; }

 private:
  SemaphoreHandle_t mutex_ = nullptr;
  uint32_t next_seq_ = 1;
  bool error_ = false;
};

// Heartbeat: the last time the device was known to be running, so after a
// power cut we know roughly when the machine went off.
namespace heartbeat {
void write(uint32_t ts, uint8_t flags);
bool read(uint32_t* ts, uint8_t* flags);
}  // namespace heartbeat
