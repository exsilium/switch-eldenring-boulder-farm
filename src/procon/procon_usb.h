#pragma once

#include <stdint.h>

#include "procon_protocol.h"
#include "procon_reports.h"

// Adafruit TinyUSB glue for the emulated Pro Controller.
namespace procon {

// Configure the USB HID device (descriptor, VID/PID, strings, callbacks) and
// start the stack detached. Call once from setup().
void begin();

// Present the device on the bus (enumerate as 057E:2009).
void attach();

// Remove the device from the bus.
void detach();

// True once the host has enumerated/mounted the HID interface.
bool mounted();

// True once the handshake has progressed far enough that the host queried
// device info (a good "connection established" signal).
bool handshakeStarted();

// On-device handshake trace for the TFT debug surface.
const Protocol::Diag& diag();

// Pump the USB IN endpoint: send the next report whenever the endpoint is
// ready. Call frequently from loop().
void task();

// Access to the live input state (buttons / sticks) shared with the macro.
Input& input();

// Reset per-connection protocol state (timers, handshake flags). Call on each
// fresh attach so a reconnect starts clean.
void resetProtocol();

}  // namespace procon
