// Host-side unit tests for the macro engine's condition-driven control flow
// (engine.h / engine.cpp): the interrupt/abort predicate + reset sequence, and
// the per-axis StickAxis helper. Uses an injected virtual clock so timing is
// deterministic -- no ESP-IDF needed (run with `pio test -e native`).

#include <unity.h>

#include "engine.h"

using namespace macro;

// ---- Virtual clock + rumble feed -----------------------------------------
static unsigned long gNow = 0;
static unsigned long testClock() { return gNow; }

// A = byte0 bit 0x08, B = byte0 bit 0x04 (see macro::Channel).
static bool aDown(const procon::Input& in) { return (in.buttons[0] & 0x08) != 0; }
static bool bDown(const procon::Input& in) { return (in.buttons[0] & 0x04) != 0; }

static const Step kMain[] = {
    Down(Channel::A),
    Wait(100),
    Up(Channel::A),
    Wait(100),
};
static const Step kReset[] = {
    Down(Channel::B),
    Wait(50),
    Up(Channel::B),
};
static bool deathPred(const TickContext& c) { return c.rumbleLeft >= 64; }

void setUp(void) {
  gNow = 0;
  Player::setClock(testClock);
}
void tearDown(void) { Player::setClock(nullptr); }

// Predicate never fires -> the main sequence loops normally, forever.
void test_normal_loop_no_interrupt(void) {
  Player p(kMain);
  p.setLoop(true);
  p.setInterrupt(deathPred, kReset);
  p.feedRumble(0, 0);  // never crosses the threshold
  p.start();
  procon::Input in;

  gNow = 0;
  p.update(in);
  TEST_ASSERT_TRUE(aDown(in));  // Down(A) then Wait(100): A held

  gNow = 100;
  p.update(in);
  TEST_ASSERT_FALSE(aDown(in));  // Up(A) then Wait(100): A released

  gNow = 200;
  p.update(in);  // end of main -> loop restart (neutral this tick)
  TEST_ASSERT_FALSE(p.isInterrupting());
  TEST_ASSERT_FALSE(p.isDone());

  gNow = 201;
  p.update(in);
  TEST_ASSERT_TRUE(aDown(in));  // main restarted: A held again
}

// Predicate fires mid-main -> abort, run the reset sequence to completion, then
// resume looping the main sequence.
void test_interrupt_aborts_and_resets(void) {
  Player p(kMain);
  p.setLoop(true);
  p.setInterrupt(deathPred, kReset);
  p.start();
  procon::Input in;

  gNow = 0;
  p.update(in);
  TEST_ASSERT_TRUE(aDown(in));  // mid-main, A held

  // Death: rumble spike aborts main and enters the reset sequence.
  p.feedRumble(100, 0);
  gNow = 10;
  p.update(in);
  TEST_ASSERT_TRUE(p.isInterrupting());
  TEST_ASSERT_FALSE(aDown(in));  // neutralised on abort

  // Reset sequence runs (Down(B), Wait(50), Up(B)); rumble stays high but the
  // predicate is not re-evaluated while interrupting.
  gNow = 10;
  p.update(in);
  TEST_ASSERT_TRUE(bDown(in));
  TEST_ASSERT_FALSE(aDown(in));

  gNow = 60;
  p.update(in);  // reset done -> resume main loop
  TEST_ASSERT_FALSE(p.isInterrupting());
  TEST_ASSERT_FALSE(bDown(in));

  // Rumble clears; main resumes on the next tick.
  p.feedRumble(0, 0);
  gNow = 61;
  p.update(in);
  TEST_ASSERT_TRUE(aDown(in));
}

// StickAxis sets a single axis, leaving the other at its accumulated value.
void test_stick_axis_sets_single_axis(void) {
  static const Step kAxis[] = {
      StickMove(Stick::Left, 0x400, 0x400),
      StickAxis(Stick::Left, Axis::X, 0xC00),  // change X, keep Y at 0x400
      Wait(10),
  };
  Player p(kAxis);
  p.start();
  procon::Input in;
  gNow = 0;
  p.update(in);
  TEST_ASSERT_EQUAL_UINT16(0xC00, in.lx);
  TEST_ASSERT_EQUAL_UINT16(0x400, in.ly);
  // Right stick untouched -> stays centred.
  TEST_ASSERT_EQUAL_UINT16(procon::kStickCenter, in.rx);
  TEST_ASSERT_EQUAL_UINT16(procon::kStickCenter, in.ry);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_normal_loop_no_interrupt);
  RUN_TEST(test_interrupt_aborts_and_resets);
  RUN_TEST(test_stick_axis_sets_single_axis);
  return UNITY_END();
}
