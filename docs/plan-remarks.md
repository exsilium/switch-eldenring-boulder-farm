# Plan Remarks — Switch Pro Controller Emulation

Consolidated record of how this project actually played out versus the original
plan ([plan-switchProControllerEmulation.prompt.md](plan-switchProControllerEmulation.prompt.md)),
where the time went, the real root causes, and what the plan was missing.

Captured after the device successfully enumerated on a PC as a Nintendo Switch
Pro Controller on the latest official ESP-IDF stack.

---

## 1. Outcome (current baseline)

- ✅ Enumerates on a **PC** as a Nintendo Switch Pro Controller (`057E`/`2009`).
- ✅ Latest official PlatformIO `espressif32` **7.0.1** (ESP-IDF **6.0.1**), raw
  `esp_tinyusb` **2.2.1**.
- ✅ LVGL/TFT status UI, one-button (BOOT) menu, NeoPixel status, macro module.
- ⏳ Still to validate: Switch 2 acceptance and the real boulder-farm routine.

---

## 2. How the project drifted from the original plan

The original plan was **Arduino + Adafruit TinyUSB**, a flat `src/` layout, a
hand-copied 203-byte descriptor, and a 2 s long-press to attach. The shipped
project differs on almost every axis:

| Area | Original plan | Where it landed |
|---|---|---|
| Framework | Arduino + Adafruit TinyUSB | **ESP-IDF + raw `esp_tinyusb`** |
| USB bring-up | `TinyUSBDevice.attach()` on long-press | `tinyusb_driver_install()` in `app_main`, **auto-attach at boot** |
| Descriptor | Hand-pasted 203-byte array in `procon_descriptor.h` | **Generated from `espp/hid-rp`** (`switch_pro_descriptor()`, 209 bytes) |
| Display | Adafruit-GFX `display.cpp` | **LVGL over `esp_lcd`** (`esp_lvgl_port`) |
| Layout | `input_macro.*` at `src/` root | **`src/macros/boulder_macro.*`** (folder for multiple macros) |
| Status surface | TFT only | TFT **+ NeoPixel** (RED/BLUE/YELLOW/GREEN phase) |
| Toolchain | unspecified | Pinned, then moved to **latest official 7.0.1 / IDF 6.0.1** |

The drift was driven by discovering, one failure at a time, that the plan's
foundational assumptions (Arduino USB, framework defaults) did not hold.

---

## 3. Where the time actually went (the expensive lessons)

### 3a. Arduino framework dead-end
The plan committed to Arduino + Adafruit TinyUSB and even "validated the API
signatures." In practice:
- `TinyUSBDevice.begin()` does **not** initialise the USB hardware on ESP32-S3;
  the arduino-esp32 **core** does that, and only when USB-on-boot is enabled —
  which we deliberately disabled for a clean HID-only device. The stack was
  *linked but never initialised or pumped*, so the host saw nothing.
