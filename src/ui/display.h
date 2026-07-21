#pragma once

#include <stdint.h>

#include "ui/button.h"

// LVGL + esp_lcd (ST7789) UI for the Feather ESP32-S3 TFT. Provides a live
// status screen and a one-button menu navigated with button::Event.
namespace ui {

// Handshake phase shown on the status screen.
enum class Phase {
  Detached,   // not mounted
  Mounted,    // mounted, handshake not started
  Handshake,  // device info queried, handshake underway
  Ready,      // standard input mode -> controller accepted
};

// Commands the menu raises for the application layer to act on.
enum class Command {
  None,
  RunMacro,     // start a (looping) macro run
  Reattach,     // cycle the USB connection
  TogglePause,  // pause / resume the active run
  StopMacro,    // stop the active run and neutralise the controller
};

// Bring up the TFT (power, SPI, ST7789 panel) and LVGL, and build the UI.
// If onSelfTestStep is non-null, it is called with the color index (0..4)
// before each boot self-test fill, so the caller can mirror it elsewhere
// (e.g. blink the NeoPixel) to confirm activity during startup.
using SelfTestStepFn = void (*)(int colorIndex);
void begin(SelfTestStepFn onSelfTestStep = nullptr);

// Push the latest handshake state to the status screen (thread-safe).
void setStatus(Phase phase, bool mounted, uint32_t in, uint32_t out,
               uint8_t lastSub, bool dev, bool cal, bool mode, bool vib);

// 0..1 hold-progress bar (mirrors button::holdProgress()).
void setHoldProgress(float pct);

// Push the live controller input to the RUNNING overlay's Pro Controller
// diagram, highlighting the currently pressed buttons and moving the analog
// stick dots (thread-safe; a no-op unless the RUNNING overlay is visible).
// `buttons` is the 3-byte report layout (see procon::Input); stick values are
// 12-bit with centre procon::kStickCenter.
void setControllerState(const uint8_t buttons[3], uint16_t lx, uint16_t ly,
                        uint16_t rx, uint16_t ry);

// Show a short diagnostic note on the status phase label (thread-safe). Used
// to leave on-screen breadcrumbs while app_main does blocking work.
void setNote(const char *text);

// Push a short macro-written status label onto the RUNNING overlay (e.g.
// "Death Detected"). Thread-safe; a no-op unless the RUNNING overlay is
// visible. Pass "" or nullptr to clear it. Kept in a small fixed buffer so the
// UI task never churns the heap.
void setRunStatus(const char *text);

// Mirror the macro's actual pause state onto the RUNNING overlay's play/pause
// glyph (thread-safe; a no-op unless the overlay is visible or unchanged).
// Needed because the engine can self-pause (e.g. after a death-triggered
// reset), not just in response to the button toggle.
void setRunPaused(bool paused);

// Push the decoded per-side host-rumble amplitude (0..255) to the RUNNING
// overlay's rumble meters. Thread-safe; a no-op unless the RUNNING overlay is
// visible; repaints only on change. The meters turn red once an amplitude
// crosses procon::kRumbleMin, and the highest value seen during the run is
// printed next to each bar (reset on (re)start and on pause).
void setRumble(uint16_t left, uint16_t right);

// Feed a button event into the UI (menu navigation / selection).
void onButton(button::Event e);

// Pop a pending menu command (Command::None if none).
Command takeCommand();

}  // namespace ui
