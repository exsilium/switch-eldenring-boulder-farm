#include "button.h"

namespace button {

namespace {
constexpr uint8_t kPin = 0;              // GPIO0 / BOOT button (active LOW)
constexpr unsigned long kHoldMs = 2000;  // long-press threshold
constexpr unsigned long kDebounceMs = 20;

bool gWasDown = false;        // debounced state on the previous update
bool gFired = false;          // long-press already emitted for this hold
unsigned long gPressStart = 0;
unsigned long gLastEdgeMs = 0;
bool gRawLast = false;
}  // namespace

void begin() {
  pinMode(kPin, INPUT_PULLUP);  // reads LOW when pressed
  gWasDown = false;
  gFired = false;
  gRawLast = false;
}

bool update() {
  const unsigned long now = millis();
  const bool raw = (digitalRead(kPin) == LOW);

  // Debounce: only accept a level change that is stable for kDebounceMs.
  if (raw != gRawLast) {
    gRawLast = raw;
    gLastEdgeMs = now;
  }
  bool down = gWasDown;
  if ((now - gLastEdgeMs) >= kDebounceMs) {
    down = raw;
  }

  bool event = false;

  if (down && !gWasDown) {
    // Press started.
    gPressStart = now;
    gFired = false;
  } else if (down) {
    if (!gFired && (now - gPressStart) >= kHoldMs) {
      gFired = true;
      event = true;
    }
  }

  gWasDown = down;
  return event;
}

float holdProgress() {
  if (!gWasDown) return 0.0f;
  float pct = (float)(millis() - gPressStart) / (float)kHoldMs;
  if (pct < 0.0f) pct = 0.0f;
  if (pct > 1.0f) pct = 1.0f;
  return pct;
}

bool isDown() { return gWasDown; }

}  // namespace button
