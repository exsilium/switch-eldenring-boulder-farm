#pragma once

#include <stddef.h>
#include <stdint.h>

#include "procon/procon_reports.h"

// Declarative, GPC-style macro engine.
//
// A macro is a flat, readable list of `Step`s. Each step carries its own
// timing, so every press, hold, and wait can have an individual duration
// (unlike a hand-written phase state machine with shared constants). Steps can
// drive any Pro Controller button, the D-pad, and both analog sticks, including
// overlapping / simultaneous holds.
//
// Authoring a macro is "copy the table, edit the steps" -- no engine changes:
//
//   static constexpr macro::Step kSequence[] = {
//       macro::Tap(macro::Channel::Left, 100),
//       macro::Wait(1000),
//       macro::Tap(macro::Channel::Right, 100),
//   };
//   static macro::Player gPlayer(kSequence);
//
// Then forward start/reset/update/isRunning/isDone to `gPlayer`.
namespace macro {

// A `Channel` names a single digital input and encodes *where* its bit lives in
// the standard 0x30 input report's three button bytes. The value packs the
// button-byte index in the high byte and the bit mask in the low byte:
//
//   value = (byteIndex << 8) | bitMask
//
// Byte layout (matches procon::Input::buttons and procon_reports.h):
//
//   byte 0 -- right cluster    byte 1 -- shared        byte 2 -- left + dpad
//   ----------------------     -------------------     ---------------------
//   0x01 Y                     0x01 Minus              0x01 Down
//   0x02 X                     0x02 Plus               0x02 Up
//   0x04 B                     0x04 R-stick click      0x04 Right
//   0x08 A                     0x08 L-stick click      0x08 Left
//   0x10 SR (right)            0x10 Home               0x10 SR (left)
//   0x20 SL (right)            0x20 Capture            0x20 SL (left)
//   0x40 R                                             0x40 L
//   0x80 ZR                                            0x80 ZL
enum class Channel : uint16_t {
  // byte 0 -- right cluster
  Y = 0x0001,
  X = 0x0002,
  B = 0x0004,
  A = 0x0008,
  RightSR = 0x0010,
  RightSL = 0x0020,
  R = 0x0040,
  ZR = 0x0080,

  // byte 1 -- shared
  Minus = 0x0101,
  Plus = 0x0102,
  RStick = 0x0104,
  LStick = 0x0108,
  Home = 0x0110,
  Capture = 0x0120,

  // byte 2 -- left cluster + D-pad
  Down = 0x0201,
  Up = 0x0202,
  Right = 0x0204,
  Left = 0x0208,
  LeftSR = 0x0210,
  LeftSL = 0x0220,
  L = 0x0240,
  ZL = 0x0280,
};

// Which analog stick a stick op targets.
enum class Stick : uint8_t { Left, Right };

// Extract the button-byte index / bit mask packed into a Channel.
constexpr uint8_t channelByte(Channel c) {
  return (uint8_t)((uint16_t)c >> 8);
}
constexpr uint8_t channelMask(Channel c) {
  return (uint8_t)((uint16_t)c & 0xFF);
}

// A single tagged op in a macro sequence. Author these with the factory
// helpers below rather than by hand.
enum class Op : uint8_t {
  Down,      // press a channel (bit set); no delay
  Up,        // release a channel (bit clear); no delay
  Wait,      // hold the current accumulated state for `ms`
  SetStick,  // set one analog stick to (x, y); no delay
  Tap,       // press `channel`, hold `ms`, then release
};

struct Step {
  Op op;
  Channel channel;  // Down / Up / Tap
  Stick stick;      // SetStick
  uint16_t x;       // SetStick
  uint16_t y;       // SetStick
  uint32_t ms;      // Wait / Tap
};

// ---- Factory helpers (constexpr, for clean authoring) --------------------

// Instant press / release. Overlap-friendly: multiple Down()s before an Up()
// hold several inputs simultaneously.
constexpr Step Down(Channel c) {
  return Step{Op::Down, c, Stick::Left, 0, 0, 0};
}
constexpr Step Up(Channel c) {
  return Step{Op::Up, c, Stick::Left, 0, 0, 0};
}

// Hold the current accumulated state for `ms` milliseconds.
constexpr Step Wait(uint32_t ms) {
  return Step{Op::Wait, Channel::Y, Stick::Left, 0, 0, ms};
}

// Convenience: press `c`, hold `ms`, release. Equivalent to
// Down(c), Wait(ms), Up(c).
constexpr Step Tap(Channel c, uint32_t ms) {
  return Step{Op::Tap, c, Stick::Left, 0, 0, ms};
}

// Instant analog set. 12-bit range; centre is procon::kStickCenter (0x800).
constexpr Step StickMove(Stick s, uint16_t x, uint16_t y) {
  return Step{Op::SetStick, Channel::Y, s, x, y, 0};
}
constexpr Step StickCenter(Stick s) {
  return Step{Op::SetStick, Channel::Y, s, procon::kStickCenter,
              procon::kStickCenter, 0};
}

// Runs a `const Step[]` sequence, accumulating controller state and driving a
// procon::Input each tick.
//
//   Player p(kSequence);   // count deduced from the array
//   p.start();
//   while (p.update(in)) { /* stream `in` each tick */ }
//
// Each update() consumes all zero-duration ops (Down/Up/SetStick) up to the
// next timed op, holds on Wait/Tap until their `ms` elapse, and writes the
// accumulated state into `in`. It returns true on the single tick the sequence
// completes; on completion the controller is neutralised (buttons released,
// sticks centred).
class Player {
 public:
  Player(const Step* steps, size_t count) : _steps(steps), _count(count) {}
  template <size_t N>
  explicit Player(const Step (&steps)[N]) : Player(steps, N) {}

  // Begin from the first step with a neutral controller state.
  void start();

  // Force back to an inert, neutral state.
  void reset();

  bool isRunning() const { return _state == State::Running; }
  bool isDone() const { return _state == State::Done; }

  // Advance the sequence and write the current state into `in`. Returns true on
  // the tick the sequence completes. No-op (returns false) when not running.
  bool update(procon::Input& in);

 private:
  enum class State : uint8_t { Idle, Running, Done };

  // esp_timer-backed millisecond clock (the engine's only platform dependency).
  static unsigned long millis();

  void neutral();
  void writeState(procon::Input& in) const;
  void applyDown(Channel c) { _buttons[channelByte(c)] |= channelMask(c); }
  void applyUp(Channel c) { _buttons[channelByte(c)] &= ~channelMask(c); }
  void applyStick(Stick s, uint16_t x, uint16_t y);

  const Step* _steps;
  size_t _count;
  size_t _index = 0;
  State _state = State::Idle;

  // Accumulated controller state.
  uint8_t _buttons[3] = {0x00, 0x00, 0x00};
  uint16_t _lx = procon::kStickCenter;
  uint16_t _ly = procon::kStickCenter;
  uint16_t _rx = procon::kStickCenter;
  uint16_t _ry = procon::kStickCenter;

  // In-progress timed op (Wait / Tap).
  bool _timing = false;
  bool _hasRelease = false;  // Tap: release _releaseChannel when the hold ends
  Channel _releaseChannel = Channel::Y;
  unsigned long _timerStart = 0;
  uint32_t _timerMs = 0;
};

}  // namespace macro
