#include "macros/boulder_macro.h"

#include "engine.h"
#include "procon/procon_reports.h"

namespace boulder_macro {

namespace {
using namespace macro;

// Timings ported from the reference Cronus Zen GPC (Elden Ring - Boulder Chase
// Farm at Lenne's Rise). See the gist linked in issue #6.
constexpr uint32_t TORRENT_TIME = 1940;   // wait for Torrent before turning
constexpr uint32_t TURN_TIME = 1475;      // turn to the correct path
constexpr uint32_t RUN_TIME = 10480;      // reach the ball spawn point
constexpr uint32_t DODGE_TIME = 400;      // delay before dodging the ball
constexpr uint32_t BALL_DROP_TIME = 2000; // wait until the reward drops
constexpr uint32_t RESET_TIME = 7000;     // reload the Site of Grace

// Button mapping note: the reference targets a PS5 pad; this firmware presents a
// Switch Pro Controller. The PS->Pro face-button mapping used below is
//   TRIANGLE->X, CIRCLE->A, CROSS->B, and the PS touchpad reset->Minus.
// Forward on the left stick is +Y (procon::kStickMax); tune on hardware.
constexpr uint16_t kFwd = procon::kStickMax;      // left stick up / forward
constexpr uint16_t kRight = procon::kStickMax;    // left stick right
constexpr uint16_t kCenter = procon::kStickCenter;

// main_sequence: the full happy-path farm cycle (run out, dodge the boulder,
// collect the reward, then reload the Site of Grace), looped continuously.
constexpr Step kMainSequence[] = {
    // Summon Torrent (PS5 TRIANGLE + UP).
    Down(Channel::X), Down(Channel::Up), Wait(100), Up(Channel::Up),
    Up(Channel::X),
    Wait(TORRENT_TIME),

    // Turn onto the path: hold right on LX and sprint (CIRCLE -> A).
    StickAxis(Stick::Left, Axis::X, kRight), Down(Channel::A), Wait(TURN_TIME),

    // Run forward: LY up, recentre LX, keep sprinting.
    StickAxis(Stick::Left, Axis::Y, kFwd),
    StickAxis(Stick::Left, Axis::X, kCenter), Wait(RUN_TIME),
    StickAxis(Stick::Left, Axis::Y, kCenter), Up(Channel::A),

    // Line up the dodge.
    StickAxis(Stick::Left, Axis::X, kRight), Wait(500),
    StickAxis(Stick::Left, Axis::X, kCenter),
    Wait(DODGE_TIME),
    StickAxis(Stick::Left, Axis::X, kRight), Wait(500),

    // Dodge-roll spam (CIRCLE -> A) to slip past the boulder.
    Tap(Channel::A, 100), Wait(100),
    Tap(Channel::A, 100), Wait(100),
    Tap(Channel::A, 100), Wait(100),
    Tap(Channel::A, 200), Wait(100),
    Tap(Channel::A, 2000),
    StickAxis(Stick::Left, Axis::X, kCenter),

    // Wait for the reward to drop.
    Wait(BALL_DROP_TIME),

    // Reset: open the map / reload the Site of Grace (PS touchpad -> Minus,
    // then confirm through the teleport prompt with X then B).
    Tap(Channel::Minus, 100),
    Wait(500),
    Tap(Channel::X, 100), Wait(100),
    Tap(Channel::B, 100),
    Wait(300),
    Tap(Channel::B, 100),
    Wait(RESET_TIME),
};

// reset_sequence: the death-recovery path. When the game rumbles hard (the
// player fell / died) the interrupt aborts main_sequence mid-flight and runs
// this to reload the Site of Grace before the loop resumes -- exactly the GPC's
// presumeDead -> reset_sequence behaviour.
constexpr Step kResetSequence[] = {
    Tap(Channel::Minus, 100),
    Wait(500),
    Tap(Channel::X, 100), Wait(100),
    Tap(Channel::B, 100),
    Wait(300),
    Tap(Channel::B, 100),
    Wait(RESET_TIME),
};

// Death detection: fire when either actuator crosses the rumble threshold.
bool deathDetected(const TickContext& c) {
  return c.rumbleLeft >= procon::kRumbleMin || c.rumbleRight >= procon::kRumbleMin;
}

macro::Player makePlayer() {
  macro::Player p(kMainSequence);
  p.setLoop(true);  // repeat the farm cycle until the run is stopped
  p.setInterrupt(deathDetected, kResetSequence);
  return p;
}

macro::Player gPlayer = makePlayer();
}  // namespace

void start() { gPlayer.start(); }

void reset() { gPlayer.reset(); }

bool update(procon::Input& in) { return gPlayer.update(in); }

void feedRumble(uint16_t left, uint16_t right) {
  gPlayer.feedRumble(left, right);
}

bool isDeathDetected() { return gPlayer.isInterrupting(); }

bool isRunning() { return gPlayer.isRunning(); }

bool isDone() { return gPlayer.isDone(); }

void pause() { gPlayer.pause(); }

void resume() { gPlayer.resume(); }

bool isPaused() { return gPlayer.isPaused(); }

}  // namespace boulder_macro
