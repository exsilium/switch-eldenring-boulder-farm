#include "ui/display.h"

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
lv_obj_t *gRunTitle = nullptr;
lv_obj_t *gRunHint = nullptr;
lv_obj_t *gRunBar = nullptr;

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

void buildRunScreen() {
  lv_obj_t *scr = lv_screen_active();
  gRun = lv_obj_create(scr);
  lv_obj_set_size(gRun, kHRes, kVRes);
  lv_obj_align(gRun, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_set_style_bg_color(gRun, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(gRun, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(gRun, 0, 0);
  lv_obj_set_style_pad_all(gRun, 8, 0);
  lv_obj_clear_flag(gRun, LV_OBJ_FLAG_SCROLLABLE);

  gRunTitle = lv_label_create(gRun);
  lv_obj_set_style_text_font(gRunTitle, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(gRunTitle, lv_palette_main(LV_PALETTE_GREEN), 0);
  lv_label_set_text(gRunTitle, "RUNNING");
  lv_obj_align(gRunTitle, LV_ALIGN_TOP_LEFT, 0, 6);

  gRunHint = lv_label_create(gRun);
  lv_obj_set_style_text_font(gRunHint, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(gRunHint, lv_color_hex(0xBBBBBB), 0);
  lv_label_set_text(gRunHint, "Tap: pause/resume\nHold: exit to menu");
  lv_obj_align(gRunHint, LV_ALIGN_TOP_LEFT, 0, 46);

  gRunBar = lv_bar_create(gRun);
  lv_obj_set_size(gRunBar, kHRes - 24, 12);
  lv_obj_align(gRunBar, LV_ALIGN_BOTTOM_MID, 0, -8);
  lv_bar_set_range(gRunBar, 0, 100);
  lv_bar_set_value(gRunBar, 0, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(gRunBar, lv_color_hex(0x222222), 0);
  lv_obj_set_style_bg_color(gRunBar, lv_palette_main(LV_PALETTE_YELLOW),
                            LV_PART_INDICATOR);

  lv_obj_add_flag(gRun, LV_OBJ_FLAG_HIDDEN);  // hidden until a run starts
}

void renderRun() {
  lv_label_set_text(gRunTitle, gRunPaused ? "PAUSED" : "RUNNING");
  lv_obj_set_style_text_color(
      gRunTitle,
      gRunPaused ? lv_palette_main(LV_PALETTE_AMBER)
                 : lv_palette_main(LV_PALETTE_GREEN),
      0);
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
  lv_bar_set_value(gRunBar, 0, LV_ANIM_OFF);
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

void setNote(const char *text) {
  if (!gLblPhase || !text) return;
  if (!lvgl_port_lock(0)) return;
  lv_label_set_text(gLblPhase, text);
  lv_obj_set_style_text_color(gLblPhase, lv_color_white(), 0);
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
