#include "macros/boulder_macro.h"

#include "engine.h"

namespace boulder_macro {

namespace {
// Placeholder sequence (NOT the real Lenne's Rise routine): D-pad LEFT tap ->
// 1 s wait -> D-pad RIGHT tap, then neutral. Authoring a new macro is just
// copying this table and editing the steps -- see engine.h for the full list
// of channels and factory helpers.
static constexpr macro::Step kSequence[] = {
    macro::Tap(macro::Channel::Left, 100),
    macro::Wait(1000),
    macro::Tap(macro::Channel::Right, 100),
};

macro::Player gPlayer(kSequence);
}  // namespace

void start() { gPlayer.start(); }

void reset() { gPlayer.reset(); }

bool update(procon::Input& in) { return gPlayer.update(in); }

bool isRunning() { return gPlayer.isRunning(); }

bool isDone() { return gPlayer.isDone(); }

}  // namespace boulder_macro
