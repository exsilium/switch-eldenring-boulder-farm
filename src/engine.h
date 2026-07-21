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

// Which axis of a stick a per-axis op targets.
enum class Axis : uint8_t { X, Y };

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
  SetAxis,   // set one axis of one stick to x, leaving the other axis; no delay
  Tap,       // press `channel`, hold `ms`, then release
};

struct Step {
  Op op;
  Channel channel;  // Down / Up / Tap
  Stick stick;      // SetStick / SetAxis
  Axis axis;        // SetAxis
  uint16_t x;       // SetStick / SetAxis (value)
  uint16_t y;       // SetStick
  uint32_t ms;      // Wait / Tap
};

// ---- Factory helpers (constexpr, for clean authoring) --------------------

// Instant press / release. Overlap-friendly: multiple Down()s before an Up()
// hold several inputs simultaneously.
constexpr Step Down(Channel c) {
  return Step{Op::Down, c, Stick::Left, Axis::X, 0, 0, 0};
}
constexpr Step Up(Channel c) {
  return Step{Op::Up, c, Stick::Left, Axis::X, 0, 0, 0};
}

// Hold the current accumulated state for `ms` milliseconds.
constexpr Step Wait(uint32_t ms) {
  return Step{Op::Wait, Channel::Y, Stick::Left, Axis::X, 0, 0, ms};
}

// Convenience: press `c`, hold `ms`, release. Equivalent to
// Down(c), Wait(ms), Up(c).
constexpr Step Tap(Channel c, uint32_t ms) {
  return Step{Op::Tap, c, Stick::Left, Axis::X, 0, 0, ms};
}

// Instant analog set. 12-bit range; centre is procon::kStickCenter (0x800).
constexpr Step StickMove(Stick s, uint16_t x, uint16_t y) {
  return Step{Op::SetStick, Channel::Y, s, Axis::X, x, y, 0};
}
constexpr Step StickCenter(Stick s) {
  return Step{Op::SetStick, Channel::Y, s, Axis::X, procon::kStickCenter,
              procon::kStickCenter, 0};
}

// Instant single-axis analog set: set one axis of `s` to `value`, leaving the
// other axis at its current accumulated value. Mirrors GPCs that drive PS5_LX /
// PS5_LY independently (hold one axis while sweeping the other).
constexpr Step StickAxis(Stick s, Axis a, uint16_t value) {
  return Step{Op::SetAxis, Channel::Y, s, a, value, 0, 0};
}

// Per-tick runtime context handed to an interrupt predicate. Lets a macro react
// to controller feedback (e.g. rumble amplitude) and elapsed time each tick,
// without the engine reaching into the USB/protocol layer -- the run loop feeds
// the values in via feedRumble() at the update() call site.
struct TickContext {
  uint16_t rumbleLeft;   // decoded host rumble, left side (RUMBLE_A), 0..255
  uint16_t rumbleRight;  // decoded host rumble, right side (RUMBLE_B), 0..255
  uint32_t elapsedMs;    // milliseconds since the current sequence started
};

// Interrupt predicate: return true to abort the main sequence and run the
// interrupt (reset) sequence. Evaluated once per tick while the main sequence
// runs (never during the interrupt sequence itself).
using InterruptFn = bool (*)(const TickContext&);

// Injectable millisecond clock (test hook). Defaults to an esp_timer-backed
// clock on device; host tests install a virtual clock via Player::setClock().
using ClockFn = unsigned long (*)();

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
//
// Looping: with setLoop(true) the sequence restarts (neutralised, from the
// first step) instead of completing, so it runs forever until reset(). In loop
// mode update() never returns true and isDone() never becomes true.
//
// Pausing: pause() freezes the accumulated state in place -- the held buttons /
// sticks keep streaming but timers stop advancing -- and resume() continues
// exactly where it left off (any in-progress Wait/Tap keeps its remaining time).
//
// Interrupts (condition-driven control flow): setInterrupt(pred, seq, n) arms a
// per-tick predicate + a distinct interrupt sequence. While the main sequence
// runs, `pred(ctx)` is evaluated each tick; when it fires the controller is
// neutralised and the interrupt (reset) sequence runs to completion. Afterwards
// the player resumes looping the main sequence (loop mode) or stops (one-shot).
// This maps 1:1 onto the reference GPC's "presumeDead -> reset_sequence" abort.
// Feed the predicate's rumble amplitude with feedRumble() before each update().
class Player {
 public:
  Player(const Step* steps, size_t count)
      : _main(steps), _mainCount(count), _steps(steps), _count(count) {}
  template <size_t N>
  explicit Player(const Step (&steps)[N]) : Player(steps, N) {}

