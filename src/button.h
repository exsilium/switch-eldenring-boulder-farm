#pragma once

#include <Arduino.h>

// GPIO0 (BOOT) long-press detector.
//
// Emits exactly one "long-press" event per physical hold: once the button has
// been held for HOLD_DURATION_MS the event fires a single time, and will not
// fire again until the button is released and pressed once more.
namespace button {

// Call once from setup().
void begin();

// Call every loop() iteration. Returns true on the single iteration where a
// completed long-press is detected.
bool update();

// Fractional progress (0.0 .. 1.0) of the current hold. 0.0 when not pressed.
// Useful for rendering a progress bar while arming.
float holdProgress();

// True while the button is physically down.
bool isDown();

}  // namespace button
