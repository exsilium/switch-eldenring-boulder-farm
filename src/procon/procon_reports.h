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

}  // namespace procon
