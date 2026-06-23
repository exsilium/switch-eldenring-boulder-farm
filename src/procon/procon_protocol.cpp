#include "procon_protocol.h"

#include <Arduino.h>
#include <string.h>

namespace procon {

const uint8_t Protocol::kVibOpts[4] = {0x0A, 0x0C, 0x0B, 0x09};

Protocol::Protocol() {
  static const uint8_t kAddr[6] = {0x7C, 0xBB, 0x8A, 0x12, 0x34, 0x56};
  memcpy(_addr, kAddr, sizeof(_addr));
  reset();
}

void Protocol::reset() {
  clearReport();
  clearRequest();
  _vibrationEnabled = false;
  _vibrationReport = 0x00;
  _vibrationIdx = 0x00;
  _imuEnabled = false;
  _deviceInfoQueried = false;
  _timer = 0;
  _timestampMs = 0;
  _diag = Diag{};
  input.reset();
}

void Protocol::clearReport() { memset(_report, 0x00, sizeof(_report)); }

void Protocol::clearRequest() { memset(_request, 0x00, sizeof(_request)); }

void Protocol::onOutputReport(const uint8_t* data, uint16_t len) {
  if (len == 0) return;
  if (len > sizeof(_request)) len = sizeof(_request);
  memset(_request, 0x00, sizeof(_request));
  memcpy(_request, data, len);

  _diag.outCount++;
  _diag.lastOutId = data[0];
  if (data[0] == 0x80 && len > 1) {
    _diag.lastHandshake = data[1];
  } else if (data[0] == 0x01 && len > 10) {
    _diag.lastSubcommand = data[10];
  }
}

// ---------------------------------------------------------------------------
// Report assembly
// ---------------------------------------------------------------------------

void Protocol::setTimer() {
  const uint32_t now = millis();
  if (_timestampMs == 0) {
    _timestampMs = now;
    _report[2] = 0x00;
    return;
  }
  const uint32_t deltaMs = now - _timestampMs;
  // Joy-Con timer ticks at ~4.96 ms; approximate the per-ms tick rate.
  const uint32_t elapsedTicks = deltaMs * 4;
  _timer = (_timer + elapsedTicks) & 0xFF;
  _report[2] = (uint8_t)_timer;
  _timestampMs = now;
}

void Protocol::setStandardInputReport() {
  setTimer();
  _report[3] = 0x81;  // battery (full) + USB-connected
  _report[4] = input.buttons[0];
  _report[5] = input.buttons[1];
  _report[6] = input.buttons[2];
  packStick(input.lx, input.ly, &_report[7]);
  packStick(input.rx, input.ry, &_report[10]);
  _report[13] = _vibrationReport;
}

void Protocol::setImuData() {
  if (!_imuEnabled) return;
  static const uint8_t kImu[36] = {
      0x75, 0xFD, 0xFD, 0xFF, 0x09, 0x10, 0x21, 0x00, 0xD5, 0xFF, 0xE0, 0xFF,
      0x72, 0xFD, 0xF9, 0xFF, 0x0A, 0x10, 0x22, 0x00, 0xD5, 0xFF, 0xE0, 0xFF,
      0x76, 0xFD, 0xFC, 0xFF, 0x09, 0x10, 0x23, 0x00, 0xD5, 0xFF, 0xE0, 0xFF};
  memcpy(_report + 14, kImu, sizeof(kImu));
}

void Protocol::setFullInputReport() {
  _report[1] = 0x30;
  setStandardInputReport();
  setImuData();
}

void Protocol::setSubcommandReply() {
  _report[1] = 0x21;
  // Nudge the vibrator byte on each subcommand reply (mirrors real HW).
  if (_vibrationEnabled) {
    _vibrationIdx = (_vibrationIdx + 1) % 4;
    _vibrationReport = kVibOpts[_vibrationIdx];
  }
  setStandardInputReport();
}

// ---------------------------------------------------------------------------
// Subcommand handlers
// ---------------------------------------------------------------------------

void Protocol::setBt() {
  _report[14] = 0x81;
  _report[15] = 0x01;
  _report[16] = 0x03;
}

void Protocol::setDeviceInfo() {
  _report[14] = 0x82;  // ACK
  _report[15] = 0x02;  // subcommand reply
  _report[16] = 0x03;  // firmware version (major)
  _report[17] = 0x48;  // firmware version (minor)
  _report[18] = 0x03;  // controller id: Pro Controller
  _report[19] = 0x02;  // unknown, always 2
  memcpy(_report + 20, _addr, 6);
  _report[26] = 0x01;  // unknown, always 1
  _report[27] = 0x01;  // colours stored in SPI
  _diag.gotDeviceInfo = true;
}

void Protocol::setShipment() {
  _report[14] = 0x80;
  _report[15] = 0x08;
}

void Protocol::spiRead() {
  const uint8_t addrTop = _request[12];
  const uint8_t addrBottom = _request[11];
  const uint8_t readLength = _request[15];

  _report[14] = 0x90;        // ACK
  _report[15] = 0x10;        // subcommand reply
  _report[16] = addrBottom;  // echo address (low)
  _report[17] = addrTop;     // echo address (high)
  _report[20] = readLength;  // echo length

  // Stick parameters; generally identical for both sticks.
  static const uint8_t params[18] = {0x0F, 0x30, 0x61, 0x96, 0x30, 0xF3,
                                     0xD4, 0x14, 0x54, 0x41, 0x15, 0x54,
                                     0xC7, 0x79, 0x9C, 0x33, 0x36, 0x63};

  if (addrTop == 0x60 && addrBottom == 0x00) {
    // Serial number: 0xFF => "no serial number".
    memset(_report + 21, 0xFF, 16);
  } else if (addrTop == 0x60 && addrBottom == 0x50) {
    // Controller colours.
    memset(_report + 21, 0x32, 3);  // body
    memset(_report + 24, 0xFF, 3);  // buttons
    memset(_report + 27, 0xFF, 7);  // grips / spacer
  } else if (addrTop == 0x80 && addrBottom == 0x10) {
    // User calibration: null.
    memset(_report + 21, 0xFF, 3);
  } else if (addrTop == 0x60 && addrBottom == 0x3D) {
    // Factory analog stick calibration.
    static const uint8_t lCal[9] = {0xD4, 0x75, 0x61, 0xE5, 0x87,
                                    0x7C, 0xEC, 0x55, 0x61};
    static const uint8_t rCal[9] = {0x5D, 0xD8, 0x7F, 0x18, 0xE6,
                                    0x61, 0x86, 0x65, 0x5D};
    memcpy(_report + 21, lCal, sizeof(lCal));
    memcpy(_report + 30, rCal, sizeof(rCal));
    _report[39] = 0xFF;             // spacer
    memset(_report + 40, 0x32, 3);  // body colour
    memset(_report + 43, 0xFF, 3);  // button colour
    _diag.gotStickCal = true;
  } else if (addrTop == 0x60 && addrBottom == 0x20) {
    // Six-axis motion sensor factory calibration.
    static const uint8_t saCal[24] = {
        0xCC, 0x00, 0x40, 0x00, 0x91, 0x01, 0x00, 0x40, 0x00, 0x40, 0x00, 0x40,
        0xE7, 0xFF, 0x0E, 0x00, 0xDC, 0xFF, 0x3B, 0x34, 0x3B, 0x34, 0x3B, 0x34};
    memcpy(_report + 21, saCal, sizeof(saCal));
  } else if (addrTop == 0x60 && addrBottom == 0x80) {
    // Six-axis factory parameters.
    _report[21] = 0x50;
    _report[22] = 0xFD;
    _report[23] = 0x00;
    _report[24] = 0x00;
    _report[25] = 0xC6;
    _report[26] = 0x0F;
    memcpy(_report + 27, params, sizeof(params));
  } else if (addrTop == 0x60 && addrBottom == 0x98) {
    // Stick device parameters 2 (duplicate of params 1).
    memcpy(_report + 21, params, sizeof(params));
  } else {
    memset(_report + 21, 0xFF, readLength);
  }
}

void Protocol::setMode() {
  _report[14] = 0x80;
  _report[15] = 0x03;
  _diag.gotSetMode = true;
}

void Protocol::setTriggerButtons() {
  _report[14] = 0x83;
  _report[15] = 0x04;
}

void Protocol::toggleImu() {
  _report[14] = 0x80;
  _report[15] = 0x40;
  _imuEnabled = (_request[11] == 0x01);
}

void Protocol::imuSensitivity() {
  _report[14] = 0x80;
  _report[15] = 0x41;
}

void Protocol::enableVibration() {
  _report[14] = 0x80;
  _report[15] = 0x48;
  _vibrationEnabled = true;
  _vibrationIdx = 0;
  _vibrationReport = kVibOpts[_vibrationIdx];
  _diag.gotVibration = true;
}

void Protocol::setPlayerLights() {
  _report[14] = 0x80;
  _report[15] = 0x30;
}

void Protocol::setNfcIrState() {
  _report[14] = 0x80;
  _report[15] = 0x22;
}

void Protocol::setNfcIrConfig() {
  _report[14] = 0xA0;
  _report[15] = 0x21;
  static const uint8_t params[8] = {0x01, 0x00, 0xFF, 0x00,
                                    0x08, 0x00, 0x1B, 0x01};
  memcpy(_report + 16, params, sizeof(params));
  _report[49] = 0xC8;
}

void Protocol::setUnknownSubcommand(uint8_t subId) {
  _report[14] = 0x00;  // NACK
  _report[15] = subId;
}

// ---------------------------------------------------------------------------
// Top-level report builders
// ---------------------------------------------------------------------------

uint8_t* Protocol::buildHandshakeReport() {
  clearReport();
  _report[0] = 0x81;
  _report[1] = _request[1];
  if (_request[1] == 0x01) {
    _report[3] = 0x03;  // device type
    for (int i = 0; i < 6; i++) {
      _report[4 + i] = _addr[5 - i];  // MAC, reversed
    }
  }
  return _report;
}

uint8_t* Protocol::buildSubcommandReport() {
  clearReport();
  _report[0] = 0xA1;  // BT prefix; skipped on the wire

  if (_request[0] == 0x01) {
    switch (_request[10]) {
      case 0x01: setSubcommandReply(); setBt(); break;
      case 0x02:
        _deviceInfoQueried = true;
        setSubcommandReply();
        setDeviceInfo();
        break;
      case 0x08: setSubcommandReply(); setShipment(); break;
      case 0x10: setSubcommandReply(); spiRead(); break;
      case 0x03: setSubcommandReply(); setMode(); break;
      case 0x04: setSubcommandReply(); setTriggerButtons(); break;
      case 0x40: setSubcommandReply(); toggleImu(); break;
      case 0x41: setSubcommandReply(); imuSensitivity(); break;
      case 0x48: setSubcommandReply(); enableVibration(); break;
      case 0x30: setSubcommandReply(); setPlayerLights(); break;
      case 0x22: setSubcommandReply(); setNfcIrState(); break;
      case 0x21: setSubcommandReply(); setNfcIrConfig(); break;
      default:
        setSubcommandReply();
        setUnknownSubcommand(_request[10]);
        break;
    }
  } else {
    // Idle / rumble-only output reports: stream the neutral standard report.
    setFullInputReport();
  }
  return _report;
}

const uint8_t* Protocol::generateUsbReport(size_t& outLen) {
  outLen = 64;
  const bool handshake =
      (_request[0] == 0x80) &&
      (_request[1] == 0x01 || _request[1] == 0x02 || _request[1] == 0x03);

  const uint8_t* result;
  if (handshake) {
    result = buildHandshakeReport();  // USB-indexed, starts at 0x81
  } else {
    result = buildSubcommandReport() + 1;  // skip the 0xA1 BT prefix
  }

  if (_request[0] != 0x00) clearRequest();
  _diag.inCount++;
  return result;
}

}  // namespace procon
