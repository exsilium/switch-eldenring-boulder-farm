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
  RunMacro,   // re-arm and run the input macro
  Reattach,   // cycle the USB connection
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

// Show a short diagnostic note on the status phase label (thread-safe). Used
// to leave on-screen breadcrumbs while app_main does blocking work.
void setNote(const char *text);

// Feed a button event into the UI (menu navigation / selection).
void onButton(button::Event e);

// Pop a pending menu command (Command::None if none).
Command takeCommand();

}  // namespace ui
