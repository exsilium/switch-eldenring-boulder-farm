#include "ui/display.h"

#include <string.h>

#include "procon/procon_reports.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

// DISPLAY_DEBUG: when 1, run the slow (~3.5s) color self-test at boot and the
// NeoPixel stage markers. When 0 (default) the display comes up immediately so
// it can show breadcrumbs from the Switch enumeration as early as possible.
#ifndef DISPLAY_DEBUG
#define DISPLAY_DEBUG 0
#endif

namespace ui {

namespace {
// ---- Board pins (Adafruit Feather ESP32-S3 TFT) --------------------------
constexpr gpio_num_t kPinTftPower = GPIO_NUM_21;  // TFT_I2C_POWER (HIGH = on)
constexpr gpio_num_t kPinBacklite = GPIO_NUM_45;  // TFT_BACKLITE (HIGH = on)
constexpr gpio_num_t kPinSclk = GPIO_NUM_36;
constexpr gpio_num_t kPinMosi = GPIO_NUM_35;
constexpr gpio_num_t kPinCs = GPIO_NUM_7;
constexpr gpio_num_t kPinDc = GPIO_NUM_39;
constexpr gpio_num_t kPinRst = GPIO_NUM_40;

// ---- Panel geometry (240x135 landscape) ----------------------------------
// NOTE: kVRes is intentionally 136 (one more than the physical 135) so the
// extra row at the gap boundary covers a stray 1px line on hardware. With
// kGapY=53 this lands cleanly; do NOT "fix" it back to 135. If the image is
// shifted or mirrored on other hardware, tune kGapX/kGapY/the mirror() args.
constexpr int kHRes = 240;
constexpr int kVRes = 136;
constexpr int kGapX = 40;
constexpr int kGapY = 53;

constexpr spi_host_device_t kSpiHost = SPI2_HOST;

// ---- LVGL objects --------------------------------------------------------
lv_obj_t *gLblPhase = nullptr;
lv_obj_t *gLblCounts = nullptr;
lv_obj_t *gFlag[4] = {nullptr, nullptr, nullptr, nullptr};
lv_obj_t *gBar = nullptr;

lv_obj_t *gMenu = nullptr;     // full-screen menu overlay (hidden by default)
lv_obj_t *gMenuRow[3] = {nullptr, nullptr, nullptr};

lv_obj_t *gRun = nullptr;       // full-screen "running" overlay (hidden by default)
lv_obj_t *gRunTitle = nullptr;  // play / pause glyph (cassette-style)
lv_obj_t *gRunStatus = nullptr; // macro-written status label (e.g. Death Detected)
lv_obj_t *gRunBar = nullptr;

// ---- Per-side rumble meters (on the RUNNING overlay) ---------------------
lv_obj_t *gRumL = nullptr;     // left (RUMBLE_A) amplitude bar
lv_obj_t *gRumR = nullptr;     // right (RUMBLE_B) amplitude bar
lv_obj_t *gRumLVal = nullptr;  // left run-max numeric label
lv_obj_t *gRumRVal = nullptr;  // right run-max numeric label
uint16_t gPrevRumL = 0xFFFF, gPrevRumR = 0xFFFF;  // last pushed, for repaint-on-change
uint16_t gMaxRumL = 0, gMaxRumR = 0;              // highest seen this run
bool gRumLHot = false, gRumRHot = false;          // last threshold state

// ---- Live Pro Controller diagram (on the RUNNING overlay) ----------------
// Each drawn digital input maps to a bit in the standard input report's three
// button bytes (see procon::Input / macro::Channel). When that bit is set the
// chip is highlighted; the analog sticks are shown as dots that move.
struct CtrlBtn {
  lv_obj_t *box;
  lv_obj_t *lbl;    // may be null (D-pad / stick rings have no label)
  uint8_t byteIdx;  // index into buttons[3]
  uint8_t mask;     // bit within that byte
};
constexpr int kCtrlMax = 24;
CtrlBtn gCtrl[kCtrlMax];
int gCtrlCount = 0;

lv_obj_t *gPad = nullptr;   // container holding the controller diagram
lv_obj_t *gLDot = nullptr;  // left analog stick dot
lv_obj_t *gRDot = nullptr;  // right analog stick dot

// Fixed centres (within gPad) of the two analog stick rings; the dots rest here.
constexpr int kLCx = 34, kLCy = 60;
constexpr int kRCx = 150, kRCy = 86;

// Last pushed state, so setControllerState() only repaints on a change.
uint8_t gPrevBtn[3] = {0xFF, 0xFF, 0xFF};
uint16_t gPrevLx = 0xFFFF, gPrevLy = 0xFFFF, gPrevRx = 0xFFFF, gPrevRy = 0xFFFF;

constexpr int kMenuCount = 3;
const char *kMenuLabels[kMenuCount] = {"Run Boulder", "Reattach USB", "Back"};
const char *kFlagLabels[4] = {"DEV", "CAL", "MODE", "VIB"};

enum class View { Status, Menu, Running };
View gView = View::Status;
int gSel = 0;
bool gRunPaused = false;

volatile Command gPending = Command::None;

lv_color_t phaseColor(Phase p) {
  switch (p) {
    case Phase::Detached:  return lv_color_hex(0xBBBBBB);
    case Phase::Mounted:   return lv_palette_main(LV_PALETTE_BLUE);
    case Phase::Handshake: return lv_palette_main(LV_PALETTE_AMBER);
    case Phase::Ready:     return lv_palette_main(LV_PALETTE_GREEN);
  }
  return lv_color_white();
}

const char *phaseName(Phase p) {
  switch (p) {
    case Phase::Detached:  return "DETACHED";
    case Phase::Mounted:   return "MOUNTED";
    case Phase::Handshake: return "HANDSHAKE";
    case Phase::Ready:     return "READY";
  }
  return "?";
}

// ---- LCD bring-up --------------------------------------------------------
void lcdInit(esp_lcd_panel_io_handle_t *io, esp_lcd_panel_handle_t *panel) {
  gpio_config_t pwr = {};
  pwr.pin_bit_mask = (1ULL << kPinTftPower) | (1ULL << kPinBacklite);
  pwr.mode = GPIO_MODE_OUTPUT;
  gpio_config(&pwr);
  gpio_set_level(kPinTftPower, 1);
  gpio_set_level(kPinBacklite, 1);

  spi_bus_config_t bus = {};
  bus.sclk_io_num = kPinSclk;
  bus.mosi_io_num = kPinMosi;
  bus.miso_io_num = -1;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = kHRes * kVRes * 2;
  ESP_ERROR_CHECK(spi_bus_initialize(kSpiHost, &bus, SPI_DMA_CH_AUTO));

  esp_lcd_panel_io_spi_config_t io_cfg = {};
  io_cfg.dc_gpio_num = kPinDc;
  io_cfg.cs_gpio_num = kPinCs;
  io_cfg.pclk_hz = 20 * 1000 * 1000;  // 20 MHz: 40 MHz was unstable on this board
  io_cfg.lcd_cmd_bits = 8;
  io_cfg.lcd_param_bits = 8;
  io_cfg.spi_mode = 0;
  io_cfg.trans_queue_depth = 10;
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
      (esp_lcd_spi_bus_handle_t)kSpiHost, &io_cfg, io));

