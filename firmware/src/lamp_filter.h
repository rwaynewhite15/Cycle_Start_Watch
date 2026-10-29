// Turns raw optocoupler samples into clean RUN / STOP / BLINK state changes.
//
// Pure logic, no Arduino calls, so it is unit-tested on the PC
// (test/test_lamp_filter). Times are uint32 milliseconds; differences are
// taken with unsigned subtraction so the 49-day wrap of a 32-bit ms counter
// does not matter.
//
// Three stages, each solving one physical problem:
//
//  1. Pulse stretch (stretch_ms, ~25 ms). An AC-powered lamp, or an LED that
//     the panel dims with PWM, switches the opto on and off 100-1000 times a
//     second. Any "active" sample in the last stretch_ms counts as active.
//
//  2. Debounce (debounce_ms, ~200 ms). Ignores glitches: contact bounce,
//     electrical noise from spindle drives, a lamp test flash.
//
//  3. Settle + blink detection (settle_ms, ~1500 ms). A level change is only
//     reported once it has held for settle_ms. If instead the debounced level
//     keeps flipping (blink_edges edges inside blink_window_ms) the lamp is
//     flashing and BLINK is reported. Reported changes are back-dated to when
//     they actually began, so the settle delay costs no timestamp accuracy.
//     Trade-off: a stop shorter than settle_ms is not logged.
#pragma once
#include <stdint.h>
#include "protocol.h"

struct FilterConfig {
  bool invert = false;            // swap active/inactive (e.g. a lamp that is ON when stopped)
  uint16_t stretch_ms = 25;
  uint16_t debounce_ms = 200;
  uint16_t settle_ms = 1500;
  uint16_t blink_window_ms = 4000;
  uint8_t blink_edges = 4;
};

struct StateChange {
  LampState state;
  uint32_t since_ms;  // when the new state really began
};

class LampFilter {
 public:
  void configure(const FilterConfig& cfg);

  // Feed one sample. `active` is the electrical level (true = opto conducting,
  // i.e. lamp powered) before inversion. Returns true and fills `out` when the
  // reported state changes. The first call after construction/reset reports
  // the initial state once it has settled.
  bool update(bool active, uint32_t now_ms, StateChange* out);

  LampState state() const { return reported_; }
  bool level() const { return stable_; }  // debounced level after invert

 private:
  static const uint8_t kMaxEdges = 16;

  FilterConfig cfg_;
  bool started_ = false;

  // stage 1
  uint32_t last_active_ms_ = 0;
  bool seen_active_ = false;
  // stage 2
  bool candidate_ = false;
  uint32_t candidate_since_ = 0;
  bool stable_ = false;
  // stage 3
  uint32_t edges_[kMaxEdges] = {};
  uint8_t edge_count_ = 0;   // number of valid entries (newest last)
  uint32_t last_edge_ms_ = 0;
  LampState reported_ = LAMP_UNKNOWN;

  void addEdge(uint32_t t);
  uint8_t edgesWithin(uint32_t now_ms, uint32_t window_ms, uint32_t* first) const;
};
