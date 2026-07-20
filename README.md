# switch-eldenring-boulder-farm

[![Build](https://github.com/exsilium/switch-eldenring-boulder-farm/actions/workflows/build.yml/badge.svg)](https://github.com/exsilium/switch-eldenring-boulder-farm/actions/workflows/build.yml)

Nintendo Switch Pro Controller emulation firmware for the **Adafruit Feather
ESP32-S3 TFT**, built with **PlatformIO + ESP-IDF** and raw TinyUSB.

## Building

### Option A — Docker (no host toolchain required)

The firmware builds reproducibly inside a Docker container, so you don't need
PlatformIO, the ESP-IDF toolchain, or the managed components installed locally.
Docker is the only prerequisite.

**Linux / macOS:**

```sh
./build.sh
```

**Windows (PowerShell):**

```powershell
.\build.ps1
```

**Windows (cmd):**

```bat
build.cmd
```

Each launcher builds the `switch-firmware-builder` image and runs the
containerized `pio run -e feather_s3_idf`, bind-mounting the repo so build
outputs land back on the host under `.pio/build/feather_s3_idf/`
(`firmware.elf`, `firmware.bin`).

Extra arguments are forwarded to `pio run`, e.g. `./build.sh -t upload`
(local flashing needs USB device passthrough into the container).

> The first (cold) build is slow because PlatformIO downloads the `espressif32`
> platform, the ESP-IDF toolchain and the managed components. The platform and
> toolchain are cached in the Docker image layer, so subsequent builds only
> recompile changed sources.

### Option B — Native PlatformIO

If you already have [PlatformIO Core](https://docs.platformio.org/en/latest/core/)
installed you can skip Docker entirely:

```sh
pio run -e feather_s3_idf
```

Upload (put the board in bootloader — hold **BOOT**, tap **RESET**):

```sh
pio run -e feather_s3_idf -t upload
```

## Writing a macro

Input macros are **declarative, GPC-style command streams**. A macro is a flat,
readable `macro::Step` table where every step carries its own timing, so you
never touch a phase state machine or poke button bits by hand. The reusable
runtime lives in [`src/engine.h`](src/engine.h) / [`src/engine.cpp`](src/engine.cpp);
the files under [`src/macros/`](src/macros) contain **only** macro definitions.

### The engine at a glance

Author steps with these `constexpr` factory helpers (from `src/engine.h`):

| Helper | Meaning |
| --- | --- |
| `Down(Channel)` | Press a channel (bit set), no delay. Overlap-friendly. |
| `Up(Channel)` | Release a channel (bit clear), no delay. |
| `Wait(ms)` | Hold the current accumulated state for `ms` milliseconds. |
| `Tap(Channel, ms)` | Convenience for `Down(c)`, `Wait(ms)`, `Up(c)`. |
| `StickMove(Stick, x, y)` | Set one analog stick instantly (12-bit, centre `0x800`). |
| `StickAxis(Stick, Axis, value)` | Set a single stick axis, holding the other at its current value. |
| `StickCenter(Stick)` | Recentre one analog stick. |

Durations are real milliseconds (measured with the `esp_timer` hardware clock,
independent of the FreeRTOS tick rate). Multiple `Down()`s before an `Up()` hold
several inputs simultaneously.

`Channel` covers the full Pro Controller — every button, the D-pad, and both
stick clicks:

```
Buttons : A B X Y  L R ZL ZR  Minus Plus Home Capture  LStick RStick
D-pad   : Up Down Left Right
Sticks  : StickMove(Stick::Left/Right, x, y), StickCenter(Stick::Left/Right)
```

(`Stick::Left` / `Stick::Right` select which analog stick a stick op targets.
The full byte/bit table for each `Channel` is documented in `src/engine.h`.)

### Feedback-driven interrupts (rumble / "death detection")

Beyond the linear step table, a macro can react to controller feedback and
abort mid-sequence. The firmware decodes the host's HD-rumble output into a
per-side amplitude (`0..255`, left ≈ `RUMBLE_A`, right ≈ `RUMBLE_B`; see
`procon::Protocol::rumbleLeft()` / `rumbleRight()` and
`procon::decodeRumbleAmplitude` / `procon::kRumbleMin`). The run loop feeds those
values into the player each tick with `feedRumble(left, right)`, so the engine
never reaches into the USB layer itself.

Arm a condition-driven abort with `setInterrupt(pred, resetSeq)`. While the main
sequence runs, `pred(const macro::TickContext&)` is polled every tick; when it
returns `true` the controller is neutralised, the **interrupt (reset) sequence**
runs to completion, and then the main sequence resumes (loop mode) or the run
stops (one-shot). This maps 1:1 onto the reference GPC's
`presumeDead → reset_sequence` behaviour. `TickContext` carries `rumbleLeft`,
`rumbleRight`, and `elapsedMs` (time since the active sequence started).

```cpp
static constexpr macro::Step kMain[]  = { /* ... farm loop ... */ };
static constexpr macro::Step kReset[] = { /* ... reload the Site of Grace ... */ };

// Fire when either actuator crosses the death threshold.
bool deathDetected(const macro::TickContext& c) {
  return c.rumbleLeft  >= procon::kRumbleMin ||
         c.rumbleRight >= procon::kRumbleMin;
}

macro::Player makePlayer() {
  macro::Player p(kMain);
  p.setLoop(true);
  p.setInterrupt(deathDetected, kReset);   // abort main + run kReset on death
  return p;
}
```

Expose `feedRumble()` and `isInterrupting()` through the macro's namespace so the
runner can feed rumble each tick (before `update()`) and surface the state.

### On-screen run status label

`ui::setRunStatus(const char *text)` writes a short label onto the RUNNING
overlay (thread-safe; a no-op unless that overlay is visible; pass `""` to
clear). The shipped Boulder macro shows `"Death Detected"` while its interrupt
runs and clears it on (re)start. The overlay also draws live per-side rumble
meters (fed by `ui::setRumble(left, right)`) that turn red past `kRumbleMin`,
and a cassette-style play/pause glyph in place of the old RUNNING/PAUSED words.



Copy [`src/macros/boulder_macro.{h,cpp}`](src/macros) to a new name (e.g.
`cinder_macro`), rename the namespace, and edit the `kSequence` table. The
public interface (`start` / `reset` / `update` / `isRunning` / `isDone` /
`pause` / `resume` / `isPaused`) stays identical, so the runner can drive any
macro the same way.

```cpp
// src/macros/cinder_macro.cpp
#include "macros/cinder_macro.h"
#include "engine.h"

namespace cinder_macro {
namespace {
static constexpr macro::Step kSequence[] = {
    macro::Tap(macro::Channel::A, 80),
    macro::Wait(500),
    macro::StickMove(macro::Stick::Left, procon::kStickMax, procon::kStickCenter),
    macro::Wait(1000),
    macro::StickCenter(macro::Stick::Left),
};

macro::Player makePlayer() {
  macro::Player p(kSequence);
  p.setLoop(true);   // repeat until the run is stopped; omit for a one-shot
  return p;
}
macro::Player gPlayer = makePlayer();
}  // namespace

void start()  { gPlayer.start(); }
void reset()  { gPlayer.reset(); }
bool update(procon::Input& in) { return gPlayer.update(in); }
bool isRunning() { return gPlayer.isRunning(); }
bool isDone()    { return gPlayer.isDone(); }
void pause()  { gPlayer.pause(); }
void resume() { gPlayer.resume(); }
bool isPaused() { return gPlayer.isPaused(); }
}  // namespace cinder_macro
```

### 2. Register it in the build

Add the new `.cpp` to `SRCS` in [`src/CMakeLists.txt`](src/CMakeLists.txt):

```cmake
    SRCS
        "main.cpp"
        "engine.cpp"
        "macros/boulder_macro.cpp"
        "macros/cinder_macro.cpp"   # <-- add
```

### 3. Add a menu entry

The one-button menu is defined in [`src/ui/display.cpp`](src/ui/display.cpp).
To add a row:

1. Grow the menu arrays and count:

   ```cpp
   constexpr int kMenuCount = 4;                                  // was 3
   const char *kMenuLabels[kMenuCount] =
       {"Run Boulder", "Run Cinder", "Reattach USB", "Back"};
   ```
   and widen `gMenuRow[]` to match `kMenuCount`.

2. Handle the new selection index in `activateMenu()`. Reuse the shared run
   flow — raising `Command::RunMacro` starts a run and opens the RUNNING
overlay (short tap = pause/resume, long hold = stop and return to the menu).
The RUNNING overlay draws a live Pro Controller diagram that highlights the
buttons the macro is pressing and moves the analog stick dots in real time:

   ```cpp
   void activateMenu() {
     switch (gSel) {
       case 0: gPending = Command::RunMacro;  gRunMacroId = 0; openRun(); break;
       case 1: gPending = Command::RunMacro;  gRunMacroId = 1; openRun(); break;
       case 2: gPending = Command::Reattach;  closeMenu();     break;
       case 3: closeMenu();                                    break;
     }
   }
   ```

   If a menu item needs behaviour other than running a macro, add a value to the
   `Command` enum in [`src/ui/display.h`](src/ui/display.h) and act on it in
   step 4.

### 4. Run it from `app_loop_task`

The run lifecycle is handled in `app_loop_task` in
[`src/main.cpp`](src/main.cpp), which reacts to the menu commands:

- `Command::RunMacro` — `start()` the macro and set `gMacroRunning = true`.
- `Command::TogglePause` — `pause()` / `resume()` the active macro.
- `Command::StopMacro` — clear `gMacroRunning` and `reset()` (neutralises the
  controller). Raised by the long-hold exit.

`stream_task` calls the active macro's `update()` each tick only while
`gMacroRunning` is set, so the macro drives the controller only during an
explicit run.

Because every macro shares the same interface, the simplest way to make several
macros selectable is a tiny "active macro" indirection — e.g. store the chosen
macro's function pointers (or a small `switch` on an id like `gRunMacroId`) and
call through it in the `RunMacro` / `TogglePause` / `StopMacro` handlers. With a
single macro you can just call `boulder_macro::…` directly, as the shipped code
does.

## Continuous integration

[`.github/workflows/build.yml`](.github/workflows/build.yml) runs two jobs on
every push, pull request and manual dispatch:

- **Host unit tests** — `pio test -e native` runs the dependency-light engine +
  rumble-decode tests under [`test/`](test) on the PlatformIO `native` platform
  (no hardware/ESP-IDF needed). See [`test/test_engine`](test/test_engine) and
  [`test/test_rumble`](test/test_rumble); the host build compiles only
  `src/engine.cpp` (via `build_src_filter`) with an injected virtual clock.
- **Docker firmware build** — builds the Docker image and runs the containerized
  firmware build, then uploads `firmware.elf` / `firmware.bin` as workflow
  artifacts. The workflow reclaims runner disk space before building because the
  builder image bundles the full ESP-IDF toolchain (~10 GB).

