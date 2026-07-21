#include "procon_protocol.h"

#include <string.h>

#include "esp_random.h"
#include "esp_timer.h"

// IDF replacement for Arduino millis() (the only framework dependency).
static inline uint32_t millis() {
  return (uint32_t)(esp_timer_get_time() / 1000);
}

namespace procon {

const uint8_t Protocol::kVibOpts[4] = {0xA0, 0xB0, 0xC0, 0x90};

Protocol::Protocol() {
  static const uint8_t kAddr[6] = {0x7C, 0xBB, 0x8A, 0x12, 0x34, 0x56};
  memcpy(_addr, kAddr, sizeof(_addr));
  _hasSerial = false;
  _everOut = 0;
  reset();
}

void Protocol::initIdentity(const uint8_t* mac) {
  // Real device MAC (matches the reference esp_read_mac), used in the hello and
  // device-info replies in natural byte order.
  if (mac) memcpy(_addr, mac, sizeof(_addr));
  // Random 11-char ASCII serial. First byte is a digit ('0'..'9' = 0x30..0x39,
  // i.e. < 0x80) so the host treats it as a present serial (>= 0x80 = none).
  for (int i = 0; i < (int)sizeof(_serial); i++) {
    _serial[i] = (uint8_t)('0' + (esp_random() % 10));
  }
  _hasSerial = true;
}

void Protocol::reset() {
  clearReport();
  clearRequest();
  _vibrationEnabled = false;
  _vibrationReport = 0x00;
  _vibrationIdx = 0x00;
  _imuEnabled = false;
  _deviceInfoQueried = false;
  _helloPending = true;
  _hidReady = false;
  _standardMode = false;
  _timer = 0;
  _timestampMs = 0;
  _rumbleL = 0;
  _rumbleR = 0;
  _rumbleMs = 0;
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

  _everOut++;  // sticky: proves the host enumerated + sent us an OUT report
  _diag.outCount++;
  _helloPending = false;  // host is talking now; stop the unsolicited hello
  _diag.lastOutId = data[0];
  if (data[0] == 0x80 && len > 1) {
    _diag.lastHandshake = data[1];
    if (data[1] == 0x04) _hidReady = true;  // host enabled USB HID
  } else if (data[0] == 0x01 && len > 10) {
    _diag.lastSubcommand = data[10];
    // 0x01 output reports carry the 8-byte HD-rumble payload at offset 2
    // (len > 10 guarantees the full left+right payload at bytes 2..9).
    decodeRumble(data + 2);
  } else if (data[0] == 0x10 && len >= 10) {
    // 0x10 rumble-only reports share the 0x01 layout ([id][packet#][8-byte
    // rumble]), so the rumble payload is at the same offset 2.
    decodeRumble(data + 2);
  }
  decayRumble();
}

// Decay the held rumble amplitudes toward zero so a brief packet stays visible
// for a few poll cycles but a stream that stops is reflected quickly. Linear
// decay of ~4 units/ms empties a full-scale (255) reading in ~64 ms.
void Protocol::decayRumble() {
  const uint32_t now = millis();
  if (_rumbleMs == 0) {
    _rumbleMs = now;
    return;
  }
  const uint32_t dt = now - _rumbleMs;
  _rumbleMs = now;
  const uint32_t drop = dt * 4;
  _rumbleL = (drop >= _rumbleL) ? 0 : (uint16_t)(_rumbleL - drop);
  _rumbleR = (drop >= _rumbleR) ? 0 : (uint16_t)(_rumbleR - drop);
  _diag.rumbleL = _rumbleL;
  _diag.rumbleR = _rumbleR;
}

// Decode the 8-byte payload (4 bytes left, 4 bytes right) and hold the peak:
// take the max of the freshly decoded amplitude and the current decayed value
// so short bursts are observable while decayRumble() bleeds them off over time.
void Protocol::decodeRumble(const uint8_t* rumble) {
  const uint16_t l = decodeRumbleAmplitude(rumble);
  const uint16_t r = decodeRumbleAmplitude(rumble + 4);
  if (l > _rumbleL) _rumbleL = l;
  if (r > _rumbleR) _rumbleR = r;
  _diag.rumbleL = _rumbleL;
  _diag.rumbleR = _rumbleR;
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
  // Neutral IMU: controller resting flat. Accel = (0, 0, +1g) (0x1000 = 1g in
  // raw units), gyro = 0 on all axes. The previous block was a sample captured
  // from real hardware with small non-zero gyro rates; streamed every frame,
  // hosts with motion controls read that as continuous rotation (camera drift).
  static const uint8_t kImu[36] = {
      0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
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
}

void Protocol::setDeviceInfo() {
  _report[14] = 0x82;  // ACK
  _report[15] = 0x02;  // subcommand reply
  _report[16] = 0x03;  // firmware version (major)
  _report[17] = 0x48;  // firmware version (minor)
  _report[18] = 0x03;  // controller id: Pro Controller
  _report[19] = 0x02;  // unknown, always 2
  memcpy(_report + 20, _addr, 6);
  _report[26] = 0x03;  // unknown (finger563 uses 0x03)
  _report[27] = 0x02;  // colour source: default colours (finger563 uses 0x02)
  _diag.gotDeviceInfo = true;
}

void Protocol::setShipment() {
  _report[14] = 0x80;
  _report[15] = 0x08;
}

void Protocol::spiRead() {
  // Byte-for-byte port of finger563's SwitchPro::spi_read / spi_read_impl.
  // The Switch reads calibration straight out of two flat factory/user ROM
  // dumps; bank = addr_top, register offset = addr_bottom, length = sub[5].
  const uint8_t bank = _request[12];        // subcommand[2]
  const uint8_t reg = _request[11];         // subcommand[1]
  const uint8_t readLength = _request[15];  // subcommand[5]

  // Factory calibration bank 0x60xx (finger563 spi_rom_data_60).
  static const uint8_t kSpiRom60[] = {
      // 0x00: serial number (>= 0x80 first byte => no serial)
      0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
      0xFF, 0xFF, 0xFF, 0xFF,
      // 0x10
      0xFF, 0xFF, 0x03, 0xA0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x02,
      0xFF, 0xFF, 0xFF, 0xFF,
      // 0x20: IMU factory calibration
      0xF0, 0xFF, 0x89, 0x00, 0xF0, 0x01, 0x00, 0x40, 0x00, 0x40, 0x00, 0x40,
      0xF9, 0xFF, 0x06, 0x00, 0x09, 0x00, 0xE7, 0x3B, 0xE7, 0x3B, 0xE7, 0x3B,
      // 0x38-0x3C unused
      0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
      // 0x3D: left/right stick factory calibration
      0xFF, 0xF7, 0x7F, 0x00, 0x08, 0x80, 0x00, 0x08, 0x80, 0x00, 0x08, 0x80,
      0x00, 0x08, 0x80, 0xFF, 0xF7, 0x7F,
      // 0x4F unused
      0xFF,
      // 0x50: colours (body, button, left grip, right grip) + unused
      0x82, 0x82, 0x82, 0x0F, 0x0F, 0x0F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
      0xFF, 0xFF, 0xFF, 0xFF,
      // 0x60 unused
      0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
      0xFF, 0xFF, 0xFF, 0xFF,
      // 0x70 unused
      0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
      0xFF, 0xFF, 0xFF, 0xFF,
      // 0x80: IMU horizontal offset
      0x50, 0xFD, 0x00, 0x00, 0xC6, 0x0F,
      // stick device parameters pt.1
      0x0F, 0x30, 0x61, 0x96, 0x30, 0xF3, 0xD4, 0x14, 0x54, 0x41, 0x15, 0x54,
      0xC7, 0x79, 0x9C, 0x33, 0x36, 0x63,
      // stick device parameters pt.2
      0x0F, 0x30, 0x61, 0x96, 0x30, 0xF3, 0xD4, 0x14, 0x54, 0x41, 0x15, 0x54,
      0xC7, 0x79, 0x9C, 0x33, 0x36, 0x63,
      // unused
      0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

  // User calibration bank 0x80xx (finger563 spi_rom_data_80).
  static const uint8_t kSpiRom80[] = {
      // 0x00 unused
      0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
      0xFF, 0xFF, 0xFF, 0xFF,
      // 0x10: user left stick calibration (magic 0xB2 0xA1)
      0xB2, 0xA1, 0xFF, 0xF7, 0x7F, 0x00, 0x08, 0x80, 0x00, 0x00, 0x00,
      // 0x1B: user right stick calibration (magic 0xB2 0xA1)
      0xB2, 0xA1, 0xFF, 0xF7, 0x7F, 0x00, 0x08, 0x80, 0x00, 0x00, 0x00,
      // 0x26: user IMU calibration (magic 0xB2 0xA1)
      0xB2, 0xA1, 0xBE, 0xFF, 0x3E, 0x00, 0xF0, 0x01, 0x00, 0x40, 0x00, 0x40,
      0x00, 0x40, 0xFE, 0xFF, 0xFE, 0xFF, 0x08, 0x00, 0xE7, 0x3B, 0xE7, 0x3B,
      0xE7, 0x3B};

  const uint8_t* rom = nullptr;
  size_t romSize = 0;
  if (bank == 0x60) {
    rom = kSpiRom60;
    romSize = sizeof(kSpiRom60);
  } else if (bank == 0x80) {
    rom = kSpiRom80;
    romSize = sizeof(kSpiRom80);
  }

  if (rom == nullptr) {
    // finger563 NACKs unreadable banks (e.g. shipment) with 0x83 0x00.
    _report[14] = 0x83;
    _report[15] = 0x00;
    return;
  }

  _report[14] = 0x90;       // ACK
  _report[15] = 0x10;       // subcommand reply
  _report[16] = reg;        // echo address (low)
  _report[17] = bank;       // echo address (high)
  _report[18] = 0x00;
  _report[19] = 0x00;
  _report[20] = readLength;  // echo length
  for (uint8_t i = 0; i < readLength; i++) {
    const size_t idx = (size_t)reg + i;
    uint8_t b = (idx < romSize) ? rom[idx] : 0xFF;
    // Inject the device serial into the factory ROM serial region (0x60 bank,
    // 0x00-0x0F): digits at 0x00-0x0A, byte 0x0B left as template (0xFF), and
    // 0x0C-0x0F zeroed -- matching the reference's serial layout.
    if (bank == 0x60 && _hasSerial) {
      if (idx < sizeof(_serial)) {
        b = _serial[idx];
      } else if (idx >= 0x0C && idx <= 0x0F) {
        b = 0x00;
      }
    }
    _report[21 + i] = b;
  }
  if (bank == 0x60 && reg <= 0x3D && (uint16_t)(reg + readLength) > 0x3D) {
    _diag.gotStickCal = true;
  }
}

void Protocol::setMode() {
  _report[14] = 0x80;
  _report[15] = 0x03;
  // subcommand[1] (req[11]) selects the report mode: 0x30 standard, 0x31 nfc/ir,
  // 0x3F simpleHID. Begin streaming standard input reports once set to 0x30.
  if (_request[11] == 0x30) _standardMode = true;
  _diag.gotSetMode = true;
}

void Protocol::setTriggerButtons() {
  _report[14] = 0x83;
  _report[15] = 0x04;
  // Trigger-buttons-elapsed-time payload: 7 little-endian uint16 (L, R, ZL, ZR,
  // SL, SR, HOME) in 10ms units. We don't track press durations, so report all
  // zeros -- which is exactly what the reference reports at handshake time.
  memset(_report + 16, 0x00, 14);
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
  _report[14] = 0x82;
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
  // finger563 deliberately ACKs unknown subcommands (0x80 + 0x03) instead of
  // NACKing, to avoid getting stuck in an infinite loop "arguing" with the
  // Switch (which would otherwise re-send and eventually drop the controller).
  _report[14] = 0x80;
  _report[15] = subId;
  _report[16] = 0x03;
}

// ---------------------------------------------------------------------------
// Top-level report builders
// ---------------------------------------------------------------------------

uint8_t* Protocol::buildHelloReport() {
  // Device-initiated hello (finger563 on_attach DEVICE_INIT_REPORT): announce as
  // a Pro Controller with our MAC so the host begins driving the handshake.
  // Wire: [0x81, 0x01, 0x00, 0x03, MAC[0..5]] (MAC in natural order). Built in
  // the dedicated _stream buffer so it never races the USB-task response path.
  memset(_stream, 0x00, sizeof(_stream));
  _stream[0] = 0x81;
  _stream[1] = 0x01;
  _stream[3] = 0x03;  // device type: Pro Controller
  memcpy(_stream + 4, _addr, 6);
  return _stream;
}

uint8_t* Protocol::buildHandshakeReport() {
  // Reply to a 0x80 host-init command. finger563 returns DEVICE_INIT_REPORT
  // (0x81) echoing the command byte for every 0x80 sub-command; the 0x02
  // handshake additionally echoes the host's full payload back.
  clearReport();
  _report[0] = 0x81;
  if (_request[1] == 0x02) {
    for (int i = 1; i < 63; i++) _report[i] = _request[i];
  } else {
    _report[1] = _request[1];
  }
  return _report;
}

uint8_t* Protocol::buildSubcommandReport() {
  clearReport();
  _report[0] = 0xA1;  // BT prefix; skipped on the wire

  if (_request[0] == 0x01) {
    switch (_request[10]) {
      case 0x00:
        // ONLY_CONTROLLER_STATE: ACK with 0x80 + 0x00 (finger563).
        setSubcommandReply();
        _report[14] = 0x80;
        _report[15] = 0x00;
        break;
      case 0x01: setSubcommandReply(); setBt(); break;
      case 0x02:
        _deviceInfoQueried = true;
        _hidReady = true;
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

const uint8_t* Protocol::buildResponse(size_t& outLen) {
  // Immediate reply to the OUT report just stored by onOutputReport(). finger563
  // answers the 0x80 host-init family and 0x01 output/subcommand reports; a
  // rumble-only report (0x10) gets no reply.
  if (_request[0] == 0x80) {
    const uint8_t* result = buildHandshakeReport();
    clearRequest();
    outLen = 64;
    _diag.inCount++;
    return result;
  }
  if (_request[0] == 0x01) {
    const uint8_t* result = buildSubcommandReport() + 1;  // skip 0xA1 BT prefix
    clearRequest();
    outLen = 64;
    _diag.inCount++;
    return result;
  }
  clearRequest();  // rumble-only / unhandled -> no reply
  outLen = 0;
  return _report;
}

const uint8_t* Protocol::takeHelloReport(size_t& outLen) {
  _helloPending = false;  // one-shot
  outLen = 64;
  _diag.inCount++;
  return buildHelloReport();
}

const uint8_t* Protocol::buildStreamReport(size_t& outLen) {
  // Unsolicited standard 0x30 input report, built in its own buffer so the main
  // loop can stream it without racing the USB-task response path.
  memset(_stream, 0x00, sizeof(_stream));
  _stream[0] = 0x30;

  const uint32_t now = millis();
  if (_timestampMs == 0) _timestampMs = now;
  const uint32_t deltaMs = now - _timestampMs;
  _timer = (_timer + deltaMs * 4) & 0xFF;
  _timestampMs = now;
  _stream[1] = (uint8_t)_timer;

  _stream[2] = 0x81;  // battery full + USB connected
  _stream[3] = input.buttons[0];
  _stream[4] = input.buttons[1];
  _stream[5] = input.buttons[2];
  packStick(input.lx, input.ly, &_stream[6]);
  packStick(input.rx, input.ry, &_stream[9]);
  _stream[12] = _vibrationReport;
  if (_imuEnabled) {
    // Neutral IMU (flat at rest, zero gyro) -- see setImuData() for rationale.
    static const uint8_t kImu[36] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    memcpy(_stream + 13, kImu, sizeof(kImu));
  }
  outLen = 64;
  _diag.inCount++;
  return _stream;
}

}  // namespace procon
