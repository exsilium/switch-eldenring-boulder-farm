// Switch Pro Controller emulation — Adafruit Feather ESP32-S3 TFT (ESP-IDF).
//
// Migrated from the Arduino framework to ESP-IDF + raw TinyUSB after validation
// proved the Arduino composite USB stack was the blocker (the Switch never sent
// OUT reports / never drove the handshake). This build uses the real
// procon::Protocol (byte-matched to finger563/esp-usb-ble-hid) over raw
// esp_tinyusb so the console accepts the controller.
//
// No serial console is possible (the single USB-C port is the USB-OTG port), so
// status is shown on the onboard NeoPixel:
//   RED    = not mounted
//   BLUE   = mounted, handshake not started
//   YELLOW = handshake underway (device info queried)
//   GREEN  = standard input mode -> controller accepted; the input macro runs
//
// Phase 2 will add the LVGL TFT UI (one-button menus); Phase 3 the boulder-farm
// macro. The legacy Arduino TFT/button glue is kept under legacy-arduino/ for
// reference, and esp-usb-ble-hid/ remains the protocol reference.

#include <string.h>

#include <array>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "led_strip.h"

#include "tinyusb.h"
#include "tusb.h"

// Upstream Switch Pro HID report descriptor generator (espp/hid-rp managed
// component). Using it directly means the report descriptor tracks any future
// upstream changes instead of being a hand-maintained byte array.
#include "hid-rp-switch-pro.hpp"

#include "procon/procon_protocol.h"
#include "macros/boulder_macro.h"
#include "ui/display.h"
#include "ui/button.h"

static const char *TAG = "procon";

// ---- Board pins (Adafruit Feather ESP32-S3 TFT) --------------------------
#define PIN_NEOPIXEL 33
#define PIN_NEOPIXEL_POWER 34
#define PIN_LED 13

// ---- Protocol + send state ----------------------------------------------
static procon::Protocol gProtocol;
static SemaphoreHandle_t gSendMutex;
static volatile bool gMacroRunning = false;  // set while a menu-started run is active

// Last input report body (without the leading report-id byte) so a control
// GET_REPORT(INPUT) can be answered instead of stalled (matches finger563).
static uint8_t s_last_input[64] = {0};
static uint16_t s_last_input_len = 0;

// ---- HID report descriptor (generated from espp/hid-rp upstream) ---------
// espp::switch_pro_descriptor() is a constexpr generator that emits the Pro
// Controller report descriptor at compile time -- exactly what the reference
// esp-usb-ble-hid feeds its USB path. .size() drives the configuration
// descriptor length below.
//
// NOTE: the hid-rp generator is heavy constexpr template metaprogramming that
// the VS Code C/C++ IntelliSense engine (EDG) cannot constant-evaluate -- it
// reports a bogus "expression must have a constant value". GCC evaluates it
// correctly, so the squiggle is a false positive. Under IntelliSense ONLY
// (`__INTELLISENSE__` is defined by the editor's parser, never by GCC) we swap
// in a plain std::array stand-in of the known 209-byte size so the editor stops
// complaining; every real build uses the generator.
#ifdef __INTELLISENSE__
static constexpr std::array<uint8_t, 209> kProDescriptor{};
#else
static constexpr auto kProDescriptor = espp::switch_pro_descriptor();
#endif

// IMPORTANT: TinyUSB on the ESP32-S3 (dwc2) DMAs EP0 control data, so every
// descriptor handed to the host must live in DMA-capable INTERNAL RAM, not in
// flash (.rodata). kProDescriptor is constexpr (flash), so the report
// descriptor is copied into this RAM buffer at startup and THAT is returned
// from tud_hid_descriptor_report_cb. (desc_device and the config descriptor are
// likewise non-const below so they land in .data / RAM. This is why the
// all-const version failed to enumerate while the reference -- which fills its
// descriptors at runtime, i.e. in RAM -- works.)
static uint8_t s_report_descriptor[kProDescriptor.size()];

// ---- USB descriptors (mirror finger563) ----------------------------------
#define TUSB_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_INOUT_DESC_LEN)

static tusb_desc_device_t desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x057E,
    .idProduct = 0x2009,
    .bcdDevice = 0x0200,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

