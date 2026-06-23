#include "procon_usb.h"

#include <Arduino.h>
#include <string.h>

#include "USB.h"     // arduino-esp32 core USB device (ESPUSB `USB`)
#include "USBHID.h"  // arduino-esp32 core HID class

#include "procon_descriptor.h"

// ---------------------------------------------------------------------------
// USB glue using the arduino-esp32 *core* TinyUSB stack (not the Adafruit
// TinyUSB library). On ESP32 the Adafruit library never initialises the
// USB-OTG hardware unless USB-CDC-on-boot is enabled, so the device never
// enumerated. The core stack is brought up explicitly via USB.begin(), which
// runs tinyusb_init() (driver install + the device task that pumps
// tud_task()). Our report descriptor still defines a pure HID Pro Controller.
// ---------------------------------------------------------------------------

// TinyUSB connection control (provided by the core's bundled TinyUSB).
extern "C" {
bool tud_connect(void);
bool tud_disconnect(void);
bool tud_mounted(void);
}

namespace procon {

namespace {
Protocol gProtocol;

// Custom HID device: hands the host our Pro Controller report descriptor and
// routes OUT reports (handshake / rumble / subcommands) into the protocol.
class ProConDevice : public USBHIDDevice {
 public:
  uint16_t _onGetDescriptor(uint8_t* dst) override {
    memcpy(dst, kReportDescriptor, sizeof(kReportDescriptor));
    return sizeof(kReportDescriptor);
  }

  // The core delivers interrupt-OUT data with the report-id byte stripped and
  // supplied separately. The protocol expects the original wire layout
  // (data[0] == report id), so reassemble it before handing it over.
  void _onOutput(uint8_t report_id, const uint8_t* buffer,
                 uint16_t len) override {
    if (report_id == 0) {
      gProtocol.onOutputReport(buffer, len);
      return;
    }
    uint8_t full[65];
    if (len > sizeof(full) - 1) {
      len = sizeof(full) - 1;
    }
    full[0] = report_id;
    memcpy(full + 1, buffer, len);
    gProtocol.onOutputReport(full, (uint16_t)(len + 1));
  }
};

USBHID gHid;
ProConDevice gDevice;
}  // namespace

void begin() {
  // Device identity must be set before USB.begin() captures it.
  USB.VID(kVendorId);
  USB.PID(kProductId);
  USB.usbVersion(0x0200);           // bcdUSB 2.0
  USB.firmwareVersion(kDeviceBcd);  // bcdDevice 0x0210
  USB.usbClass(0x00);               // class defined at interface level (HID)
  USB.usbSubClass(0x00);
  USB.usbProtocol(0x00);
  USB.manufacturerName(kManufacturer);
  USB.productName(kProduct);

  gHid.addDevice(&gDevice, sizeof(kReportDescriptor));
  gHid.begin();

  // Bring up the USB-OTG stack (driver install + device task), then drop off
  // the bus so the controller only appears after a deliberate long-press.
  USB.begin();
  tud_disconnect();
}

void attach() {
  gProtocol.reset();
  // Force a clean disconnect -> settle -> connect so the host reliably sees a
  // fresh enumeration. The brief blocking settle runs once per button press.
  tud_disconnect();
  delay(50);
  tud_connect();
}

void detach() { tud_disconnect(); }

bool mounted() { return tud_mounted(); }

bool handshakeStarted() { return gProtocol.handshakeStarted(); }

const Protocol::Diag& diag() { return gProtocol.diag(); }

void resetProtocol() { gProtocol.reset(); }

Input& input() { return gProtocol.input; }

void task() {
  if (!tud_mounted()) return;
  if (!gHid.ready()) return;

  size_t len = 0;
  const uint8_t* report = gProtocol.generateUsbReport(len);
  // report[0] is the in-band report id; report-id arg 0 sends the buffer as-is.
  gHid.SendReport(0, report, len, 8);
}

}  // namespace procon
