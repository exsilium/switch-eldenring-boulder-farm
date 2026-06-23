#include "display.h"

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <stdio.h>
#include <string.h>

namespace display {

namespace {
// Board pin macros (TFT_CS, TFT_DC, TFT_RST, TFT_BACKLITE, TFT_I2C_POWER) come
// from the Feather ESP32-S3 TFT variant.
Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

constexpr int16_t kBarX = 20;
constexpr int16_t kBarY = 118;
constexpr int16_t kBarW = 200;
constexpr int16_t kBarH = 13;

constexpr int16_t kDiagY = 88;   // diagnostics count line
constexpr int16_t kFlagY = 102;  // milestone flag row

constexpr uint16_t kGrey = 0x7BEF;  // RGB565 mid-grey (ST77XX has no DARKGREY)

AppState gLastState = AppState::DETACHING;  // force first paint
bool gStatePainted = false;
int16_t gLastFillW = -1;

// Diagnostics render cache.
bool gDiagActive = false;
uint32_t gDiagLastMs = 0;
uint32_t gLastIn = 0xFFFFFFFF;
uint32_t gLastOut = 0xFFFFFFFF;
uint8_t gLastSub = 0xFF;
uint8_t gLastFlags = 0xFF;

uint16_t stateColor(AppState s) {
  switch (s) {
    case AppState::IDLE_DETACHED:  return ST77XX_WHITE;
    case AppState::ATTACHING:      return ST77XX_YELLOW;
    case AppState::HANDSHAKING:    return ST77XX_ORANGE;
    case AppState::RUN_MACRO:      return ST77XX_CYAN;
    case AppState::CONNECTED_IDLE: return ST77XX_GREEN;
    case AppState::DETACHING:      return ST77XX_MAGENTA;
  }
  return ST77XX_WHITE;
}
}  // namespace

void begin() {
  // Power up the TFT rail and turn on the backlight.
  pinMode(TFT_I2C_POWER, OUTPUT);
  digitalWrite(TFT_I2C_POWER, HIGH);
  pinMode(TFT_BACKLITE, OUTPUT);
  digitalWrite(TFT_BACKLITE, HIGH);

  tft.init(135, 240);  // 240x135 ST7789
  tft.setRotation(3);  // landscape
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextWrap(false);
}

void showState(AppState state) {
  if (gStatePainted && state == gLastState) return;
  gLastState = state;
  gStatePainted = true;

  tft.fillScreen(ST77XX_BLACK);

  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(2);
  tft.setCursor(20, 18);
  tft.print("Pro Controller");

  tft.setTextColor(stateColor(state));
  tft.setTextSize(3);
  tft.setCursor(20, 55);
  tft.print(appStateName(state));

  tft.setTextColor(kGrey);
  tft.setTextSize(1);
  tft.setCursor(20, 90);
  if (state == AppState::IDLE_DETACHED) {
    tft.print("Hold BOOT 2s to attach");
  }

  // A fresh repaint invalidates the diagnostics + progress-bar caches so the
  // next live update redraws cleanly on the new background.
  gDiagActive = false;
  gLastIn = 0xFFFFFFFF;
  gLastOut = 0xFFFFFFFF;
  gLastSub = 0xFF;
  gLastFlags = 0xFF;
  gLastFillW = -1;
  tft.drawRect(kBarX, kBarY, kBarW, kBarH, kGrey);
}

void showDiag(bool mounted, uint32_t inCount, uint32_t outCount, uint8_t lastSub,
              bool devInfo, bool stickCal, bool setMode, bool vibration) {
  const uint8_t flags = (devInfo ? 0x01 : 0) | (stickCal ? 0x02 : 0) |
                        (setMode ? 0x04 : 0) | (vibration ? 0x08 : 0) |
                        (mounted ? 0x10 : 0);

  // Throttle to ~5 Hz; only repaint when something actually changed.
  const uint32_t now = millis();
  const bool changed = (inCount != gLastIn) || (outCount != gLastOut) ||
                       (lastSub != gLastSub) || (flags != gLastFlags);
  if (gDiagActive && (!changed || (now - gDiagLastMs) < 200)) return;
  gDiagActive = true;
  gDiagLastMs = now;
  gLastIn = inCount;
  gLastOut = outCount;
  gLastSub = lastSub;
  gLastFlags = flags;

  // USB activity line: mount state, IN/OUT counts and last subcommand.
  tft.fillRect(20, kDiagY, 220, 8, ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(mounted ? ST77XX_GREEN : kGrey);
  tft.setCursor(20, kDiagY);
  char buf[36];
  snprintf(buf, sizeof(buf), "%s in:%lu out:%lu s:%02X",
           mounted ? "MNT" : "---", (unsigned long)inCount,
           (unsigned long)outCount, lastSub);
  tft.print(buf);

  // Milestone flags: lit green when achieved, grey otherwise.
  static const char* kLabels[4] = {"DEV", "CAL", "MODE", "VIB"};
  tft.fillRect(20, kFlagY, 200, 8, ST77XX_BLACK);
  int16_t x = 20;
  for (int i = 0; i < 4; i++) {
    tft.setTextColor((flags & (1 << i)) ? ST77XX_GREEN : kGrey);
    tft.setCursor(x, kFlagY);
    tft.print(kLabels[i]);
    x += (int16_t)(strlen(kLabels[i]) * 6 + 8);
  }
}

void showHoldProgress(float pct) {
  if (pct < 0.0f) pct = 0.0f;
  if (pct > 1.0f) pct = 1.0f;

  int16_t fillW = (int16_t)((kBarW - 4) * pct);
  if (fillW == gLastFillW) return;
  gLastFillW = fillW;

  uint16_t color = (pct >= 1.0f) ? ST77XX_GREEN : ST77XX_YELLOW;
  tft.fillRect(kBarX + 2, kBarY + 2, fillW, kBarH - 4, color);
  tft.fillRect(kBarX + 2 + fillW, kBarY + 2, (kBarW - 4) - fillW, kBarH - 4,
               ST77XX_BLACK);
}

}  // namespace display