static const char *hid_string_descriptor[5] = {
    (char[]){0x09, 0x04},  // 0: English (0x0409)
    "Nintendo Co., Ltd.",  // 1: Manufacturer
    "Pro Controller",      // 2: Product
    "000000000001",        // 3: Serial
    "USB HID Interface",   // 4: HID interface
};

static uint8_t hid_configuration_descriptor[] = {
    // config num, itf count, string idx, total len, attributes (bus-powered), power(mA)
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUSB_DESC_TOTAL_LEN, 0x00, 100),
    // itf num, string idx, protocol, report desc len, EP OUT, EP IN, size, interval
    TUD_HID_INOUT_DESCRIPTOR(0, 4, HID_ITF_PROTOCOL_NONE, kProDescriptor.size(),
                             0x01, 0x81, CFG_TUD_HID_EP_BUFSIZE, 1),
};

// ---- Sending helper ------------------------------------------------------
// `buf` is a full wire report with the report id at buf[0]; len includes it.
// report_id 0 tells TinyUSB the id is already present in the buffer. Caches the
// body (buf+1) so GET_REPORT(INPUT) can be answered. Mutex-guarded because the
// USB task (response path) and the stream task can both send.
static bool send_report(const uint8_t *buf, uint16_t len) {
  if (len == 0) return false;
  bool ok = false;
  xSemaphoreTake(gSendMutex, portMAX_DELAY);
  if (tud_hid_ready()) {
    uint16_t blen = len - 1;
    if (blen > sizeof(s_last_input)) blen = sizeof(s_last_input);
    memcpy(s_last_input, buf + 1, blen);
    s_last_input_len = blen;
    ok = tud_hid_report(0, buf, len);
  }
  xSemaphoreGive(gSendMutex);
  return ok;
}

// ---- TinyUSB device event handler ----------------------------------------
static void device_event_handler(tinyusb_event_t *event, void *arg) {
  (void)arg;
  switch (event->id) {
    case TINYUSB_EVENT_ATTACHED:
      ESP_LOGI(TAG, "USB mounted");
      gProtocol.reset();
      boulder_macro::reset();
      gMacroRunning = false;
      if (gProtocol.helloPending()) {
        size_t outLen = 0;
        const uint8_t *hello = gProtocol.takeHelloReport(outLen);
        send_report(hello, (uint16_t)outLen);
      }
      break;
    case TINYUSB_EVENT_DETACHED:
      ESP_LOGI(TAG, "USB unmounted");
      gMacroRunning = false;
      break;
    default:
      break;
  }
}

// ---- TinyUSB HID callbacks -----------------------------------------------
extern "C" {

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
  (void)instance;
  return s_report_descriptor;  // RAM copy (DMA-capable); see note at definition
}

// Answer GET_REPORT(INPUT) with the last input report instead of stalling.
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t *buffer,
                               uint16_t reqlen) {
  (void)instance;
  (void)report_id;
  if (report_type == HID_REPORT_TYPE_INPUT && s_last_input_len) {
    uint16_t n = s_last_input_len < reqlen ? s_last_input_len : reqlen;
    memcpy(buffer, s_last_input, n);
    return n;
  }
  return 0;
}

// OUT endpoint data / SET_REPORT: feed the protocol and reply synchronously.
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type, uint8_t const *buffer,
                           uint16_t bufsize) {
  (void)instance;
  if (report_type != HID_REPORT_TYPE_OUTPUT && report_type != 0) return;
  if (bufsize == 0) return;

  // Reassemble the full wire report (report id + payload) for the protocol.
  static uint8_t wire[80];
  uint16_t wlen;
  if (report_id != 0) {
    wire[0] = report_id;
    uint16_t n = bufsize;
    if (n > sizeof(wire) - 1) n = sizeof(wire) - 1;
    memcpy(wire + 1, buffer, n);
    wlen = n + 1;
  } else {
    wlen = bufsize;
    if (wlen > sizeof(wire)) wlen = sizeof(wire);
    memcpy(wire, buffer, wlen);
  }

  gProtocol.onOutputReport(wire, wlen);
  size_t outLen = 0;
  const uint8_t *resp = gProtocol.buildResponse(outLen);
  if (outLen > 0) send_report(resp, (uint16_t)outLen);
}

