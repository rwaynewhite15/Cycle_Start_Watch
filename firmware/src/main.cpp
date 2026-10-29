// Cycle Start Watch - phase 1 firmware.
//
// What runs where:
//   input task (high priority, every 2 ms): samples the optocoupler, runs the
//       LampFilter, and queues state changes with their exact start time.
//   loop() (Arduino main task): writes queued changes to flash, keeps the
//       heartbeat, applies time/config requests from BLE, drives the LED.
//   NimBLE task: answers the phone (ble_service.cpp).
// Sampling lives in its own task so that slow work on the main loop (a flash
// write, waiting for the RTC's second boundary) can never delay or lose an edge.
//
// Read-only by design: the lamp is sensed through an optocoupler and this
// firmware has no output wired toward the machine. That guarantee comes from
// the hardware, not from this code.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <atomic>

#include "ble_service.h"
#include "device_clock.h"
#include "event_log.h"
#include "lamp_filter.h"
#include "protocol.h"
#include "settings.h"

#ifndef CSW_FW_VERSION
#define CSW_FW_VERSION "dev"
#endif

static const uint32_t kSampleMs = 2;
static const uint32_t kHeartbeatMs = 60 * 1000;
static const uint32_t kBondResetHoldMs = 5000;

static Settings g_settings;
static DeviceClock g_clock;
static EventLog g_log;

// --- shared between the input task and the main loop -----------------------
struct QueuedChange {
  LampState state;
  int64_t since_mono_ms;
};
static QueueHandle_t g_changes;
static portMUX_TYPE g_filter_mux = portMUX_INITIALIZER_UNLOCKED;
static FilterConfig g_filter_cfg;        // guarded by g_filter_mux
static bool g_filter_cfg_dirty = false;  // guarded by g_filter_mux
static std::atomic<uint8_t> g_input_level{0};

// --- shared between the main loop and BLE callbacks -------------------------
static portMUX_TYPE g_state_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t g_state = LAMP_UNKNOWN;  // last logged state
static uint32_t g_state_since = 0;

static portMUX_TYPE g_req_mux = portMUX_INITIALIZER_UNLOCKED;
static bool g_time_req = false;
static int64_t g_time_req_epoch = 0, g_time_req_mono = 0;
static bool g_cfg_req = false;
static Settings g_cfg_req_settings;
static char g_cfg_error[64] = "";

// ============================================================ input task ===
static void inputTask(void*) {
  LampFilter filter;
  FilterConfig cfg;
  portENTER_CRITICAL(&g_filter_mux);
  cfg = g_filter_cfg;
  portEXIT_CRITICAL(&g_filter_mux);
  filter.configure(cfg);

  TickType_t wake = xTaskGetTickCount();
  for (;;) {
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(kSampleMs));

    portENTER_CRITICAL(&g_filter_mux);
    bool dirty = g_filter_cfg_dirty;
    if (dirty) cfg = g_filter_cfg;
    g_filter_cfg_dirty = false;
    portEXIT_CRITICAL(&g_filter_mux);
    if (dirty) filter.configure(cfg);

    // Opto conducts (lamp powered) -> transistor pulls the pin LOW.
    bool active = digitalRead(CSW_PIN_LAMP) == LOW;
    int64_t now = monoMs();
    StateChange c;
    if (filter.update(active, (uint32_t)now, &c)) {
      // since_ms is a 32-bit time in the past; rebuild the 64-bit value.
      QueuedChange q{c.state, now - (int64_t)(uint32_t)((uint32_t)now - c.since_ms)};
      xQueueSend(g_changes, &q, 0);  // queue holds 32; the main loop drains it every few ms
    }
    g_input_level.store(filter.level() ? 1 : 0);
  }
}

// ======================================================= helpers ===========
static EventRecord makeRecord(EventType type, uint32_t value, int64_t epoch_ms) {
  EventRecord r{};
  r.ts = (uint32_t)(epoch_ms / 1000);
  r.ms = (uint16_t)(epoch_ms % 1000);
  r.type = type;
  r.flags = g_clock.valid() ? 0 : EVF_TIME_INVALID;
  r.value = value;
  return r;
}

static void logEvent(EventType type, uint32_t value, int64_t epoch_ms) {
  EventRecord r = makeRecord(type, value, epoch_ms);
  if (g_log.append(&r))
    Serial.printf("[log] #%lu type=%u value=%lu ts=%lu.%03u%s\n", (unsigned long)r.seq, r.type,
                  (unsigned long)r.value, (unsigned long)r.ts, r.ms, r.flags ? " (time invalid)" : "");
}

