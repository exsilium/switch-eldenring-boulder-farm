#pragma once

#include <stdint.h>
#include <stddef.h>

#include "procon_reports.h"

namespace procon {

// Implements the Switch Pro Controller wired handshake + subcommand protocol.
//
// The host sends OUT reports (0x80 handshake family, 0x01 rumble+subcommand,
// 0x10/0x11 rumble-only). The device must answer every IN poll with either a
// requested reply (0x81 handshake echo / 0x21 subcommand reply) or the neutral
// 0x30 standard input report, otherwise the console drops the controller.
//
// Indexing mirrors dekuNukem / retro-pico-switch: an internal buffer carries a
// leading 0xA1 "BT" byte at index 0 so subcommand offsets match the reference
// tables exactly; generateUsbReport() returns the pointer past that byte.
class Protocol {
 public:
  Protocol();

  // The live input state the host will see. Mutated externally (macro/main).
  Input input;

  // Reset per-connection handshake/timer state. Call on each fresh attach.
  void reset();

  // Provision the device identity once at boot (matches the reference, which
  // reads esp_read_mac and generates a serial at startup): copies the real
  // device MAC (used in the hello + device-info replies) and injects a random
  // ASCII serial into the factory SPI ROM serial region (0x60 0x00-0x0A).
  void initIdentity(const uint8_t* mac);

  // Record an OUT report received from the host.
  void onOutputReport(const uint8_t* data, uint16_t len);

  // Build the immediate IN reply to the OUT report just recorded via
  // onOutputReport(). Returns a pointer to outLen bytes (valid until the next
  // call); outLen == 0 means "send nothing" (e.g. a rumble-only report). Meant
  // to be called synchronously from the USB OUT callback, exactly like
  // finger563's on_hid_report -> tud_hid_report.
  const uint8_t* buildResponse(size_t& outLen);

  // One-shot device-initiated hello (0x81 0x01 + MAC). True once per connection
  // until taken; mirrors finger563's single on_attach DEVICE_INIT_REPORT.
  bool helloPending() const { return _helloPending; }
  const uint8_t* takeHelloReport(size_t& outLen);

  // True once the host switched us into standard (0x30) input mode; only then do
  // we stream unsolicited input reports (a real controller is silent until
  // set_mode 0x30). buildStreamReport() uses its own buffer so it is safe to
  // call from the main loop while buildResponse() runs in the USB task.
  bool streamingEnabled() const { return _standardMode; }
  const uint8_t* buildStreamReport(size_t& outLen);

  // True once the host has queried device info (a useful "handshake underway"
  // signal for the UI / state machine).
  bool handshakeStarted() const { return _deviceInfoQueried; }

  // Latest decoded host-rumble amplitude per side (0..255), left ~= RUMBLE_A,
  // right ~= RUMBLE_B. Extracted from the 0x01/0x10 output reports' HD-rumble
  // payload (see procon::decodeRumbleAmplitude) with a short decay/hold so brief
  // packets remain observable at the ~120 Hz poll rate. A macro reads these to
  // implement feedback-driven behaviour (e.g. death detection / auto-stop).
  uint16_t rumbleLeft() const { return _rumbleL; }
  uint16_t rumbleRight() const { return _rumbleR; }

  // Sticky total of OUT reports received across ALL connection sessions (NOT
  // cleared by reset()). >0 proves the host enumerated us and began the
  // handshake at least once -- distinguishes "never enumerated" from "handshake
  // failed" even if the host attach/detach loops and wipes the per-session diag.
  uint32_t everOutCount() const { return _everOut; }

  // On-device handshake trace (the TFT is the only debug surface). Counters and
  // milestone flags let P2 be diagnosed without USB serial.
  struct Diag {
    uint32_t outCount;       // OUT reports received from the host
    uint32_t inCount;        // IN reports sent to the host
    uint8_t lastOutId;       // report id of the last OUT report (buf[0])
    uint8_t lastSubcommand;  // last 0x01 subcommand id (buf[10])
    uint8_t lastHandshake;   // last 0x80 handshake sub-type (buf[1])
    bool gotDeviceInfo;      // 0x02 device-info subcommand answered
    bool gotStickCal;        // 0x603D factory stick calibration read
    bool gotSetMode;         // 0x03 set-input-report-mode answered
    bool gotVibration;       // 0x48 enable-vibration answered
    uint16_t rumbleL;        // last decoded left (RUMBLE_A) amplitude, 0..255
    uint16_t rumbleR;        // last decoded right (RUMBLE_B) amplitude, 0..255
  };
  const Diag& diag() const { return _diag; }

 private:
  // Vibration option cycle used to nudge the vibrator byte on subcommand reply.
  static const uint8_t kVibOpts[4];

  uint8_t _report[100];
  uint8_t _request[100];
  uint8_t _stream[64];  // dedicated buffer for hello / streamed 0x30 (loop task)
  uint8_t _addr[6];
  uint8_t _serial[11];  // ASCII serial injected into factory SPI ROM (0x60)
  bool _hasSerial;      // true once initIdentity() has generated _serial

  bool _vibrationEnabled;
  uint8_t _vibrationReport;
  uint8_t _vibrationIdx;
  bool _imuEnabled;  bool _deviceInfoQueried;
  bool _helloPending;  // announce 0x81 0x01 until the host sends its first OUT
  bool _hidReady;      // stream 0x30 only after the handshake has advanced
  bool _standardMode;  // host issued set_mode 0x30 -> begin streaming input
  uint32_t _timer;
  uint32_t _timestampMs;
  uint32_t _everOut;  // sticky OUT-report total (survives reset())
  uint16_t _rumbleL;  // decoded left (RUMBLE_A) amplitude, 0..255 (decayed)
  uint16_t _rumbleR;  // decoded right (RUMBLE_B) amplitude, 0..255 (decayed)
  uint32_t _rumbleMs; // millis() of the last rumble decay tick
  Diag _diag;

  void clearReport();
  void clearRequest();
  void setTimer();
  // Decode the 8-byte HD-rumble payload at `rumble` (4 bytes left, 4 right) and
  // fold it into the decayed _rumbleL/_rumbleR amplitudes.
  void decodeRumble(const uint8_t* rumble);
  void decayRumble();
  void writeSticks();              // fill _switchReport sticks/buttons from input
  void setStandardInputReport();   // timer + buttons + sticks + vibration
  void setFullInputReport();       // 0x30 + IMU
  void setSubcommandReply();       // 0x21 header
  void setImuData();

  // Subcommand handlers (fill ACK byte + reply payload).
  void setBt();
  void setDeviceInfo();
  void setShipment();
  void spiRead();
  void setMode();
  void setTriggerButtons();
  void toggleImu();
  void imuSensitivity();
  void enableVibration();
  void setPlayerLights();
  void setNfcIrState();
  void setNfcIrConfig();
  void setUnknownSubcommand(uint8_t subId);

  uint8_t* buildSubcommandReport();  // returns _report (BT-indexed, [0]=0xA1)
  uint8_t* buildHandshakeReport();   // returns _report (USB-indexed, [0]=0x81)
  uint8_t* buildHelloReport();       // device-initiated 0x81 0x01 hello
};

}  // namespace procon
