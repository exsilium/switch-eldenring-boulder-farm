#include "macros/boulder_macro.h"

#include "engine.h"
#include "procon/procon_reports.h"

namespace boulder_macro {

namespace {
using namespace macro;

// Timings ported 1:1 from the reference Cronus Zen GPC v1.1.0 (Elden Ring -
// Boulder Chase Farm at Lenne's Rise). See the gist linked in issue #8.
constexpr uint32_t TORRENT_TIME = 2040;    // wait for Torrent before turning
constexpr uint32_t TURN_TIME = 1490;       // turn with Torrent to the path
constexpr uint32_t RUN_HALF_TIME = 5240;   // reach half-way for course correction
constexpr uint32_t RUN_END_TIME = 5040;    // reach the ball spawn point
constexpr uint32_t DODGE_TIME = 300;       // delay before dodging the ball
constexpr uint32_t BALL_DROP_TIME = 3800;  // wait until the reward drops
constexpr uint32_t RESET_TIME = 7000;      // reload the Site of Grace

// Button mapping note: the reference targets a PS5 pad; this firmware presents
// a Switch Pro Controller, so buttons translate to the game's default Pro
// Controller bindings (issue #8):
//   TRIANGLE -> Y  (pickup; Y+UP summons Torrent)
//   CIRCLE   -> B  (evade / sprint)
//   CROSS    -> A  (confirm)
//   TOUCHPAD -> Minus  (map)
//
// Stick mapping note: the GPC drives PS5_LX / PS5_LY in -100..100 percent where
// LY = -100 is forward (up). Sticks here are 12-bit around kStickCenter with +Y
// = up, so GPC percentages convert via pct() with the Y sign flipped.
constexpr uint16_t pct(int v) {
  return (uint16_t)((int)procon::kStickCenter + v * 0x7FF / 100);
}
constexpr uint16_t kFwd = pct(100);      // GPC LY = -100 (forward / up)
constexpr uint16_t kRight = pct(100);    // GPC LX = +100
constexpr uint16_t kLeft40 = pct(-40);   // GPC LX = -40 (mid-run correction)
constexpr uint16_t kCenter = procon::kStickCenter;

// reset_sequence (shared step list): open the map (Minus), select the Site of
// Grace marker (Y), confirm travel (A), confirm again (A), then wait for the
// loading screen -- the GPC's TOUCH -> TRIANGLE -> CROSS -> CROSS routine. Kept
// as a macro so the tail of the main cycle and the death-interrupt sequence
// can't drift apart.
#define BOULDER_RESET_STEPS                              \
  Tap(Channel::Minus, 100), Wait(500),                   \
  Tap(Channel::Y, 100), Wait(100),                       \
  Tap(Channel::A, 100), Wait(300),                       \
  Tap(Channel::A, 100), Wait(RESET_TIME)

// main_sequence: the full happy-path farm cycle (summon Torrent, ride out,
// dodge the boulder, collect the reward, then reload the Site of Grace),
// looped continuously. Each block mirrors one set_val/wait block of the GPC.
constexpr Step kMainSequence[] = {
    // Summon Torrent: hold Y + UP (GPC TRIANGLE + UP) while waiting for the
    // mount animation.
    Down(Channel::Y), Down(Channel::Up), Wait(TORRENT_TIME),
    Up(Channel::Y), Up(Channel::Up),

    // Turn onto the path: hold LX right and sprint (CIRCLE -> B).
    StickAxis(Stick::Left, Axis::X, kRight), Down(Channel::B), Wait(TURN_TIME),

    // Run forward to the half-way point: LY forward, LX recentred, keep
    // sprinting.
    StickAxis(Stick::Left, Axis::X, kCenter),
    StickAxis(Stick::Left, Axis::Y, kFwd), Wait(RUN_HALF_TIME),

    // Half-way point: make a small correction to the left.
    StickAxis(Stick::Left, Axis::X, kLeft40), Wait(100),

    // Run until the spawning point.
    StickAxis(Stick::Left, Axis::X, kCenter), Wait(RUN_END_TIME),

    // Veer right off the path (sprint released, GPC drops CIRCLE here).
    Up(Channel::B), StickAxis(Stick::Left, Axis::Y, kCenter),
    StickAxis(Stick::Left, Axis::X, kRight), Wait(500),

    // Wait for the ball...
    StickAxis(Stick::Left, Axis::X, kCenter), Wait(DODGE_TIME),

    // ...and dodge right.
    StickAxis(Stick::Left, Axis::X, kRight), Wait(500),
    StickAxis(Stick::Left, Axis::X, kCenter),

    // Evade volley (GPC CIRCLE -> B): five 200 ms presses, 200 ms apart.
    Tap(Channel::B, 200), Wait(200),
    Tap(Channel::B, 200), Wait(200),
    Tap(Channel::B, 200), Wait(200),
    Tap(Channel::B, 200), Wait(200),
    Tap(Channel::B, 200),

    // Acquire materials (GPC TRIANGLE -> Y).
    Tap(Channel::Y, 200),

    // Wait for the reward to drop.
    Wait(BALL_DROP_TIME),

    // Reset: reload the Site of Grace to begin a new run.
    BOULDER_RESET_STEPS,
};

// reset_sequence as the interrupt table: when the game rumbles hard (the
// player fell / died) the interrupt aborts main_sequence mid-flight and runs
// this to reload the Site of Grace before the loop resumes -- exactly the
// GPC's presumeDead -> reset_sequence behaviour.
constexpr Step kResetSequence[] = {
    BOULDER_RESET_STEPS,
};

// Death detection: fire when either actuator crosses the rumble threshold.
bool deathDetected(const TickContext& c) {
  return c.rumbleLeft >= procon::kRumbleMin || c.rumbleRight >= procon::kRumbleMin;
}

macro::Player makePlayer() {
  macro::Player p(kMainSequence);
  p.setLoop(true);  // repeat the farm cycle until the run is stopped
  p.setInterrupt(deathDetected, kResetSequence);
  // After a death-triggered reset the run parks paused (rearmed at the first
  // step) until the user resumes -- the GPC turns the macro off after
  // reset_sequence rather than blindly re-running.
  p.setPauseAfterInterrupt(true);
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

bool isDeathDetected() {
  // Covers both the reset routine itself and the parked-paused state after it,
  // so the UI's "Death Detected" label stays up until the user resumes.
  return gPlayer.isInterrupting() || gPlayer.isInterruptPaused();
}

bool isRunning() { return gPlayer.isRunning(); }

bool isDone() { return gPlayer.isDone(); }

void pause() { gPlayer.pause(); }

void resume() { gPlayer.resume(); }

bool isPaused() { return gPlayer.isPaused(); }

}  // namespace boulder_macro
