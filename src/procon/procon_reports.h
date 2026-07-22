#pragma once

#include <stdint.h>

namespace procon {

// 12-bit analog stick range; sticks rest at centre.
constexpr uint16_t kStickMin = 0x000;
constexpr uint16_t kStickCenter = 0x800;
constexpr uint16_t kStickMax = 0xFFF;

// Discrete D-pad bits live in button byte 2 of the standard input report.
constexpr uint8_t kDpadDown = 0x01;
constexpr uint8_t kDpadUp = 0x02;
constexpr uint8_t kDpadRight = 0x04;
constexpr uint8_t kDpadLeft = 0x08;

// Live controller input. main()/the macro mutate this; the protocol layer
// reads it when assembling 0x30 / 0x21 reports. Defaults to a neutral,
// centred controller.
struct Input {
  uint8_t buttons[3] = {0x00, 0x00, 0x00};  // [0]=right cluster, [1]=shared, [2]=left+dpad
  uint16_t lx = kStickCenter;
  uint16_t ly = kStickCenter;
  uint16_t rx = kStickCenter;
  uint16_t ry = kStickCenter;

  void reset() {
    buttons[0] = buttons[1] = buttons[2] = 0x00;
    lx = ly = rx = ry = kStickCenter;
  }
};

// Packs a 12-bit stick pair into the 3-byte little-endian-nibble layout the
// Switch expects: out[0]=x&0xff, out[1]=((y&0xf)<<4)|(x>>8), out[2]=y>>4.
inline void packStick(uint16_t x, uint16_t y, uint8_t* out) {
  out[0] = (uint8_t)(x & 0xFF);
  out[1] = (uint8_t)(((y & 0x0F) << 4) | (x >> 8));
  out[2] = (uint8_t)(y >> 4);
}

// ---- Host rumble (HD-rumble) amplitude decode ----------------------------
//
// The Switch drives haptics with an 8-byte rumble payload: 4 bytes for the
// left actuator (RUMBLE_A) followed by 4 bytes for the right (RUMBLE_B). Each
// side encodes a high-frequency and a low-frequency band (frequency + linear
// amplitude), but full HD-rumble reconstruction is unnecessary here -- a
// macro's "death detection" only needs a robust amplitude/threshold.
//
// The idle / "no rumble" payload for a side is {0x00, 0x01, 0x40, 0x40}
// (the values a controller streams when the game requests silence). We decode a
// coarse 0..255 amplitude as the deviation from that neutral pattern:
//   * high-band amplitude lives in byte 1 (bits 7..1); bit 0 / byte 0 are
//     frequency, so masking 0xFE yields 0 at idle and grows with intensity.
//   * low-band amplitude shows up as byte 3's deviation from the 0x40 neutral.
// The side amplitude is the stronger of the two bands, clamped to 0..255. This
// is intentionally a lightweight proxy (see issue #6: full HD-rumble fidelity
// is out of scope); it is monotonic with intensity and reads exactly 0 at idle.
constexpr uint8_t kRumbleNeutralLow = 0x40;

inline uint8_t decodeRumbleAmplitude(const uint8_t side[4]) {
  const uint8_t hi = (uint8_t)(side[1] & 0xFE);  // high-band amplitude, 0 at idle
  int loDev = (int)side[3] - (int)kRumbleNeutralLow;
  if (loDev < 0) loDev = -loDev;
  loDev *= 2;  // scale the low-band deviation into the 0..255 range
  if (loDev > 0xFF) loDev = 0xFF;
  return hi > (uint8_t)loDev ? hi : (uint8_t)loDev;
}

// Default death-detection threshold (matches the reference GPC's RUMBLE_MIN
// intent): an amplitude at/above this counts as a strong rumble.
constexpr uint16_t kRumbleMin = 10;

}  // namespace procon
