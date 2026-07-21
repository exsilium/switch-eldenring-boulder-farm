#pragma once

// Top-level application state machine.
//
//   IDLE_DETACHED ──(long-press)──▶ ATTACHING ──(mounted)──▶ HANDSHAKING
//                                                               │ (init done)
//                                                               ▼
//   IDLE_DETACHED ◀─(long-press)── DETACHING        RUN_MACRO ─(done)─▶ CONNECTED_IDLE
//        ▲                              ▲                                     │
//        └──────────────────────────────┴───────────(long-press)─────────────┘
enum class AppState {
  IDLE_DETACHED,   // powered, USB detached, waiting for long-press
  ATTACHING,       // TinyUSBDevice.attach() called, waiting for host mount
  HANDSHAKING,     // mounted, answering 0x80 + subcommands
  RUN_MACRO,       // boulder-farm macro run active (started from the menu)
  CONNECTED_IDLE,  // macro done, holding neutral 0x30 on the poll path
  DETACHING        // TinyUSBDevice.detach() called, returning to idle
};

inline const char* appStateName(AppState s) {
  switch (s) {
    case AppState::IDLE_DETACHED:  return "DETACHED";
    case AppState::ATTACHING:      return "ATTACHING";
    case AppState::HANDSHAKING:    return "HANDSHAKING";
    case AppState::RUN_MACRO:      return "MACRO";
    case AppState::CONNECTED_IDLE: return "CONNECTED";
    case AppState::DETACHING:      return "DETACHING";
  }
  return "?";
}
