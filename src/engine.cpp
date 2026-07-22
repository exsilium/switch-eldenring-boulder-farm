#include "engine.h"

// The engine's only platform dependency is a millisecond clock. On device it is
// esp_timer; for host-side unit tests (ESP_PLATFORM undefined) the default is a
// zero clock and tests install a virtual clock via Player::setClock().
#if defined(ESP_PLATFORM)
#include "esp_timer.h"
#endif

namespace macro {

namespace {
unsigned long defaultMillis() {
#if defined(ESP_PLATFORM)
  return (unsigned long)(esp_timer_get_time() / 1000);
#else
  return 0;
#endif
}
ClockFn sClock = defaultMillis;
}  // namespace

void Player::setClock(ClockFn fn) { sClock = fn ? fn : defaultMillis; }

unsigned long Player::millis() { return sClock(); }

void Player::neutral() {
  _buttons[0] = _buttons[1] = _buttons[2] = 0x00;
  _lx = _ly = _rx = _ry = procon::kStickCenter;
}

void Player::writeState(procon::Input& in) const {
  in.buttons[0] = _buttons[0];
  in.buttons[1] = _buttons[1];
  in.buttons[2] = _buttons[2];
  in.lx = _lx;
  in.ly = _ly;
  in.rx = _rx;
  in.ry = _ry;
}

void Player::applyStick(Stick s, uint16_t x, uint16_t y) {
  if (s == Stick::Left) {
    _lx = x;
    _ly = y;
  } else {
    _rx = x;
    _ry = y;
  }
}

void Player::applyAxis(Stick s, Axis a, uint16_t v) {
  if (s == Stick::Left) {
    if (a == Axis::X)
      _lx = v;
    else
      _ly = v;
  } else {
    if (a == Axis::X)
      _rx = v;
    else
      _ry = v;
  }
}

void Player::restartMain() {
  neutral();
  _steps = _main;
  _count = _mainCount;
  _index = 0;
  _timing = false;
  _hasRelease = false;
  _inInterrupt = false;
  _seqStart = millis();
}

void Player::enterInterrupt(unsigned long now) {
  neutral();
  _steps = _interrupt;
  _count = _interruptCount;
  _index = 0;
  _timing = false;
  _hasRelease = false;
  _inInterrupt = true;
  _seqStart = now;
}

void Player::start() {
  neutral();
  _steps = _main;
  _count = _mainCount;
  _index = 0;
  _timing = false;
  _hasRelease = false;
  _paused = false;
  _inInterrupt = false;
  _interruptPaused = false;
  _seqStart = millis();
  _state = State::Running;
}

void Player::reset() {
  neutral();
  _steps = _main;
  _count = _mainCount;
  _index = 0;
  _timing = false;
  _hasRelease = false;
  _paused = false;
  _inInterrupt = false;
  _interruptPaused = false;
  _state = State::Idle;
}

void Player::pause() {
  if (_state != State::Running || _paused) return;
  _paused = true;
  _pauseStart = millis();
}

void Player::resume() {
  if (!_paused) return;
  _paused = false;
  _interruptPaused = false;
  // Push the in-progress timer + sequence clock forward by the paused span so
  // no time is lost (the interrupt predicate's elapsedMs stays continuous too).
  const unsigned long paused = millis() - _pauseStart;
  if (_timing) _timerStart += paused;
  _seqStart += paused;
}

bool Player::update(procon::Input& in) {
  if (_state != State::Running) return false;

  // While paused, stream a neutral controller (nothing held down) without
  // advancing timers or losing the accumulated state -- resume() picks the
  // held buttons / sticks back up exactly where they left off.
  if (_paused) {
    procon::Input neutralIn;
    in = neutralIn;
    return false;
  }

  const unsigned long now = millis();

  // Condition-driven abort: while the main sequence runs, poll the interrupt
  // predicate each tick. On a fire, neutralise and switch to the interrupt
  // (reset) sequence, which then runs to completion uninterrupted.
  if (!_inInterrupt && _interruptFn) {
    const TickContext ctx{_rumbleL, _rumbleR, (uint32_t)(now - _seqStart)};
    if (_interruptFn(ctx)) {
      enterInterrupt(now);
      writeState(in);
      return false;
    }
  }

  // Finish an in-progress timed op (Wait / Tap) once its duration elapses.
  if (_timing) {
    if (now - _timerStart < _timerMs) {
      writeState(in);
      return false;  // still holding
    }
    _timing = false;
    if (_hasRelease) applyUp(_releaseChannel);
    _index++;
  }

  // Consume zero-duration ops until the next timed op or the end of the table.
  while (_index < _count) {
    const Step& s = _steps[_index];
    switch (s.op) {
      case Op::Down:
        applyDown(s.channel);
        _index++;
        break;
      case Op::Up:
        applyUp(s.channel);
        _index++;
        break;
      case Op::SetStick:
        applyStick(s.stick, s.x, s.y);
        _index++;
        break;
      case Op::SetAxis:
        applyAxis(s.stick, s.axis, s.x);
        _index++;
        break;
      case Op::Wait:
        _timing = true;
        _hasRelease = false;
        _timerStart = now;
        _timerMs = s.ms;
        writeState(in);
        return false;
      case Op::Tap:
        applyDown(s.channel);
        _timing = true;
        _hasRelease = true;
        _releaseChannel = s.channel;
        _timerStart = now;
        _timerMs = s.ms;
        writeState(in);
        return false;
    }
  }

  // End of the active sequence. If this was the interrupt (reset) sequence, hand
  // control back to the main sequence: loop mode restarts it (parked paused when
  // setPauseAfterInterrupt is armed), otherwise stop.
  if (_inInterrupt) {
    neutral();
    writeState(in);
    if (_loop) {
      restartMain();
      if (_pauseAfterInterrupt) {
        // Rearmed at the first step but held until resume() -- the GPC's
        // "death turns the macro off after reset_sequence" behaviour.
        _paused = true;
        _interruptPaused = true;
        _pauseStart = now;
      }
      return false;
    }
    _inInterrupt = false;
    _state = State::Done;
    return true;
  }

  // End of the main sequence. Loop mode restarts from the top (neutralised) and
  // keeps running; otherwise neutralise and report completion this tick.
  neutral();
  writeState(in);
  if (_loop) {
    _index = 0;
    _timing = false;
    _hasRelease = false;
    _seqStart = now;
    return false;
  }
  _state = State::Done;
  return true;
}

}  // namespace macro
