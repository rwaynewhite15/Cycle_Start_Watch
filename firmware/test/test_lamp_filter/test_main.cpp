// PC-side tests for the lamp filter: `pio test -e native`
#include <unity.h>
#include <vector>
#include "lamp_filter.h"

// Simulates sampling every 2 ms, like the firmware's input task.
struct Sim {
  LampFilter f;
  uint64_t t = 0;  // 64-bit here so the test loop itself survives the wrap
  std::vector<StateChange> changes;

  explicit Sim(FilterConfig cfg = FilterConfig()) { f.configure(cfg); }

  // Hold the electrical level for `ms`.
  void hold(bool active, uint32_t ms) {
    for (uint64_t end = t + ms; t < end; t += 2) {
      StateChange c;
      if (f.update(active, (uint32_t)t, &c)) changes.push_back(c);
    }
  }
  // 50/60 Hz AC through a bidirectional opto: conducts except near zero crossings.
  void holdAc(uint32_t ms) {
    for (uint64_t end = t + ms; t < end; t += 2) {
      bool active = (t % 10) != 0;  // drops out every 10 ms
      StateChange c;
      if (f.update(active, (uint32_t)t, &c)) changes.push_back(c);
    }
  }
};

void setUp() {}
void tearDown() {}

void test_initial_state_reported_after_settle() {
  Sim s;
  s.hold(false, 1400);
  TEST_ASSERT_EQUAL(0, s.changes.size());
  s.hold(false, 200);
  TEST_ASSERT_EQUAL(1, s.changes.size());
  TEST_ASSERT_EQUAL(LAMP_STOP, s.changes[0].state);
  TEST_ASSERT_EQUAL_UINT32(0, s.changes[0].since_ms);
}

void test_run_then_stop_backdated() {
  Sim s;
  s.hold(false, 2000);
  s.hold(true, 60000);   // lamp turns on at t=2000
  s.hold(false, 5000);   // off at t=62000
  TEST_ASSERT_EQUAL(3, s.changes.size());
  TEST_ASSERT_EQUAL(LAMP_RUN, s.changes[1].state);
  TEST_ASSERT_EQUAL_UINT32(2000, s.changes[1].since_ms);
  TEST_ASSERT_EQUAL(LAMP_STOP, s.changes[2].state);
  // stretch stage holds the level for up to 25 ms after the last active sample
  TEST_ASSERT_UINT32_WITHIN(30, 62000, s.changes[2].since_ms);
}

void test_glitch_ignored() {
  Sim s;
  s.hold(false, 2000);
  s.hold(true, 100);  // shorter than debounce
  s.hold(false, 5000);
  TEST_ASSERT_EQUAL(1, s.changes.size());
  TEST_ASSERT_EQUAL(LAMP_STOP, s.changes[0].state);
}

void test_ac_lamp_reads_as_steady_run() {
  Sim s;
  s.hold(false, 2000);
  s.holdAc(30000);
  TEST_ASSERT_EQUAL(2, s.changes.size());
  TEST_ASSERT_EQUAL(LAMP_RUN, s.changes[1].state);
}

void test_blinking_lamp_reports_blink_not_toggles() {
  Sim s;
  s.hold(true, 10000);  // running
  for (int i = 0; i < 10; i++) {  // 1 Hz flash: 500 ms on, 500 ms off
    s.hold(false, 500);
    s.hold(true, 500);
  }
  s.hold(false, 10000);  // steady off
  TEST_ASSERT_EQUAL(3, s.changes.size());
  TEST_ASSERT_EQUAL(LAMP_RUN, s.changes[0].state);
  TEST_ASSERT_EQUAL(LAMP_BLINK, s.changes[1].state);
  TEST_ASSERT_UINT32_WITHIN(30, 10000, s.changes[1].since_ms);  // back-dated to first flash
  TEST_ASSERT_EQUAL(LAMP_STOP, s.changes[2].state);
}

void test_short_stops_between_cycles_are_logged() {
  Sim s;
  s.hold(false, 2000);
  for (int i = 0; i < 5; i++) {  // 20 s cycle, 3 s load/unload
    s.hold(true, 20000);
    s.hold(false, 3000);
  }
  TEST_ASSERT_EQUAL(11, s.changes.size());
  for (size_t i = 1; i < s.changes.size(); i++)
    TEST_ASSERT_NOT_EQUAL(LAMP_BLINK, s.changes[i].state);
}

void test_invert() {
  FilterConfig cfg;
  cfg.invert = true;
  Sim s(cfg);
  s.hold(false, 2000);
  TEST_ASSERT_EQUAL(1, s.changes.size());
  TEST_ASSERT_EQUAL(LAMP_RUN, s.changes[0].state);
}

void test_invert_change_rereports_without_fake_edge() {
  Sim s;
  s.hold(false, 2000);
  FilterConfig cfg;
  cfg.invert = true;
  s.f.configure(cfg);
  s.hold(false, 2000);
  TEST_ASSERT_EQUAL(2, s.changes.size());
  TEST_ASSERT_EQUAL(LAMP_RUN, s.changes[1].state);
  TEST_ASSERT_EQUAL_UINT32(2000, s.changes[1].since_ms);
}

void test_millis_wraparound() {
  Sim s;
  s.t = 0xFFFFFFFFu - 3001;  // 3 s before the 32-bit ms counter wraps
  s.hold(false, 2000);
  s.hold(true, 5000);        // crosses the wrap
  TEST_ASSERT_EQUAL(2, s.changes.size());
  TEST_ASSERT_EQUAL(LAMP_RUN, s.changes[1].state);
  TEST_ASSERT_EQUAL_UINT32((uint32_t)(0xFFFFFFFFu - 1001), s.changes[1].since_ms);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_initial_state_reported_after_settle);
  RUN_TEST(test_run_then_stop_backdated);
  RUN_TEST(test_glitch_ignored);
  RUN_TEST(test_ac_lamp_reads_as_steady_run);
  RUN_TEST(test_blinking_lamp_reports_blink_not_toggles);
  RUN_TEST(test_short_stops_between_cycles_are_logged);
  RUN_TEST(test_invert);
  RUN_TEST(test_invert_change_rereports_without_fake_edge);
  RUN_TEST(test_millis_wraparound);
  return UNITY_END();
}
