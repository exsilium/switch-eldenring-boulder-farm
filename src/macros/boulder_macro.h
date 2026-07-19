#pragma once

#include "procon/procon_reports.h"

// Boulder-farm input macro for Elden Ring.
//
// Macros live in their own files under src/macros/ and hold *only* a macro
// definition -- the reusable runtime is the declarative engine in engine.h.
// Each macro exposes the same small interface (start / reset / update /
// isRunning / isDone) so the runner in main.cpp can drive whichever macro is
// selected from the menu.
//
// A macro is authored as a flat, readable `macro::Step` table (each step with
// its own timing) plus a file-local `macro::Player` that the shims below
// forward to. To add a new macro, copy boulder_macro.cpp, rename the namespace,
// and edit the step table -- no engine changes required. See engine.h for the
// full channel list (buttons, D-pad, both analog sticks) and factory helpers
// (Down / Up / Wait / Tap / StickMove / StickCenter).
//
// Current (placeholder) sequence: D-pad LEFT tap -> 1 s wait -> D-pad RIGHT
// tap, looped continuously. Replace kSequence in boulder_macro.cpp with the
// real farm routine.
namespace boulder_macro {

// Begin the macro from the start.
void start();

// Force back to an inert, neutral state.
void reset();

// Drive the macro. Writes the current input state into `in`. Returns true on
// the iteration the macro completes. Safe to call when not running (no-op).
bool update(procon::Input& in);

bool isRunning();
bool isDone();

// The macro loops forever while running, so use these to control a run:
// pause() freezes the current inputs in place, resume() continues, and
// reset() stops it entirely and neutralises the controller.
void pause();
void resume();
bool isPaused();

}  // namespace boulder_macro