  esp_lcd_panel_dev_config_t panel_cfg = {};
  panel_cfg.reset_gpio_num = kPinRst;
  panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
  panel_cfg.bits_per_pixel = 16;
  ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(*io, &panel_cfg, panel));

  ESP_ERROR_CHECK(esp_lcd_panel_reset(*panel));
  ESP_ERROR_CHECK(esp_lcd_panel_init(*panel));
  ESP_ERROR_CHECK(esp_lcd_panel_invert_color(*panel, true));  // ST7789 needs invert
  ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(*panel, true));       // landscape
  ESP_ERROR_CHECK(esp_lcd_panel_mirror(*panel, true, false));
  ESP_ERROR_CHECK(esp_lcd_panel_set_gap(*panel, kGapX, kGapY));
  ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(*panel, true));
}

// ---- Panel self-test (direct esp_lcd, bypasses LVGL) ---------------------
// Fills the whole panel with solid colors so we can confirm the panel, SPI,
// orientation and gap are correct independently of LVGL. Colors are byte-
// swapped to match the panel's MSB-first RGB565 order (same as LVGL
// swap_bytes=true). Sequence: WHITE, BLACK, RED, GREEN, BLUE.
// TODO: remove once the display is confirmed working on hardware.
#if DISPLAY_DEBUG
void selfTest(esp_lcd_panel_handle_t panel, SelfTestStepFn onStep) {
  static uint16_t line[kHRes];
  const uint16_t seq[] = {0xFFFF, 0x0000, 0xF800, 0x07E0, 0x001F};
  int idx = 0;
  for (uint16_t c : seq) {
    if (onStep) onStep(idx);
    const uint16_t v = (uint16_t)((c >> 8) | (c << 8));
    for (int i = 0; i < kHRes; i++) line[i] = v;
    for (int y = 0; y < kVRes; y++) {
      esp_lcd_panel_draw_bitmap(panel, 0, y, kHRes, y + 1, line);
    }
    vTaskDelay(pdMS_TO_TICKS(700));
    idx++;
  }
}
#endif