static void setLed(uint8_t state) {
#ifdef CSW_PIN_RGB
  // Dim colours: green = in cycle, red = stopped, yellow = flashing, blue = starting up.
  switch (state) {
    case LAMP_RUN: neopixelWrite(CSW_PIN_RGB, 0, 12, 0); break;
    case LAMP_STOP: neopixelWrite(CSW_PIN_RGB, 12, 0, 0); break;
    case LAMP_BLINK: neopixelWrite(CSW_PIN_RGB, 10, 6, 0); break;
    default: neopixelWrite(CSW_PIN_RGB, 0, 0, 12); break;
  }
#endif
}

// ============================================ app hooks used by BLE layer ===
StatusPayload appStatus() {
  StatusPayload p{};
  p.protocol = CSW_PROTOCOL_VERSION;
  portENTER_CRITICAL(&g_state_mux);
  p.state = g_state;
  p.state_since = g_state_since;
  portEXIT_CRITICAL(&g_state_mux);
  p.input = g_input_level.load();
  p.flags = (g_clock.valid() ? STF_TIME_VALID : 0) | (g_clock.rtcPresent() ? STF_RTC_PRESENT : 0) |
            (g_log.error() ? STF_LOG_ERROR : 0);
  p.now = (uint32_t)(g_clock.nowMs() / 1000);
  p.first_seq = g_log.firstSeq();
  p.next_seq = g_log.nextSeq();
  return p;
}

size_t appReadLog(uint32_t from_seq, EventRecord* out, size_t max) { return g_log.readFrom(from_seq, out, max); }

int64_t appClockNowMs() { return g_clock.nowMs(); }

void appRequestTimeSet(int64_t epoch_ms) {
  portENTER_CRITICAL(&g_req_mux);
  g_time_req = true;
  g_time_req_epoch = epoch_ms;
  g_time_req_mono = monoMs();
  portEXIT_CRITICAL(&g_req_mux);
}

String appConfigJson() {
  // Read on the BLE task; the fields are only changed on the main loop, and a
  // torn read of a config value is harmless here.
  JsonDocument doc;
  doc["name"] = g_settings.name;
  doc["invert"] = g_settings.filter.invert;
  doc["debounce_ms"] = g_settings.filter.debounce_ms;
  doc["settle_ms"] = g_settings.filter.settle_ms;
  doc["fw"] = CSW_FW_VERSION;
  doc["protocol"] = CSW_PROTOCOL_VERSION;
  if (g_cfg_error[0]) doc["error"] = g_cfg_error;
  String out;
  serializeJson(doc, out);
  return out;
}

bool appRequestConfig(const char* json, size_t len) {
  JsonDocument doc;
  Settings s = g_settings;
  const char* err = nullptr;

  if (deserializeJson(doc, json, len)) {
    err = "invalid JSON";
  } else {
    if (doc["name"].is<const char*>()) {
      const char* n = doc["name"];
      if (strlen(n) == 0 || strlen(n) >= sizeof(s.name)) err = "name must be 1-24 characters";
      else strlcpy(s.name, n, sizeof(s.name));
    }
    if (doc["invert"].is<bool>()) s.filter.invert = doc["invert"];
    if (doc["debounce_ms"].is<int>()) {
      int v = doc["debounce_ms"];
      if (v < 20 || v > 5000) err = "debounce_ms must be 20-5000";
      else s.filter.debounce_ms = v;
    }
    if (doc["settle_ms"].is<int>()) {
      int v = doc["settle_ms"];
      if (v < 200 || v > 30000) err = "settle_ms must be 200-30000";
      else s.filter.settle_ms = v;
    }
    if (doc["passkey"].is<long>()) {
      long v = doc["passkey"];
      if (v < 100000 || v > 999999) err = "passkey must be 6 digits";
      else s.passkey = v;
    }
  }

  strlcpy(g_cfg_error, err ? err : "", sizeof(g_cfg_error));
  if (err) return false;
  portENTER_CRITICAL(&g_req_mux);
  g_cfg_req_settings = s;
  g_cfg_req = true;
  portEXIT_CRITICAL(&g_req_mux);
  return true;
}

