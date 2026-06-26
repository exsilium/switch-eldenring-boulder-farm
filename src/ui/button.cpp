#include "ui/button.h"

#include "driver/gpio.h"
#include "esp_timer.h"

namespace button {

namespace {
constexpr gpio_num_t kPin = GPIO_NUM_0;  // BOOT button, active LOW
constexpr uint32_t kLongMs = 700;        // short vs long-press threshold
constexpr uint32_t kDebounceMs = 20;

bool gDown = false;        // debounced pressed state
bool gRawLast = false;     // last raw sample
uint32_t gLastEdgeMs = 0;  // when the raw level last changed
uint32_t gPressStart = 0;  // when the current press began
bool gLongFired = false;   // long event already emitted for this hold

uint32_t nowMs() { return (uint32_t)(esp_timer_get_time() / 1000); }
}  // namespace

void begin() {
  gpio_config_t cfg = {};
  cfg.pin_bit_mask = 1ULL << kPin;
  cfg.mode = GPIO_MODE_INPUT;
  cfg.pull_up_en = GPIO_PULLUP_ENABLE;
  cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
  cfg.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&cfg);

  gDown = false;
  gRawLast = false;
  gLongFired = false;
}

Event poll() {
  const uint32_t now = nowMs();
  const bool raw = (gpio_get_level(kPin) == 0);  // active LOW

  // Debounce: accept a level only after it has been stable for kDebounceMs.
  if (raw != gRawLast) {
    gRawLast = raw;
    gLastEdgeMs = now;
  }
  bool down = gDown;
  if ((now - gLastEdgeMs) >= kDebounceMs) {
    down = raw;
  }

  Event ev = Event::None;

  if (down && !gDown) {
    // Press started.
    gPressStart = now;
    gLongFired = false;
  } else if (down && !gLongFired && (now - gPressStart) >= kLongMs) {
    // Crossed the long-hold threshold.
    gLongFired = true;
    ev = Event::Long;
  } else if (!down && gDown) {
    // Released: a short tap if the long event never fired.
    if (!gLongFired) ev = Event::Short;
  }

  gDown = down;
  return ev;
}

float holdProgress() {
  if (!gDown) return 0.0f;
  float pct = (float)(nowMs() - gPressStart) / (float)kLongMs;
  if (pct < 0.0f) pct = 0.0f;
  if (pct > 1.0f) pct = 1.0f;
  return pct;
}

bool isDown() { return gDown; }

}  // namespace button
