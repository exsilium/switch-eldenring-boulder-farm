#include "engine.h"

#include "esp_timer.h"

namespace macro {

// IDF replacement for Arduino millis() (the engine's only framework dependency).
unsigned long Player::millis() {
  return (unsigned long)(esp_timer_get_time() / 1000);
}

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

void Player::start() {
  neutral();
  _index = 0;
  _timing = false;
  _hasRelease = false;
  _state = State::Running;
}

void Player::reset() {
  neutral();
  _index = 0;
  _timing = false;
  _hasRelease = false;
  _state = State::Idle;
}

bool Player::update(procon::Input& in) {
  if (_state != State::Running) return false;

  const unsigned long now = millis();

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

  // End of sequence: neutralise everything and report completion this tick.
  neutral();
  writeState(in);
  _state = State::Done;
  return true;
}

}  // namespace macro
