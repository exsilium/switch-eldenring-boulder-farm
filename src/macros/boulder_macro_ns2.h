#pragma once

#include "procon/procon_reports.h"

// Boulder-farm input macro for Elden Ring -- Nintendo Switch 2 edition.
//
// Same Lenne's Rise routine and timings as boulder_macro.h, re-bound to the
// Switch 2 release's default Pro Controller layout:
//   summon Torrent -> X + UP   (was Y + UP)
//   evade / sprint -> A        (was B)
//   pickup         -> X        (was Y)
//   reset          -> Minus, X, A, A  (was Minus, Y, A, A)
//
// The PC-layout macro in boulder_macro.cpp is left untouched; the menu offers
// both and the runner in main.cpp binds to whichever was selected.
namespace boulder_macro_ns2 {

// Begin the macro from the start.
void start();

// Force back to an inert, neutral state.
void reset();

// Drive the macro. Writes the current input state into `in`. Returns true on
// the iteration the macro completes. Safe to call when not running (no-op).
bool update(procon::Input& in);

// Feed the current host-rumble amplitudes (per side, 0..255) so the macro's
// death-detection interrupt can fire. Call once per tick before update().
void feedRumble(uint16_t left, uint16_t right);

// True while the death-recovery (reset) sequence is running -- or while the
// run is parked paused after it -- following a rumble spike that aborted the
// main farm loop.
bool isDeathDetected();

bool isRunning();
bool isDone();

// The macro loops forever while running, so use these to control a run:
// pause() releases the controller to neutral while keeping the run's progress
// frozen aside, resume() re-asserts the held inputs and continues, and
// reset() stops it entirely and neutralises the controller.
void pause();
void resume();
bool isPaused();

}  // namespace boulder_macro_ns2
