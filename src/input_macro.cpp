#include "input_macro.h"

#include "esp_timer.h"

// IDF replacement for Arduino millis() (the only framework dependency).
static inline unsigned long millis() {
  return (unsigned long)(esp_timer_get_time() / 1000);
}

namespace input_macro {

namespace {
// Press duration for each D-pad tap, and the gap between LEFT and RIGHT.
constexpr unsigned long kPressMs = 100;
constexpr unsigned long kGapMs = 1000;

enum class Phase { Idle, PressLeft, Gap, PressRight, Done };

Phase gPhase = Phase::Idle;
unsigned long gPhaseStart = 0;

void enter(Phase p) {
  gPhase = p;
  gPhaseStart = millis();
}
}  // namespace

void start() { enter(Phase::PressLeft); }

void reset() { gPhase = Phase::Idle; }

bool isRunning() {
  return gPhase != Phase::Idle && gPhase != Phase::Done;
}

bool isDone() { return gPhase == Phase::Done; }

bool update(procon::Input& in) {
  if (!isRunning()) return false;

  const unsigned long elapsed = millis() - gPhaseStart;
  bool justFinished = false;

  switch (gPhase) {
    case Phase::PressLeft:
      in.buttons[2] = (in.buttons[2] & ~procon::kDpadRight) | procon::kDpadLeft;
      if (elapsed >= kPressMs) {
        in.buttons[2] &= ~procon::kDpadLeft;
        enter(Phase::Gap);
      }
      break;

    case Phase::Gap:
      in.buttons[2] &= ~(procon::kDpadLeft | procon::kDpadRight);
      if (elapsed >= kGapMs) {
        enter(Phase::PressRight);
      }
      break;

    case Phase::PressRight:
      in.buttons[2] = (in.buttons[2] & ~procon::kDpadLeft) | procon::kDpadRight;
      if (elapsed >= kPressMs) {
        in.buttons[2] &= ~procon::kDpadRight;
        gPhase = Phase::Done;
        justFinished = true;
      }
      break;

    default:
      break;
  }

  return justFinished;
}

}  // namespace input_macro