void tud_hid_report_complete_cb(uint8_t instance, uint8_t const *report, uint16_t len) {
  (void)instance;
  (void)report;
  (void)len;
}

}  // extern "C"

// ---- Streaming task: unsolicited 0x30 input once in standard mode ---------
static void stream_task(void *arg) {
  (void)arg;
  const TickType_t period = pdMS_TO_TICKS(8);  // ~120 Hz
  while (1) {
    if (tud_mounted() && gProtocol.streamingEnabled() && tud_hid_ready()) {
      // Controller accepted: drive the input macro only while a run is active
      // (started from the menu), then stream the current input state.
      if (gMacroRunning) boulder_macro::update(gProtocol.input);

      size_t outLen = 0;
      const uint8_t *rep = gProtocol.buildStreamReport(outLen);
      if (outLen > 0) send_report(rep, (uint16_t)outLen);
    }
    vTaskDelay(period);
  }
}

// ---- NeoPixel status ------------------------------------------------------
static led_strip_handle_t s_strip;

static void status_init(void) {
  gpio_reset_pin((gpio_num_t)PIN_NEOPIXEL_POWER);
  gpio_set_direction((gpio_num_t)PIN_NEOPIXEL_POWER, GPIO_MODE_OUTPUT);
  gpio_set_level((gpio_num_t)PIN_NEOPIXEL_POWER, 1);  // power the NeoPixel rail

  gpio_reset_pin((gpio_num_t)PIN_LED);
  gpio_set_direction((gpio_num_t)PIN_LED, GPIO_MODE_OUTPUT);

  led_strip_config_t strip_config = {};
  strip_config.strip_gpio_num = PIN_NEOPIXEL;
  strip_config.max_leds = 1;
  led_strip_rmt_config_t rmt_config = {};
  rmt_config.resolution_hz = 10 * 1000 * 1000;
  ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip));
  led_strip_clear(s_strip);
}

static void status_set(uint8_t r, uint8_t g, uint8_t b) {
  led_strip_set_pixel(s_strip, 0, r, g, b);
  led_strip_refresh(s_strip);
  gpio_set_level((gpio_num_t)PIN_LED, g > 0 ? 1 : 0);
}

// ---- USB attachment ------------------------------------------------------
// Minimal USB bring-up, mirroring the verified-working esp-usb-ble-hid
// reference: install the TinyUSB device stack and start the streaming task,
// both on core 0. Called directly from app_main on core 0.
static volatile bool s_usbStarted = false;

static void usb_start(void) {
  if (s_usbStarted) return;

  // Copy the (flash/constexpr) report descriptor into the DMA-capable RAM
  // buffer that the GET_DESCRIPTOR callback hands to TinyUSB.
  memcpy(s_report_descriptor, kProDescriptor.data(), sizeof(s_report_descriptor));

  tinyusb_config_t tusb_cfg = {};
  tusb_cfg.phy.skip_setup = false;
  tusb_cfg.phy.self_powered = false;
  tusb_cfg.phy.vbus_monitor_io = -1;
  tusb_cfg.task.size = 4096;
  tusb_cfg.task.priority = 4;
  tusb_cfg.task.xCoreID = 0;
  tusb_cfg.descriptor.device = &desc_device;
  tusb_cfg.descriptor.qualifier = NULL;
  tusb_cfg.descriptor.string = hid_string_descriptor;
  tusb_cfg.descriptor.string_count =
      sizeof(hid_string_descriptor) / sizeof(hid_string_descriptor[0]);
  tusb_cfg.descriptor.full_speed_config = hid_configuration_descriptor;
  tusb_cfg.descriptor.high_speed_config = NULL;
  tusb_cfg.event_cb = device_event_handler;
  tusb_cfg.event_arg = NULL;

  ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
  ESP_LOGI(TAG, "TinyUSB installed");

  // Streaming task: unsolicited 0x30 input reports once in standard mode.
  xTaskCreatePinnedToCore(stream_task, "procon_stream", 4096, NULL, 5, NULL, 0);
  s_usbStarted = true;
}

// Forward decl: UI/button/status loop, pinned to core 1 (defined after app_main).
static void app_loop_task(void *arg);

