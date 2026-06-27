#pragma once

#include "procon/procon_reports.h"

// Boulder-farm input macro for Elden Ring.
//
// Macros live in their own files under src/macros/ and each exposes the same
// small interface (start / reset / update / isRunning / isDone) so a new macro
// can be added by copying this pattern. The runner in main.cpp drives whichever
// macro is selected from the menu.
//
// Current (placeholder) sequence: D-pad LEFT -> 1 s gap -> D-pad RIGHT, then
// neutral. Replace the phases in boulder_macro.cpp with the real farm routine.
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

}  // namespace boulder_macro
