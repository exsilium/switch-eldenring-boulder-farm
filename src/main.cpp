#include <Arduino.h>

#include "app_state.h"
#include "button.h"
#include "display.h"
#include "input_macro.h"
#include "procon/procon_usb.h"

// ---------------------------------------------------------------------------
// Switch Pro Controller emulation — Adafruit Feather ESP32-S3 TFT
//
// A 2 s GPIO0 (BOOT) long-press toggles USB attach/detach. On connect the
// firmware answers the Switch handshake, runs a one-shot D-pad LEFT -> 1s ->
// RIGHT macro, then holds neutral. The TFT is the status/debug surface (USB
// CDC serial is disabled).
// ---------------------------------------------------------------------------

namespace {
AppState gState = AppState::IDLE_DETACHED;
bool gMacroDone = false;  // macro fires exactly once per connection

void setState(AppState s) { gState = s; }
}  // namespace

void setup() {
  button::begin();
  display::begin();
  procon::begin();  // configures USB HID, starts detached

  setState(AppState::IDLE_DETACHED);
  display::showState(gState);
}

void loop() {
  // Service USB on every iteration so the host never sees a silent endpoint.
  procon::task();

  const bool longPress = button::update();

  switch (gState) {
    case AppState::IDLE_DETACHED:
      if (longPress) {
        gMacroDone = false;
        input_macro::reset();
        procon::attach();
        setState(AppState::ATTACHING);
      }
      break;

    case AppState::ATTACHING:
      // Long-press during a transition cancels back to detached.
      if (longPress) {
        procon::detach();
        setState(AppState::DETACHING);
      } else if (procon::mounted()) {
        setState(AppState::HANDSHAKING);
      }
      break;

    case AppState::HANDSHAKING:
      if (longPress || !procon::mounted()) {
        procon::detach();
        setState(AppState::DETACHING);
      } else if (procon::handshakeStarted()) {
        if (gMacroDone) {
          setState(AppState::CONNECTED_IDLE);
        } else {
          input_macro::start();
          setState(AppState::RUN_MACRO);
        }
      }
      break;

    case AppState::RUN_MACRO:
      if (longPress || !procon::mounted()) {
        input_macro::reset();
        procon::detach();
        setState(AppState::DETACHING);
      } else if (input_macro::update(procon::input())) {
        gMacroDone = true;
        setState(AppState::CONNECTED_IDLE);
      }
      break;

    case AppState::CONNECTED_IDLE:
      if (longPress || !procon::mounted()) {
        procon::detach();
        setState(AppState::DETACHING);
      }
      break;

    case AppState::DETACHING:
      procon::input().reset();
      input_macro::reset();
      setState(AppState::IDLE_DETACHED);
      break;
  }

  display::showState(gState);

  // Live USB/handshake trace on the TFT (the only debug surface) from the
  // moment we start attaching, so an enumeration stall is visible: MNT shows
  // host mount, and the OUT count climbs once the host actually talks to us.
  if (gState == AppState::ATTACHING || gState == AppState::HANDSHAKING ||
      gState == AppState::RUN_MACRO || gState == AppState::CONNECTED_IDLE) {
    const procon::Protocol::Diag& d = procon::diag();
    display::showDiag(procon::mounted(), d.inCount, d.outCount, d.lastSubcommand,
                      d.gotDeviceInfo, d.gotStickCal, d.gotSetMode,
                      d.gotVibration);
  }

  // The long-press toggles attach/detach in every state, so show the arming
  // bar filling whenever BOOT is held, and clear it the moment it's released.
  display::showHoldProgress(button::isDown() ? button::holdProgress() : 0.0f);
}