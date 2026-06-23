#pragma once

#include "procon/procon_reports.h"

// Non-blocking D-pad macro: LEFT -> 1 s gap -> RIGHT, then neutral.
// Runs once per invocation; all timing is millis()-based (never delay()).
namespace input_macro {

// Begin the macro from the start.
void start();

// Force back to an inert, neutral state.
void reset();

// Drive the macro. Writes the current D-pad state into `in`. Returns true on
// the iteration the macro completes. Safe to call when not running (no-op).
bool update(procon::Input& in);

bool isRunning();
bool isDone();

}  // namespace input_macro