- The "obvious" fix (call the core's `USB.begin()`) **fails to link**: the
  Adafruit library bundles its own full TinyUSB while the core ships another —
  **duplicate-symbol errors**. The two stacks are mutually exclusive.
- Even after switching to the core `USB`/`USBHID` API (which did enumerate on a
  PC), the Switch **never drove the handshake** (`in:1 out:0`) — the core's
  composite descriptor assembly was the blocker. A minimal raw-TinyUSB **ESP-IDF**
  spike immediately produced an OUT report (NeoPixel GREEN), proving the Arduino
  USB stack itself was the wall. → Migrated the whole project to ESP-IDF.

### 3b. ESP-IDF with the wrong tick rate — *the big one*
After the migration, the device **installed fine but the host reported "USB
device not recognized."** We chased this for a long time across descriptors,
versions, core affinity, and platforms. The actual cause was mundane:
- **`CONFIG_FREERTOS_HZ` defaulted to 100 Hz** (10 ms tick). At that granularity
  the TinyUSB device task cannot service EP0 control transfers inside the host's
  enumeration timeouts → enumeration fails.
- Fix: **`CONFIG_FREERTOS_HZ=1000`** (the working reference runs 1 kHz).
- This single line was the blocking bug. It was hard to find because it is
  invisible in the code — it lives in a config default, and the symptom
  (timeout) looked like a descriptor problem.

### 3c. A custom gamepad/descriptor structure that didn't match the component
The plan defined its own `procon/` structure and a hand-pasted 203-byte
descriptor. The reusable reference component (`espp/hid-rp`) **generates** the
descriptor (`switch_pro_descriptor()`), and it is **209 bytes**, not 203. Time
went into "verifying" a hand-decoded descriptor that was simply the wrong
artifact. Switching to the generated descriptor removed a whole class of
byte-mismatch doubt.

### 3d. Flash vs RAM descriptors (DMA)
On the ESP32-S3, `esp_tinyusb` runs the dwc2 controller in **DMA mode**
(`CONFIG_TINYUSB_MODE_DMA=y`) and hands our descriptor **pointers** straight to
TinyUSB. **Flash (`.rodata`) is not DMA-accessible.** Our descriptors were
`const`/`constexpr` (flash); the reference's were non-const / `std::vector`
(RAM), which is why it worked. Fix: `desc_device` and
`hid_configuration_descriptor` are **non-const** (land in `.data`/RAM) and the
report descriptor is **copied into a RAM buffer** before install. (This was a
real, necessary fix even though it was not the headline blocker.)

### 3e. Platform / toolchain churn and red herrings
A lot of effort was spent on things that turned out **not** to be the cause:
- A **pioarduino fork** detour (to match the reference's IDF 5.5.x) — unnecessary
  once the tick rate was fixed; the latest official IDF 6.0.1 works fine.
- **Global package-pool collisions** when swapping official ↔ pioarduino
  platforms (`@src-` toolchain variants → *"C compiler is not able to compile a
  simple test program"*). Cost a purge-and-reinstall cycle.
- **`CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG`** was briefly blamed (the
  PHY-sharing theory). **It is a red herring** — the working reference leaves it
  enabled and still enumerates. (An earlier note in this file asserted it was the
  fix; that was wrong.)
- **Descriptor byte size (203 vs 209)** and **which core calls
  `tinyusb_driver_install`** — neither mattered.

---

## 4. The actual root causes / requirements that stuck

1. **`CONFIG_FREERTOS_HZ=1000`** — the enumeration blocker.
2. **Descriptors in DMA-capable RAM** — required in DMA mode.
3. **Raw TinyUSB on ESP-IDF** — the Arduino USB stack could not drive the
   bidirectional Pro Controller handshake.
4. Generated descriptor + faithful protocol port from `esp-usb-ble-hid`
   (finger563).

Everything else (platform/IDF/esp_tinyusb version, secondary console, core
affinity) was incidental.

---

## 5. What the original plan was missing, and how to avoid it next time

The original plan was thorough about **protocol bytes** and **API signatures**
but missed the things that actually cost the time. In order of impact:

1. **It never pinned or checked the foundational RTOS/timing config.** USB
   enumeration is timing-critical, yet the plan said nothing about
   `CONFIG_FREERTOS_HZ`. *Avoid:* for any timing-sensitive peripheral, treat the
   RTOS tick rate (and the peripheral's mode bits) as first-class config and
   **diff your `sdkconfig` against a known-working reference's `sdkconfig` early.*

2. **It validated the easy unknowns, not the riskiest one.** It "confirmed API
   signatures" but never ran a minimal spike to answer *"does this stack even
   enumerate, and will the host drive the handshake?"* *Avoid:* **spike the
   single riskiest unknown first** on the exact reference stack, before
   committing an architecture or writing features.

3. **It diverged from the working reference's framework on purpose.** The plan
   chose to "convert the USB half of an ESP-IDF reference to Arduino." That
   conversion *was* the blocker. *Avoid:* when a working reference exists,
   **match its framework/stack first, get a baseline, then diverge** — never
   port the hardest part to a different framework up front.

4. **It reimplemented instead of reusing the reference's component.** Hand-copying
   a descriptor produced a wrong-sized artifact and a lot of "is this byte
   correct?" effort. *Avoid:* **reuse the reference's data-generating component**
   (here `espp/hid-rp`) rather than pasting its output.

5. **It ignored memory/DMA placement.** Descriptors were treated as plain `const`
   arrays. *Avoid:* know whether the peripheral DMAs the buffers you hand it; if
   so, they must be in DMA-capable RAM, not flash.

6. **It left the toolchain unpinned.** Version/platform churn ate time. *Avoid:*
   **pin the platform/IDF version**, and when matching a reference, copy its
   exact toolchain before changing anything else.

7. **It mis-ranked the risk.** The plan's stated "biggest risk" was the external
   Switch-2 acceptance; the real blocker was a boring internal config default.
   *Avoid:* validate the **boring foundations** (does it enumerate on a PC at
   all?) before worrying about the exciting external unknowns.

**One-line takeaway:** start from a *running* copy of the known-good reference on
its own stack, confirm the critical path (PC enumeration) end-to-end, snapshot
that as a baseline, and only then refactor/port/add features — re-testing
enumeration after each change.

---

## 6. Runtime architecture (current)

- **Core 0:** `app_main` → `usb_start()` (TinyUSB install + `stream_task`).
- **Core 1:** LVGL render task + `app_loop_task` (BOOT-button menu, status
  screen, NeoPixel, macro commands).
- **Macros:** `src/macros/<name>.{h,cpp}`, each exposing
  `start/reset/update/isRunning/isDone`. Add a macro by copying the
  `boulder_macro` pattern + a menu entry.
- **NeoPixel:** RED = not mounted, BLUE = mounted, YELLOW = handshake underway,
  GREEN = standard input mode (accepted).
- **sdkconfig:** `sdkconfig.defaults` is the committed source of truth;
  `sdkconfig.<env>` is generated and gitignored.

---

## 7. Aside — could a Teensy 2.0 (ATmega32U4) have done this?

Prompted by [exsilium/zelda-snowball-thrower](https://github.com/exsilium/zelda-snowball-thrower),
a Teensy 2.0 project that emulated the **HORI Pokken Tournament Pro Pad** and
worked on Switch 1. Short answer: **yes — and the simple version would have
avoided our two worst bugs.** But it emulates a *different, easier* device.

### Two very different problems

- **Pokken Pro Pad emulation (what that project did) — simple and proven.** The
  Pokken Tournament Pro Pad (HORI, VID `0x0F0D` / PID `0x0092`) is a *licensed
  plain HID gamepad*. The Switch accepts it with **no Nintendo handshake** — no
  `0x80` init, no `0x21` subcommand replies, no SPI calibration dump, no
  bidirectional OUT traffic. The device just declares a small (~8-byte) HID input
  report and streams it. That is trivial on a 16 MHz ATmega32U4 with LUFA — it is
  the whole Splatoon-printer / Switch-Fightstick lineage. This is the
  well-trodden, low-risk path.

- **Full Pro Controller emulation (what THIS project does) — also feasible on the
  32U4, and it would have dodged our hardest bugs.** The 32U4 USB device
  controller supports HID IN+OUT endpoints, and LUFA gives raw control of
  `SET_REPORT`/OUT and descriptor responses. The Pro Controller handshake (the
  `0x80` family, `0x21` replies, ~240 bytes of canned SPI ROM, the 209-byte
  report descriptor, `0x30` streaming) fits in 32 KB flash / 2.5 KB RAM, if
  tightly. Crucially, the bare-metal AVR path **structurally cannot hit** the two
  problems that cost us the most:
  - **No RTOS tick-rate trap.** LUFA is bare-metal; enumeration is driven by the
    USB ISR + main loop, with no FreeRTOS tick to starve. The
    `CONFIG_FREERTOS_HZ=100` class of bug cannot exist.
  - **No DMA-from-flash trap.** AVR has no descriptor DMA; LUFA copies descriptors
    out of `PROGMEM` byte-by-byte in the control handler, so "descriptor in flash"
    is the *normal, correct* case — the RAM-placement fix we needed on the S3 is a
    non-issue.

### Why the ESP32-S3 was still a defensible choice

- **Different, simpler target device.** The Teensy/Pokken route impersonates a
  basic licensed pad. The Switch accepts controllers via a **VID/PID whitelist**,
  and the Pokken pad (`0x0F0D`/`0x0092`) is a confirmed entry — but only the
  **original Switch family is documented** (Switch / Lite / OLED). Its status on
  **Switch 2 is unverified, and plausibly broken**: Switch 2 tightens third-party
  acceptance via firmware and a proprietary handshake (firmware 21.0.0 broke
  third-party *docks*; accessory makers shipped workarounds). The authentic Pro
  Controller identity (`057E`/`2009`) is the safer bet for a Switch 2 target.
  Sources:
  [squirelo/Arduino-JoyCon-Library DeepWiki — Switch compatibility](https://deepwiki.com/squirelo/Arduino-JoyCon-Library-for-Nintendo-Switch/7.2-nintendo-switch-compatibility)
  (whitelist mechanism + Pokken VID/PID; compatibility matrix lists only the
  original Switch family, not Switch 2) and
  [Tom's Hardware, 2025-11-14](https://www.tomshardware.com/video-games/nintendo/nintendo-says-it-has-no-intention-of-blocking-third-party-switch-2-docks-following-firmware-update-that-stopped-them-from-working-accessory-makers-scramble-to-deploy-workaround)
  (Switch 2 firmware breaking third-party accessories; proprietary handshake).
  **No source definitively confirms the Pokken pad on Switch 2** — proven on
  Switch 1, unverified on Switch 2.
- **On-device UX.** The 32U4 has no display and ~2.5 KB RAM. This project wanted a
  **TFT status UI, a one-button menu, and a NeoPixel**; the ESP32-S3 (240 MHz,
  320 KB RAM, native USB, SPI TFT) supports that comfortably, whereas a Teensy 2.0
  would be a headless, minimal device.
- **Same family as the reference.** The ESP32-S3 matches the working
  `esp-usb-ble-hid` reference, so the protocol was a *port/copy* rather than a
  re-derivation.

### Honest verdict

If the only goal were "send inputs to farm on **Switch 1**," a Teensy 2.0 running
the Pokken-pad approach is the simplest, most proven solution and would have
skipped the timing/DMA rabbit holes entirely. This project deliberately aimed
higher — an **authentic Pro Controller** with an **on-device UI** on a richer MCU,
with **Switch 2** in mind — which is why it took the harder ESP-IDF/TinyUSB path.
The 32U4 *could* host the full Pro Controller protocol too, but you would trade
away the display/UX and fight tight RAM. The lesson from §5 still applies: if a
proven device + board combo exists for your actual target, start there before
reaching for the more capable but more finicky stack.