// ---- UI construction -----------------------------------------------------
void buildStatusScreen() {
  lv_obj_t *scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

  gLblPhase = lv_label_create(scr);
  lv_obj_set_style_text_font(gLblPhase, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(gLblPhase, lv_color_white(), 0);
  lv_label_set_text(gLblPhase, "DETACHED");
  lv_obj_align(gLblPhase, LV_ALIGN_TOP_LEFT, 6, 6);

  gLblCounts = lv_label_create(scr);
  lv_obj_set_style_text_font(gLblCounts, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(gLblCounts, lv_color_hex(0xBBBBBB), 0);
  lv_label_set_text(gLblCounts, "--- in:0 out:0 s:00");
  lv_obj_align(gLblCounts, LV_ALIGN_TOP_LEFT, 6, 48);

  int x = 6;
  for (int i = 0; i < 4; i++) {
    gFlag[i] = lv_label_create(scr);
    lv_obj_set_style_text_font(gFlag[i], &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(gFlag[i], lv_color_hex(0x666666), 0);
    lv_label_set_text(gFlag[i], kFlagLabels[i]);
    lv_obj_align(gFlag[i], LV_ALIGN_TOP_LEFT, x, 74);
    x += (int)(lv_strlen(kFlagLabels[i]) * 11 + 14);
  }

  gBar = lv_bar_create(scr);
  lv_obj_set_size(gBar, kHRes - 24, 12);
  lv_obj_align(gBar, LV_ALIGN_BOTTOM_MID, 0, -8);
  lv_bar_set_range(gBar, 0, 100);
  lv_bar_set_value(gBar, 0, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(gBar, lv_color_hex(0x222222), 0);
  lv_obj_set_style_bg_color(gBar, lv_palette_main(LV_PALETTE_YELLOW),
                            LV_PART_INDICATOR);
}

void buildMenu() {
  lv_obj_t *scr = lv_screen_active();
  gMenu = lv_obj_create(scr);
  lv_obj_set_size(gMenu, kHRes, kVRes);
  lv_obj_align(gMenu, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_set_style_bg_color(gMenu, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(gMenu, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(gMenu, 0, 0);
  lv_obj_set_style_pad_all(gMenu, 8, 0);
  lv_obj_clear_flag(gMenu, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *title = lv_label_create(gMenu);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(title, lv_color_white(), 0);
  lv_label_set_text(title, "MENU");
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

  for (int i = 0; i < kMenuCount; i++) {
    gMenuRow[i] = lv_label_create(gMenu);
    lv_obj_set_style_text_font(gMenuRow[i], &lv_font_montserrat_20, 0);
    lv_label_set_text(gMenuRow[i], kMenuLabels[i]);
    lv_obj_align(gMenuRow[i], LV_ALIGN_TOP_LEFT, 12, 28 + i * 26);
  }

  lv_obj_add_flag(gMenu, LV_OBJ_FLAG_HIDDEN);  // hidden until opened
}

// ---- Controller diagram helpers ------------------------------------------
void chipStyle(lv_obj_t *o, int radius) {
  lv_obj_set_style_pad_all(o, 0, 0);
  lv_obj_set_style_radius(o, radius, 0);
  lv_obj_set_style_border_width(o, 1, 0);
  lv_obj_set_style_border_color(o, lv_color_hex(0x555555), 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

// Create one button chip on gPad and register it against a report bit.
lv_obj_t *addBtn(lv_obj_t *parent, int x, int y, int w, int h, int radius,
                 const lv_font_t *font, const char *txt, uint8_t byteIdx,
                 uint8_t mask) {
  lv_obj_t *box = lv_obj_create(parent);
  lv_obj_set_size(box, w, h);
  lv_obj_set_pos(box, x, y);
  chipStyle(box, radius);
  lv_obj_t *lbl = nullptr;
  if (txt && txt[0]) {
    lbl = lv_label_create(box);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x999999), 0);
    lv_label_set_text(lbl, txt);
    lv_obj_center(lbl);
  }
  if (gCtrlCount < kCtrlMax) gCtrl[gCtrlCount++] = {box, lbl, byteIdx, mask};
  return box;
}

void applyPressed(const CtrlBtn &c, bool pressed) {
  if (pressed) {
    lv_obj_set_style_bg_opa(c.box, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(c.box, lv_palette_main(LV_PALETTE_GREEN), 0);
    lv_obj_set_style_border_color(c.box, lv_palette_main(LV_PALETTE_GREEN), 0);
    if (c.lbl) lv_obj_set_style_text_color(c.lbl, lv_color_black(), 0);
  } else {
    lv_obj_set_style_bg_opa(c.box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(c.box, lv_color_hex(0x555555), 0);
    if (c.lbl) lv_obj_set_style_text_color(c.lbl, lv_color_hex(0x999999), 0);
  }
}

lv_obj_t *makeDot(lv_obj_t *parent, int cx, int cy) {
  lv_obj_t *dot = lv_obj_create(parent);
  lv_obj_set_size(dot, 12, 12);
  lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(dot, lv_color_hex(0x888888), 0);
  lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(dot, 0, 0);
  lv_obj_set_style_pad_all(dot, 0, 0);
  lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_pos(dot, cx - 6, cy - 6);
  return dot;
}

void moveDot(lv_obj_t *dot, int cx, int cy, uint16_t x, uint16_t y) {
  if (!dot) return;
  const int kMax = 13;  // max dot travel from centre, in pixels
  const int dx = ((int)x - (int)procon::kStickCenter) * kMax / procon::kStickCenter;
  const int dy = ((int)y - (int)procon::kStickCenter) * kMax / procon::kStickCenter;
  lv_obj_set_pos(dot, cx - 6 + dx, cy - 6 - dy);  // screen Y is inverted (up = -)
}

void buildRunScreen() {
  lv_obj_t *scr = lv_screen_active();
  gRun = lv_obj_create(scr);
  lv_obj_set_size(gRun, kHRes, kVRes);
  lv_obj_align(gRun, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_set_style_bg_color(gRun, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(gRun, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(gRun, 0, 0);
  lv_obj_set_style_pad_all(gRun, 0, 0);
  lv_obj_clear_flag(gRun, LV_OBJ_FLAG_SCROLLABLE);

  gRunTitle = lv_label_create(gRun);
  lv_obj_set_style_text_font(gRunTitle, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(gRunTitle, lv_palette_main(LV_PALETTE_GREEN), 0);
  lv_label_set_text(gRunTitle, LV_SYMBOL_PLAY);  // cassette-style play glyph
  lv_obj_align(gRunTitle, LV_ALIGN_TOP_LEFT, 6, 1);

  // Macro-written status label (e.g. "Death Detected"), to the right of the
  // play/pause glyph so a longer message has room along the top row.
  gRunStatus = lv_label_create(gRun);
  lv_obj_set_style_text_font(gRunStatus, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(gRunStatus, lv_palette_main(LV_PALETTE_RED), 0);
  lv_label_set_text(gRunStatus, "");
  lv_obj_align(gRunStatus, LV_ALIGN_TOP_LEFT, 26, 1);

  // Controller diagram area (own container so child coords start at 0,0).
  gPad = lv_obj_create(gRun);
  lv_obj_set_size(gPad, kHRes, 112);
  lv_obj_set_pos(gPad, 0, 16);
  lv_obj_set_style_bg_opa(gPad, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(gPad, 0, 0);
  lv_obj_set_style_pad_all(gPad, 0, 0);
  lv_obj_clear_flag(gPad, LV_OBJ_FLAG_SCROLLABLE);

  const lv_font_t *f12 = &lv_font_montserrat_12;
  const lv_font_t *f14 = &lv_font_montserrat_14;

  gCtrlCount = 0;
  // Shoulder / trigger buttons (byte 2: L/ZL, byte 0: R/ZR). Shifted 8px inward
  // from the screen edges to leave room for the vertical rumble meters.
  addBtn(gPad, 10, 0, 46, 14, 3, f12, "ZL", 2, 0x80);
  addBtn(gPad, 10, 16, 46, 14, 3, f12, "L", 2, 0x40);
  addBtn(gPad, 184, 0, 46, 14, 3, f12, "ZR", 0, 0x80);
  addBtn(gPad, 184, 16, 46, 14, 3, f12, "R", 0, 0x40);
  // Left analog stick ring (click = byte 1 0x08).
  addBtn(gPad, 12, 38, 44, 44, LV_RADIUS_CIRCLE, f12, nullptr, 1, 0x08);
  // D-pad (byte 2).
  addBtn(gPad, 84, 63, 15, 15, 2, f12, nullptr, 2, 0x02);   // Up
  addBtn(gPad, 84, 93, 15, 15, 2, f12, nullptr, 2, 0x01);   // Down
  addBtn(gPad, 66, 78, 15, 15, 2, f12, nullptr, 2, 0x08);   // Left
  addBtn(gPad, 102, 78, 15, 15, 2, f12, nullptr, 2, 0x04);  // Right
  // Centre / system buttons (byte 1).
  addBtn(gPad, 104, 10, 16, 16, LV_RADIUS_CIRCLE, f12, "-", 1, 0x01);  // Minus
  addBtn(gPad, 142, 10, 16, 16, LV_RADIUS_CIRCLE, f12, "+", 1, 0x02);  // Plus
  addBtn(gPad, 104, 32, 16, 16, LV_RADIUS_CIRCLE, f12, "C", 1, 0x20);  // Capture
  addBtn(gPad, 142, 32, 16, 16, LV_RADIUS_CIRCLE, f12, "H", 1, 0x10);  // Home
  // Right analog stick ring (click = byte 1 0x04).
  addBtn(gPad, 129, 65, 42, 42, LV_RADIUS_CIRCLE, f12, nullptr, 1, 0x04);
  // Right cluster A/B/X/Y diamond (byte 0).
  addBtn(gPad, 196, 30, 20, 20, LV_RADIUS_CIRCLE, f14, "X", 0, 0x02);
  addBtn(gPad, 178, 48, 20, 20, LV_RADIUS_CIRCLE, f14, "Y", 0, 0x01);
  addBtn(gPad, 214, 48, 20, 20, LV_RADIUS_CIRCLE, f14, "A", 0, 0x08);
  addBtn(gPad, 196, 66, 20, 20, LV_RADIUS_CIRCLE, f14, "B", 0, 0x04);

  // Stick dots last so they render on top of their rings.
  gLDot = makeDot(gPad, kLCx, kLCy);
  gRDot = makeDot(gPad, kRCx, kRCy);

  // Hold-progress / status bar along the very bottom.
  gRunBar = lv_bar_create(gRun);
  lv_obj_set_size(gRunBar, kHRes - 24, 6);
  lv_obj_align(gRunBar, LV_ALIGN_BOTTOM_MID, 0, -1);
  lv_bar_set_range(gRunBar, 0, 100);
  lv_bar_set_value(gRunBar, 0, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(gRunBar, lv_color_hex(0x222222), 0);
  lv_obj_set_style_bg_color(gRunBar, lv_palette_main(LV_PALETTE_YELLOW),
                            LV_PART_INDICATOR);

  // Per-side rumble meters: thin vertical bars hugging the left/right edges
  // (children of gRun so they paint over the pad). 0..255 amplitude; the
  // indicator flips to red once it crosses procon::kRumbleMin. Run-max value is
  // printed near each bar. The shoulder buttons were shifted inward to clear
  // these columns.
  auto makeRumbleBar = [&](int x) {
    lv_obj_t *b = lv_bar_create(gRun);
    lv_obj_set_size(b, 5, 84);
    lv_obj_set_pos(b, x, 20);
    lv_bar_set_range(b, 0, 255);
    lv_bar_set_value(b, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x222222), 0);
    lv_obj_set_style_bg_color(b, lv_palette_main(LV_PALETTE_GREEN),
                              LV_PART_INDICATOR);
    return b;
  };
  gRumL = makeRumbleBar(0);
  gRumR = makeRumbleBar(kHRes - 5);

  auto makeRumbleVal = [&](int x) {
    lv_obj_t *l = lv_label_create(gRun);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xBBBBBB), 0);
    lv_label_set_text(l, "0");
    lv_obj_set_pos(l, x, 106);
    return l;
  };
  gRumLVal = makeRumbleVal(2);
  gRumRVal = makeRumbleVal(kHRes - 28);

  lv_obj_add_flag(gRun, LV_OBJ_FLAG_HIDDEN);  // hidden until a run starts
}

void renderRun() {
  // Cassette-style play / pause glyph, keeping the green (running) / amber
  // (paused) colour coding that the RUNNING/PAUSED words used to carry.
  lv_label_set_text(gRunTitle, gRunPaused ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
  lv_obj_set_style_text_color(
      gRunTitle,
      gRunPaused ? lv_palette_main(LV_PALETTE_AMBER)
                 : lv_palette_main(LV_PALETTE_GREEN),
      0);
}

// Reset the rumble meters + run-max readouts to zero (on run start / pause).
void resetRumbleMeters() {
  gPrevRumL = gPrevRumR = 0xFFFF;
  gMaxRumL = gMaxRumR = 0;
  gRumLHot = gRumRHot = false;
  if (gRumL) {
    lv_bar_set_value(gRumL, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(gRumL, lv_palette_main(LV_PALETTE_GREEN),
                              LV_PART_INDICATOR);
  }
  if (gRumR) {
    lv_bar_set_value(gRumR, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(gRumR, lv_palette_main(LV_PALETTE_GREEN),
                              LV_PART_INDICATOR);
  }
  if (gRumLVal) lv_label_set_text(gRumLVal, "0");
  if (gRumRVal) lv_label_set_text(gRumRVal, "0");
}

void renderMenuSelection() {
  for (int i = 0; i < kMenuCount; i++) {
    bool sel = (i == gSel);
    lv_obj_set_style_text_color(
        gMenuRow[i], sel ? lv_palette_main(LV_PALETTE_GREEN) : lv_color_hex(0xBBBBBB),
        0);
    lv_label_set_text_fmt(gMenuRow[i], "%s %s", sel ? ">" : " ", kMenuLabels[i]);
  }
}

void openMenu() {
  gView = View::Menu;
  gSel = 0;
  renderMenuSelection();
  lv_obj_clear_flag(gMenu, LV_OBJ_FLAG_HIDDEN);
}

void closeMenu() {
  gView = View::Status;
  lv_obj_add_flag(gMenu, LV_OBJ_FLAG_HIDDEN);
}

void openRun() {
  gView = View::Running;
  gRunPaused = false;
  renderRun();
  if (gRunStatus) lv_label_set_text(gRunStatus, "");  // clear stale status
  resetRumbleMeters();
  lv_bar_set_value(gRunBar, 0, LV_ANIM_OFF);
  // Force the next controller push to repaint every chip from a clean slate.
  gPrevBtn[0] = gPrevBtn[1] = gPrevBtn[2] = 0xFF;
  gPrevLx = gPrevLy = gPrevRx = gPrevRy = 0xFFFF;
  lv_obj_add_flag(gMenu, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(gRun, LV_OBJ_FLAG_HIDDEN);
}

void activateMenu() {
  switch (gSel) {
    case 0: gPending = Command::RunMacro; openRun(); break;
    case 1: gPending = Command::Reattach; closeMenu(); break;
    case 2: closeMenu(); break;
    default: break;
  }
}
}  // namespace

void begin(SelfTestStepFn onSelfTestStep) {
  esp_lcd_panel_io_handle_t io = nullptr;
  esp_lcd_panel_handle_t panel = nullptr;
  lcdInit(&io, &panel);

#if DISPLAY_DEBUG
  selfTest(panel, onSelfTestStep);  // visual sanity check before LVGL takes over

  // Stage markers (temporary) so the NeoPixel shows where begin() hangs:
  //   5 -> before lvgl_port_init, 6 -> before add_disp,
  //   7 -> before build, 8 -> begin() complete.
  if (onSelfTestStep) onSelfTestStep(5);
#else
  (void)onSelfTestStep;
#endif
  lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
  // Pin the LVGL render task to core 1 so the TinyUSB stack (core 0, matching
  // the working reference) owns core 0 for enumeration without sharing it with
  // the display renderer.
  port_cfg.task_affinity = 1;
  ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

#if DISPLAY_DEBUG
  if (onSelfTestStep) onSelfTestStep(6);
#endif
  lvgl_port_display_cfg_t disp_cfg = {};
  disp_cfg.io_handle = io;
  disp_cfg.panel_handle = panel;
  disp_cfg.buffer_size = kHRes * kVRes;
  disp_cfg.double_buffer = false;
  disp_cfg.hres = kHRes;
  disp_cfg.vres = kVRes;
  disp_cfg.monochrome = false;
  disp_cfg.color_format = LV_COLOR_FORMAT_RGB565;
  // esp_lvgl_port re-applies swap_xy/mirror on every flush from this rotation
  // config, so it MUST match what lcdInit() set (else the panel reverts to
  // portrait and the UI renders sideways into a narrow strip).
  disp_cfg.rotation.swap_xy = true;
  disp_cfg.rotation.mirror_x = true;
  disp_cfg.rotation.mirror_y = false;
  disp_cfg.flags.buff_dma = true;
  disp_cfg.flags.swap_bytes = true;  // ST7789 over SPI expects byte-swapped RGB565
  lvgl_port_add_disp(&disp_cfg);

#if DISPLAY_DEBUG
  if (onSelfTestStep) onSelfTestStep(7);
#endif
  if (lvgl_port_lock(0)) {
    buildStatusScreen();
    buildMenu();
    buildRunScreen();
    lvgl_port_unlock();
  }
#if DISPLAY_DEBUG
  if (onSelfTestStep) onSelfTestStep(8);
#endif
}

void setStatus(Phase phase, bool mounted, uint32_t in, uint32_t out,
               uint8_t lastSub, bool dev, bool cal, bool mode, bool vib) {
  if (!gLblPhase) return;
  if (!lvgl_port_lock(0)) return;

  lv_label_set_text(gLblPhase, phaseName(phase));
  lv_obj_set_style_text_color(gLblPhase, phaseColor(phase), 0);

  lv_label_set_text_fmt(gLblCounts, "%s in:%u out:%u s:%02X",
                        mounted ? "MNT" : "---", (unsigned)in, (unsigned)out,
                        lastSub);
  lv_obj_set_style_text_color(
      gLblCounts, mounted ? lv_palette_main(LV_PALETTE_GREEN) : lv_color_hex(0xBBBBBB),
      0);

  const bool flags[4] = {dev, cal, mode, vib};
  for (int i = 0; i < 4; i++) {
    lv_obj_set_style_text_color(
        gFlag[i],
        flags[i] ? lv_palette_main(LV_PALETTE_GREEN) : lv_color_hex(0x666666), 0);
  }

  lvgl_port_unlock();
}

void setHoldProgress(float pct) {
  if (!gBar) return;
  if (pct < 0.0f) pct = 0.0f;
  if (pct > 1.0f) pct = 1.0f;
  if (!lvgl_port_lock(0)) return;
  const int v = (int)(pct * 100);
  const lv_color_t col = pct >= 1.0f ? lv_palette_main(LV_PALETTE_GREEN)
                                     : lv_palette_main(LV_PALETTE_YELLOW);
  lv_bar_set_value(gBar, v, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(gBar, col, LV_PART_INDICATOR);
  if (gRunBar) {
    lv_bar_set_value(gRunBar, v, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(gRunBar, col, LV_PART_INDICATOR);
  }
  lvgl_port_unlock();
}

void setControllerState(const uint8_t buttons[3], uint16_t lx, uint16_t ly,
                        uint16_t rx, uint16_t ry) {
  if (!gPad) return;
  if (gView != View::Running) return;  // only meaningful on the RUNNING overlay
  // Skip the (relatively expensive) LVGL work when nothing changed.
  if (buttons[0] == gPrevBtn[0] && buttons[1] == gPrevBtn[1] &&
      buttons[2] == gPrevBtn[2] && lx == gPrevLx && ly == gPrevLy &&
      rx == gPrevRx && ry == gPrevRy) {
    return;
  }
  if (!lvgl_port_lock(0)) return;
  for (int i = 0; i < gCtrlCount; i++) {
    const bool pressed = (buttons[gCtrl[i].byteIdx] & gCtrl[i].mask) != 0;
    applyPressed(gCtrl[i], pressed);
  }
  moveDot(gLDot, kLCx, kLCy, lx, ly);
  moveDot(gRDot, kRCx, kRCy, rx, ry);
  gPrevBtn[0] = buttons[0];
  gPrevBtn[1] = buttons[1];
  gPrevBtn[2] = buttons[2];
  gPrevLx = lx;
  gPrevLy = ly;
  gPrevRx = rx;
  gPrevRy = ry;
  lvgl_port_unlock();
}

void setNote(const char *text) {
  if (!gLblPhase || !text) return;
  if (!lvgl_port_lock(0)) return;
  lv_label_set_text(gLblPhase, text);
  lv_obj_set_style_text_color(gLblPhase, lv_color_white(), 0);
  lvgl_port_unlock();
}

void setRunStatus(const char *text) {
  if (!gRunStatus) return;
  if (gView != View::Running) return;  // no-op unless the RUNNING overlay is up
  // Small fixed buffer so the UI task never churns the heap; repaint on change.
  static char buf[24] = {0};
  char next[24];
  size_t i = 0;
  if (text) {
    for (; text[i] && i < sizeof(next) - 1; i++) next[i] = text[i];
  }
  next[i] = '\0';
  if (strncmp(next, buf, sizeof(buf)) == 0) return;
  if (!lvgl_port_lock(0)) return;
  memcpy(buf, next, sizeof(next));
  lv_label_set_text(gRunStatus, buf);
  lvgl_port_unlock();
}

void setRumble(uint16_t left, uint16_t right) {
  if (!gRumL || !gRumR) return;
  if (gView != View::Running) return;  // no-op unless the RUNNING overlay is up
  if (left > 255) left = 255;
  if (right > 255) right = 255;
  bool maxChanged = false;
  if (left > gMaxRumL) {
    gMaxRumL = left;
    maxChanged = true;
  }
  if (right > gMaxRumR) {
    gMaxRumR = right;
    maxChanged = true;
  }
  if (left == gPrevRumL && right == gPrevRumR && !maxChanged) return;
  if (!lvgl_port_lock(0)) return;
  const bool lHot = left >= procon::kRumbleMin;
  const bool rHot = right >= procon::kRumbleMin;
  lv_bar_set_value(gRumL, left, LV_ANIM_OFF);
  lv_bar_set_value(gRumR, right, LV_ANIM_OFF);
  if (lHot != gRumLHot) {
    lv_obj_set_style_bg_color(gRumL,
                              lHot ? lv_palette_main(LV_PALETTE_RED)
                                   : lv_palette_main(LV_PALETTE_GREEN),
                              LV_PART_INDICATOR);
    gRumLHot = lHot;
  }
  if (rHot != gRumRHot) {
    lv_obj_set_style_bg_color(gRumR,
                              rHot ? lv_palette_main(LV_PALETTE_RED)
                                   : lv_palette_main(LV_PALETTE_GREEN),
                              LV_PART_INDICATOR);
    gRumRHot = rHot;
  }
  if (maxChanged) {
    lv_label_set_text_fmt(gRumLVal, "%u", (unsigned)gMaxRumL);
    lv_label_set_text_fmt(gRumRVal, "%u", (unsigned)gMaxRumR);
  }
  gPrevRumL = left;
  gPrevRumR = right;
  lvgl_port_unlock();
}

void onButton(button::Event e) {
  if (e == button::Event::None) return;
  if (!lvgl_port_lock(0)) return;
  if (gView == View::Status) {
    if (e == button::Event::Long) openMenu();
  } else if (gView == View::Menu) {
    if (e == button::Event::Short) {
      gSel = (gSel + 1) % kMenuCount;
      renderMenuSelection();
    } else if (e == button::Event::Long) {
      activateMenu();
    }
  } else {  // Running
    if (e == button::Event::Short) {
      // Toggle pause/resume; the app layer mirrors this on the macro.
      gRunPaused = !gRunPaused;
      renderRun();
      resetRumbleMeters();  // pausing resets the run-max readouts
      gPending = Command::TogglePause;
    } else if (e == button::Event::Long) {
      // Hold exits the run and returns to the menu we came from.
      gPending = Command::StopMacro;
      lv_obj_add_flag(gRun, LV_OBJ_FLAG_HIDDEN);
      openMenu();
    }
  }
  lvgl_port_unlock();
}

Command takeCommand() {
  Command c = gPending;
  gPending = Command::None;
  return c;
}

}  // namespace ui