extern "C" void app_main(void) {
  ESP_LOGI(TAG, "Pro Controller (ESP-IDF) start");

  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  // Provision the protocol identity (real MAC + random serial), matching the
  // reference which does esp_read_mac + serial generation at startup.
  gProtocol.initIdentity(mac);

  gSendMutex = xSemaphoreCreateMutex();

  status_init();
  status_set(40, 0, 0);  // red: starting / not mounted

  button::begin();

  // Bring the TFT/LVGL UI up. esp_lvgl_port runs LVGL in its own task pinned to
  // core 1 (see ui::begin), so the renderer never competes with the USB tasks
  // on core 0. The boot self-test colors are mirrored onto the NeoPixel.
  ui::begin([](int colorIndex) {
    static const uint8_t c[9][3] = {
        {60, 60, 60}, {0, 0, 0}, {60, 0, 0}, {0, 60, 0}, {0, 0, 60},
        // stage markers: 5=YELLOW 6=CYAN 7=WHITE 8=GREEN(done)
        {60, 60, 0}, {0, 60, 60}, {50, 50, 50}, {0, 60, 0}};
    if (colorIndex >= 0 && colorIndex < 9) {
      status_set(c[colorIndex][0], c[colorIndex][1], c[colorIndex][2]);
    }
  });

  // Bring USB up on core 0 (the proven path: install + stream task on core 0,
  // FreeRTOS at 1 kHz so enumeration control transfers are serviced in time).
  usb_start();
  ESP_LOGI(TAG, "USB attached. Boot complete.");

  // Run the UI/button/status loop in its own task on core 1, off the USB core.
  xTaskCreatePinnedToCore(app_loop_task, "app_loop", 6144, NULL, 3, NULL, 1);
}

// ---- UI / button / status loop (runs on core 1) --------------------------
static void app_loop_task(void *arg) {
  (void)arg;
  uint32_t lastStatusMs = 0;
  while (1) {
    // Single-button navigation + menu commands.
    const button::Event ev = button::poll();
    if (ev != button::Event::None) ui::onButton(ev);
    switch (ui::takeCommand()) {
      case ui::Command::RunMacro:
        boulder_macro::start();  // begins a fresh, looping run
        gMacroRunning = true;
        break;
      case ui::Command::TogglePause:
        if (boulder_macro::isPaused()) {
          boulder_macro::resume();
        } else {
          boulder_macro::pause();
        }
        break;
      case ui::Command::StopMacro:
        gMacroRunning = false;
        boulder_macro::reset();  // neutralise the controller
        break;
      case ui::Command::Reattach:
        if (s_usbStarted) {
          tud_disconnect();
          vTaskDelay(pdMS_TO_TICKS(80));
          tud_connect();
        }
        break;
      case ui::Command::None:
      default:
        break;
    }
    ui::setHoldProgress(button::isDown() ? button::holdProgress() : 0.0f);

    // Mirror the live controller input onto the RUNNING overlay's Pro
    // Controller diagram (a no-op unless that overlay is visible). Copy the
    // shared input once; a torn read here is only a cosmetic 1-frame glitch.
    {
      const procon::Input in = gProtocol.input;
      ui::setControllerState(in.buttons, in.lx, in.ly, in.rx, in.ry);
    }

    // Status + NeoPixel refresh at ~5 Hz.
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (now - lastStatusMs >= 200) {
      lastStatusMs = now;
      const procon::Protocol::Diag &d = gProtocol.diag();
      ui::Phase phase;
      if (!tud_mounted()) {
        status_set(40, 0, 0);   // RED:    not mounted
        phase = ui::Phase::Detached;
      } else if (gProtocol.streamingEnabled()) {
        status_set(0, 60, 0);   // GREEN:  standard input mode -> accepted
        phase = ui::Phase::Ready;
      } else if (gProtocol.handshakeStarted()) {
        status_set(40, 30, 0);  // YELLOW: handshake underway
        phase = ui::Phase::Handshake;
      } else {
        status_set(0, 0, 60);   // BLUE:   mounted, no handshake yet
        phase = ui::Phase::Mounted;
      }
      ui::setStatus(phase, tud_mounted(), d.inCount, d.outCount, d.lastSubcommand,
                    d.gotDeviceInfo, d.gotStickCal, d.gotSetMode, d.gotVibration);
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}
