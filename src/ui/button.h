#pragma once

#include <stdint.h>

// Single-button (GPIO0 / BOOT) input for the one-button UI. Distinguishes a
// short tap from a long hold so a single button can both navigate and select.
namespace button {

enum class Event {
  None,
  Short,  // emitted on release of a press shorter than the long-hold threshold
  Long,   // emitted once, the moment the hold threshold is crossed
};

void begin();

// Poll the button; call periodically (e.g. every ~20 ms). Returns at most one
// event per call.
Event poll();

// 0..1 progress of the current hold toward the long-press threshold (0 when up).
float holdProgress();

bool isDown();

}  // namespace button