  // Restart the sequence from the first step forever instead of completing.
  void setLoop(bool loop) { _loop = loop; }

  // In loop mode, pause instead of resuming the main sequence once a fired
  // interrupt's (reset) sequence completes: the main sequence is rearmed at its
  // first step but held (isPaused() true, isInterruptPaused() true) until
  // resume() -- mirroring the GPC where death flips the macro off after
  // reset_sequence until the user restarts it. No effect when not looping.
  void setPauseAfterInterrupt(bool pause) { _pauseAfterInterrupt = pause; }

  // Arm a condition-driven abort: while the main sequence runs, `pred` is polled
  // each tick; when it returns true the controller is neutralised and `steps`
  // (the interrupt / reset sequence) runs to completion before the main loop
  // resumes. Pass pred == nullptr to disarm.
  void setInterrupt(InterruptFn pred, const Step* steps, size_t count) {
    _interruptFn = pred;
    _interrupt = steps;
    _interruptCount = count;
  }
  template <size_t N>
  void setInterrupt(InterruptFn pred, const Step (&steps)[N]) {
    setInterrupt(pred, steps, N);
  }

  // Supply the current host-rumble amplitudes for the interrupt predicate. Call
  // once per tick (from the run loop) before update(); safe to omit if no
  // interrupt is armed.
  void feedRumble(uint16_t left, uint16_t right) {
    _rumbleL = left;
    _rumbleR = right;
  }

  // Begin from the first step with a neutral controller state.
  void start();

  // Force back to an inert, neutral state.
  void reset();

  // Freeze / continue the sequence (no-ops unless running). While paused,
  // update() keeps streaming the frozen state but does not advance.
  void pause();
  void resume();

  bool isRunning() const { return _state == State::Running; }
  bool isDone() const { return _state == State::Done; }
  bool isPaused() const { return _paused; }

  // True while the interrupt (reset) sequence is running after a fired predicate.
  bool isInterrupting() const { return _inInterrupt; }

  // True while parked by setPauseAfterInterrupt(true): the interrupt sequence
  // finished and the player is paused awaiting resume(). Cleared by resume(),
  // start(), and reset().
  bool isInterruptPaused() const { return _interruptPaused; }

  // Advance the sequence and write the current state into `in`. Returns true on
  // the tick the sequence completes. No-op (returns false) when not running.
  bool update(procon::Input& in);

  // Install a custom millisecond clock (host tests). nullptr restores default.
  static void setClock(ClockFn fn);

 private:
  enum class State : uint8_t { Idle, Running, Done };

  // esp_timer-backed millisecond clock (the engine's only platform dependency).
  static unsigned long millis();

  void neutral();
  void writeState(procon::Input& in) const;
  void applyDown(Channel c) { _buttons[channelByte(c)] |= channelMask(c); }
  void applyUp(Channel c) { _buttons[channelByte(c)] &= ~channelMask(c); }
  void applyStick(Stick s, uint16_t x, uint16_t y);
  void applyAxis(Stick s, Axis a, uint16_t v);
  // Switch the active sequence to the interrupt table (or back to main).
  void enterInterrupt(unsigned long now);
  void restartMain();

  const Step* _main;    // the primary (main) sequence
  size_t _mainCount;
  const Step* _steps;   // the currently active sequence (main or interrupt)
  size_t _count;
  size_t _index = 0;
  State _state = State::Idle;
  bool _loop = false;

  // Interrupt / condition-driven control flow.
  InterruptFn _interruptFn = nullptr;
  const Step* _interrupt = nullptr;
  size_t _interruptCount = 0;
  bool _inInterrupt = false;
  bool _pauseAfterInterrupt = false;  // park after the interrupt seq completes
  bool _interruptPaused = false;      // currently parked by that option
  uint16_t _rumbleL = 0;
  uint16_t _rumbleR = 0;
  unsigned long _seqStart = 0;  // start time of the active sequence (elapsedMs)

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

  // Pause bookkeeping.
  bool _paused = false;
  unsigned long _pauseStart = 0;
};

}  // namespace macro
