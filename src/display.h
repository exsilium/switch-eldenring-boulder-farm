#pragma once

#include <stdint.h>

#include "app_state.h"

// TFT status surface. The TFT is the only debug/logging surface for this
// firmware (USB CDC is disabled so there is no USB serial).
namespace display {

// Call once from setup(). Powers the TFT rail, backlight and initialises the
// ST7789 panel.
void begin();

// Render the current application state. Cheap to call every loop; it only
// repaints when the rendered content actually changes.
void showState(AppState state);

// Live USB/handshake diagnostics. Shows the IN/OUT report counts, the host
// mount state, last subcommand, and milestone flags (Device-info / Stick-cal /
// set-Mode / Vibration). Internally throttled and change-gated to avoid
// flicker; safe to call every loop.
void showDiag(bool mounted, uint32_t inCount, uint32_t outCount, uint8_t lastSub,
              bool devInfo, bool stickCal, bool setMode, bool vibration);

// Draw the arming progress bar (0.0 .. 1.0) while the BOOT button is held.
void showHoldProgress(float pct);

}  // namespace display
