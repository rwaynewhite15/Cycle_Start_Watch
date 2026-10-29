#include "event_log.h"

#include <Arduino.h>
#include <LittleFS.h>

static const char* kLogPath = "/events.bin";
static const char* kAlivePath = "/alive.bin";

static uint32_t slotOffset(uint32_t seq) { return ((seq - 1) % EventLog::kCapacity) * sizeof(EventRecord); }

static bool validSeq(uint32_t seq) { return seq != 0 && seq != 0xFFFFFFFF; }

bool EventLog::begin() {
  mutex_ = xSemaphoreCreateMutex();
  // formatOnFail=true: a blank or corrupt partition is formatted on first boot.
  if (!LittleFS.begin(true)) {
    Serial.println("[log] LittleFS mount failed");
    error_ = true;
    return false;
  }

  if (!LittleFS.exists(kLogPath)) {
    File f = LittleFS.open(kLogPath, "w");
    f.close();
  }

  // Find the highest sequence number. ~0.5 s for a full 512 KB file.
  File f = LittleFS.open(kLogPath, "r");
  uint32_t max_seq = 0;
  EventRecord buf[64];
  size_t n;
  while ((n = f.read((uint8_t*)buf, sizeof(buf))) > 0) {
    for (size_t i = 0; i < n / sizeof(EventRecord); i++)
      if (validSeq(buf[i].seq) && buf[i].seq > max_seq) max_seq = buf[i].seq;
  }
  f.close();
  next_seq_ = max_seq + 1;
  uint32_t first = firstSeq();
  Serial.printf("[log] %lu records, next seq %lu, %u KB used of %u KB\n",
                (unsigned long)(first ? next_seq_ - first : 0), (unsigned long)next_seq_, (unsigned)(LittleFS.usedBytes() / 1024),
                (unsigned)(LittleFS.totalBytes() / 1024));
  return true;
}

uint32_t EventLog::firstSeq() {
  if (next_seq_ <= 1) return 0;
  return next_seq_ > kCapacity ? next_seq_ - kCapacity : 1;
}

uint32_t EventLog::nextSeq() { return next_seq_; }

bool EventLog::append(EventRecord* r) {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  r->seq = next_seq_;
  bool ok = false;
  File f = LittleFS.open(kLogPath, "r+");
  if (f) {
    ok = f.seek(slotOffset(r->seq)) && f.write((const uint8_t*)r, sizeof(*r)) == sizeof(*r);
    f.close();  // close commits the write to flash
  }
  if (ok) next_seq_++;
  error_ = !ok;
  xSemaphoreGive(mutex_);
  if (!ok) Serial.println("[log] write failed");
  return ok;
}

size_t EventLog::readFrom(uint32_t seq, EventRecord* out, size_t max) {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  uint32_t first = firstSeq();
  size_t n = 0;
  if (first != 0) {
    if (seq < first) seq = first;  // asked for records already overwritten
    File f = LittleFS.open(kLogPath, "r");
    while (f && n < max && seq < next_seq_) {
      if (!f.seek(slotOffset(seq)) || f.read((uint8_t*)&out[n], sizeof(EventRecord)) != sizeof(EventRecord))
        break;
      if (out[n].seq != seq) break;  // should never happen; stop rather than send garbage
      n++;
      seq++;
    }
    if (f) f.close();
  }
  xSemaphoreGive(mutex_);
  return n;
}

bool EventLog::last(EventRecord* out) {
  if (next_seq_ <= 1) return false;
  return readFrom(next_seq_ - 1, out, 1) == 1;
}

namespace heartbeat {

void write(uint32_t ts, uint8_t flags) {
  uint8_t buf[5];
  memcpy(buf, &ts, 4);
  buf[4] = flags;
  File f = LittleFS.open(kAlivePath, "w");
  if (f) {
    f.write(buf, sizeof(buf));
    f.close();
  }
}

bool read(uint32_t* ts, uint8_t* flags) {
  File f = LittleFS.open(kAlivePath, "r");
  if (!f) return false;
  uint8_t buf[5];
  bool ok = f.read(buf, sizeof(buf)) == sizeof(buf);
  f.close();
  if (ok) {
    memcpy(ts, buf, 4);
    *flags = buf[4];
  }
  return ok;
}

}  // namespace heartbeat
