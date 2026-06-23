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

  // Record an OUT report received from the host.
  void onOutputReport(const uint8_t* data, uint16_t len);

  // Build the next 64-byte IN report. Returns a pointer to 64 bytes valid until
  // the next call. The first byte is the report ID (0x30 / 0x21 / 0x81).
  const uint8_t* generateUsbReport(size_t& outLen);

  // True once the host has queried device info (a useful "handshake underway"
  // signal for the UI / state machine).
  bool handshakeStarted() const { return _deviceInfoQueried; }

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
  };
  const Diag& diag() const { return _diag; }

 private:
  // Vibration option cycle used to nudge the vibrator byte on subcommand reply.
  static const uint8_t kVibOpts[4];

  uint8_t _report[100];
  uint8_t _request[100];
  uint8_t _addr[6];

  bool _vibrationEnabled;
  uint8_t _vibrationReport;
  uint8_t _vibrationIdx;
  bool _imuEnabled;
  bool _deviceInfoQueried;
  uint32_t _timer;
  uint32_t _timestampMs;
  Diag _diag;

  void clearReport();
  void clearRequest();
  void setTimer();
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
};

}  // namespace procon
