#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>

// ---------------------------------------------------------------------------
// Adafruit Feather ESP32-S3 TFT
// Built-in 240x135 ST7789 TFT + BOOT button on GPIO0.
//
// Behavior:
//   - Hold the GPIO0 (BOOT) button.
//   - A progress bar fills on the LCD over HOLD_DURATION_MS.
//   - When the hold reaches HOLD_DURATION_MS, "Hello World" is printed
//     (Serial + on screen). This mimics a one-button menu "long press".
// ---------------------------------------------------------------------------

#define BUTTON_PIN 0          // GPIO0 / BOOT button (active LOW)
#define HOLD_DURATION_MS 2000 // Long-press threshold

// The board pin macros (TFT_CS, TFT_DC, TFT_RST, TFT_BACKLITE,
// TFT_I2C_POWER) are provided by the Feather ESP32-S3 TFT variant.
Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

// Progress bar geometry
const int16_t BAR_X = 20;
const int16_t BAR_W = 200;
const int16_t BAR_H = 28;
const int16_t BAR_Y = 80;

bool buttonWasDown = false;     // was the button held on the previous loop?
bool helloTriggered = false;    // did we already fire for this hold?
unsigned long pressStartMs = 0; // when the current hold began
int16_t lastFillW = -1;         // last drawn fill width (avoid redraw flicker)

void drawIdleScreen() {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextWrap(false);

  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(2);
  tft.setCursor(20, 20);
  tft.printf("Hold BOOT %ds", HOLD_DURATION_MS / 1000);

  tft.setTextColor(ST77XX_CYAN);
  tft.setTextSize(1);
  tft.setCursor(20, 50);
  tft.print("(GPIO0 button)");

  // Empty progress bar outline
  tft.drawRect(BAR_X, BAR_Y, BAR_W, BAR_H, ST77XX_WHITE);
  lastFillW = -1;
}

void drawProgress(float pct) {
  if (pct < 0) pct = 0;
  if (pct > 1) pct = 1;

  int16_t fillW = (int16_t)((BAR_W - 4) * pct);
  if (fillW == lastFillW) return; // nothing changed; skip redraw
  lastFillW = fillW;

  // Inner fill area (leave a 2px margin inside the outline)
  uint16_t color = (pct >= 1.0f) ? ST77XX_GREEN : ST77XX_YELLOW;
  tft.fillRect(BAR_X + 2, BAR_Y + 2, fillW, BAR_H - 4, color);
  // Clear the remaining portion so a shorter bar doesn't leave residue
  tft.fillRect(BAR_X + 2 + fillW, BAR_Y + 2,
               (BAR_W - 4) - fillW, BAR_H - 4, ST77XX_BLACK);
}

void showHelloWorld() {
  Serial.println("Hello World");

  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_GREEN);
  tft.setTextSize(3);
  tft.setCursor(15, 55);
  tft.print("Hello World");
}

void setup() {
  Serial.begin(115200);

  pinMode(BUTTON_PIN, INPUT_PULLUP); // BOOT button reads LOW when pressed

  // Power up the TFT rail and turn on the backlight.
  pinMode(TFT_I2C_POWER, OUTPUT);
  digitalWrite(TFT_I2C_POWER, HIGH);
  pinMode(TFT_BACKLITE, OUTPUT);
  digitalWrite(TFT_BACKLITE, HIGH);

  // 240x135 ST7789, landscape orientation.
  tft.init(135, 240);
  tft.setRotation(3);

  drawIdleScreen();
}

void loop() {
  bool buttonDown = (digitalRead(BUTTON_PIN) == LOW);

  if (buttonDown && !buttonWasDown) {
    // Button just pressed: start timing this hold.
    pressStartMs = millis();
    helloTriggered = false;
    drawIdleScreen();
  }

  if (buttonDown) {
    unsigned long held = millis() - pressStartMs;
    drawProgress((float)held / (float)HOLD_DURATION_MS);

    if (!helloTriggered && held >= HOLD_DURATION_MS) {
      helloTriggered = true;
      showHelloWorld();
    }
  } else if (buttonWasDown) {
    // Button released: reset back to idle unless we already fired.
    if (!helloTriggered) {
      drawIdleScreen();
    }
  }

  buttonWasDown = buttonDown;
  delay(10);
}