// ================================================================ setup ====
void setup() {
  Serial.begin(115200);
  delay(300);
  setLed(LAMP_UNKNOWN);

  settings::load(&g_settings);
  Serial.printf("\n=== Cycle Start Watch fw %s ===\n", CSW_FW_VERSION);
  Serial.printf("machine name : %s\n", g_settings.name);
  Serial.printf("BLE passkey  : %06lu   <- write this on the unit's label\n", (unsigned long)g_settings.passkey);

  Wire.begin(CSW_PIN_SDA, CSW_PIN_SCL);
  g_clock.begin();
  g_log.begin();

  // If the device was running before this boot, record when it was last seen
  // alive so the app can show the gap as "no power" rather than guess.
  EventRecord last;
  uint32_t alive_ts;
  uint8_t alive_flags;
  if (heartbeat::read(&alive_ts, &alive_flags) && (!g_log.last(&last) || alive_ts >= last.ts)) {
    EventRecord r{};
    r.ts = alive_ts;
    r.type = EV_POWER_DOWN;
    r.flags = alive_flags;
    g_log.append(&r);
  }
  logEvent(EV_BOOT, (uint32_t)esp_reset_reason(), g_clock.nowMs());
  heartbeat::write((uint32_t)(g_clock.nowMs() / 1000), g_clock.valid() ? 0 : EVF_TIME_INVALID);

  pinMode(CSW_PIN_LAMP, INPUT_PULLUP);  // plus the external pull-up on the board
  pinMode(CSW_PIN_BUTTON, INPUT_PULLUP);

  g_filter_cfg = g_settings.filter;
  g_changes = xQueueCreate(32, sizeof(QueuedChange));
  xTaskCreate(inputTask, "input", 3072, nullptr, configMAX_PRIORITIES - 2, nullptr);

  ble::begin(g_settings);
  enableLoopWDT();  // if loop() ever hangs for 5 s the chip resets (logged as a BOOT with the reason)
}

// ================================================================= loop ====
static void handleStateChanges() {
  QueuedChange q;
  while (xQueueReceive(g_changes, &q, 0) == pdTRUE) {
    int64_t epoch_ms = g_clock.toEpochMs(q.since_mono_ms);
    logEvent(EV_STATE, q.state, epoch_ms);
    portENTER_CRITICAL(&g_state_mux);
    g_state = q.state;
    g_state_since = (uint32_t)(epoch_ms / 1000);
    portEXIT_CRITICAL(&g_state_mux);
    setLed(q.state);
    ble::notifyStatus();
  }
}

static void handleRequests() {
  bool time_req, cfg_req;
  int64_t epoch, mono;
  Settings s;
  portENTER_CRITICAL(&g_req_mux);
  time_req = g_time_req;
  epoch = g_time_req_epoch;
  mono = g_time_req_mono;
  cfg_req = g_cfg_req;
  if (cfg_req) s = g_cfg_req_settings;
  g_time_req = g_cfg_req = false;
  portEXIT_CRITICAL(&g_req_mux);

  if (time_req) {
    uint32_t old_sec;
    // Account for the time the request waited in the queue.
    if (g_clock.set(epoch + (monoMs() - mono), &old_sec)) {
      logEvent(EV_TIME_SET, old_sec, g_clock.nowMs());
      Serial.printf("[clock] set from phone (was %lu)\n", (unsigned long)old_sec);
      ble::notifyStatus();
    }
  }

  if (cfg_req) {
    bool invert_changed = s.filter.invert != g_settings.filter.invert;
    g_settings = s;
    settings::save(g_settings);
    portENTER_CRITICAL(&g_filter_mux);
    g_filter_cfg = g_settings.filter;
    g_filter_cfg_dirty = true;
    portEXIT_CRITICAL(&g_filter_mux);
    if (invert_changed) logEvent(EV_CONFIG, g_settings.filter.invert ? 1 : 0, g_clock.nowMs());
    ble::applySettings(g_settings);
    Serial.printf("[config] saved: %s\n", appConfigJson().c_str());
  }
}

static void handleButton() {
  // Hold BOOT for 5 s to forget every paired phone (e.g. a lost phone).
  static int64_t pressed_at = -1;
  bool down = digitalRead(CSW_PIN_BUTTON) == LOW;
  int64_t now = monoMs();
  if (!down) {
    pressed_at = -1;
  } else if (pressed_at < 0) {
    pressed_at = now;
  } else if (pressed_at > 0 && now - pressed_at >= kBondResetHoldMs) {
    ble::clearBonds();
    pressed_at = 0;  // 0 = done, wait for release
  }
}

void loop() {
  static int64_t last_heartbeat = monoMs();

  handleStateChanges();
  handleRequests();
  handleButton();
  g_clock.poll();

  if (monoMs() - last_heartbeat >= kHeartbeatMs) {
    last_heartbeat = monoMs();
    heartbeat::write((uint32_t)(g_clock.nowMs() / 1000), g_clock.valid() ? 0 : EVF_TIME_INVALID);
  }

  delay(5);
}
