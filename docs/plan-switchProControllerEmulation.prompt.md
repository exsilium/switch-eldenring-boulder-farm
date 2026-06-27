# Plan: Switch Pro Controller Emulation (Feather ESP32-S3, Arduino)

Emulate a wired Switch 1 Pro Controller (VID `0x057E`/PID `0x2009`) on the Feather ESP32-S3 TFT using Adafruit TinyUSB. A 2 s GPIO0 long-press toggles attach/detach; on connect the firmware runs a one-shot D-pad **LEFT → 1 s → RIGHT** macro, then holds neutral. The existing Hello-World button/TFT scaffold in [src/main.cpp](src/main.cpp) is reshaped into the state machine and TFT status surface.

**API validation (confirmed against TinyUSB master):**
- `TinyUSBDevice.setID/setVersion/attach/detach/mounted/begin` all exist as assumed.
- `Adafruit_USBD_HID.setReportDescriptor/setReportCallback/sendReport` confirmed.
- Raw report-ID-0 rule is correct: the `hid_generic_inout` example's set-callback gets `report_id == 0` + raw bytes and replies with `sendReport(0, buf, len)` — exactly the Pro Controller shape. The spec's "command byte is `buf[0]`, not a HID report ID" holds.

## Phases

1. **P0 Skeleton** — Add build flags + TinyUSB lib to [platformio.ini](platformio.ini); refactor the existing hold-detect into a `button` module; TFT shows `DETACHED` and toggles an ARMED/DISARMED label. No USB.
2. **P1 Enumeration** *(depends on P0)* — Port the real Pro Controller descriptor + VID/PID into `Adafruit_USBD_HID`; wire `attach()`/`detach()` to the long-press toggle. Verify on a **PC** (`057E`/`2009` appears/disappears).
3. **P2 Handshake & stay-connected** *(depends on P1)* — Implement `0x80` family + minimum subcommand replies incl. the SPI-flash calibration dump; hold neutral `0x30` on the poll path. **This is the GO/NO-GO hardware milestone.**
4. **P3 Input macro** *(depends on P2)* — Non-blocking (millis) LEFT→1 s→RIGHT on first `CONNECTED_IDLE`.
5. **P4 Disconnect/reconnect** *(depends on P3)* — Long-press → `detach()` → re-arm reruns P2–P3, repeatable without power-cycle.
6. **P5 Polish** *(depends on P4)* — Live TFT state, ignore button mid-transition, macro fires exactly once per connection.

## Software architecture

```
src/
  main.cpp                 // Arduino setup()/loop(), state-machine + macro driver
  app_state.h              // State enum + transitions
  button.h / button.cpp    // GPIO0 debounce + 2s long-press (one event per hold)
  input_macro.h / .cpp     // Non-blocking LEFT -> 1s -> RIGHT sequence
  display.h / display.cpp  // TFT status rendering
  procon/
    procon_descriptor.h    // HID report descriptor, VID/PID, string descriptors
    procon_protocol.h/.cpp // handshake FSM, subcommand table, SPI-flash dump, builders
    procon_usb.h/.cpp      // Adafruit_TinyUSB glue: callbacks, sendReport, attach/detach
    procon_reports.h       // 0x30 input report struct + button bitmap helpers
```

## State machine

```
IDLE_DETACHED        // powered, USB detached, waiting for long-press
  └─(long-press)──▶ ATTACHING            // TinyUSBDevice.attach() called
                      └─(mounted)──▶ HANDSHAKING      // answering 0x80 + subcommands
                                       └─(init done)──▶ RUN_MACRO   // LEFT,1s,RIGHT (once)
                                                          └─(done)──▶ CONNECTED_IDLE
IDLE_DETACHED ◀─(long-press from any attached state)── DETACHING  // TinyUSBDevice.detach()
```

`CONNECTED_IDLE` keeps answering polls with the neutral `0x30` report (never silent), or the console drops the controller.

## platformio.ini changes

Add to `[env:featheresp32]`:

```ini
build_flags =
    -DARDUINO_USB_MODE=0          ; 0 = USB-OTG (TinyUSB). Need 0.
    -DARDUINO_USB_CDC_ON_BOOT=0   ; clean HID-only device, no composite CDC
lib_deps =
    adafruit/Adafruit TinyUSB Library
```

Keep existing ST7789 + GFX libs. Confirm the build links Adafruit TinyUSB (not Espressif `USB.h`).

## Protocol cheat-sheet (port verbatim from references; framework-agnostic)

- **0x80 handshake:** `8001` → `8101`+type+MAC; `8002` → `8102` ack; `8004` force-USB / disable-timeout (**MANDATORY**) ack; `8005` allow-timeout (teardown).
- **Subcommands** (via `0x01` OUT → reply `0x21`): `02` dev info, `10` SPI flash read (stick cal + color from canned dump), `03` set mode `0x30`, `04` trigger time, `40` IMU, `48` vibration, `30` player LEDs.
- **0x30 input report (~60 Hz):** `[0x30][timer][batt/conn][btn0][btn1][btn2][L-stick 3B][R-stick 3B][IMU 6B]`; sticks 12-bit packed, centered `0x800`. Maintain one live-report buffer.

## Verification

1. `pio run` builds clean; confirm it links Adafruit TinyUSB, not Espressif `USB.h`.
2. PC enumerates `057E`/`2009` as **HID** (if it shows as a serial port, flip `ARDUINO_USB_MODE`).
3. Docked Switch 2 **dock USB-A**: connects, clears "Connecting…", no silent drop; cursor moves L, pauses ~1 s, moves R in the Home menu.
4. Debug via TFT (CDC off = no USB serial); optional UART0 mirror.

## Decisions

- LEFT/RIGHT = D-pad presses ~100 ms (configurable).
- Docked USB-A only; handheld USB-C out of scope (vendor auth).
- CDC off → no USB serial; TFT is the debug surface.
- All timing non-blocking (millis) — never `delay()` through the 1 s gap.

## Biggest risk (external, cannot validate in code)

Whether a Switch 2 in compatibility mode accepts an emulated wired Pro Controller over dock USB-A is external behavior no code change can guarantee. Phase 2's accept-test is the real go/no-go. Everything up to that point (build, enumeration, handshake byte-correctness) is fully under our control.

## References

- `finger563/esp-usb-ble-hid` — base implementation (ESP-IDF; convert USB half to Arduino).
- `dekuNukem/Nintendo_Switch_Reverse_Engineering` (`USB-HID-Notes.md`) — `0x80` protocol spec.
- `DavidPagels/retro-pico-switch` — descriptor, SPI-flash calibration dump, `0x30` packing.
- `felis/USB_Host_Shield_2.0` (`SwitchProUSB.h`) — handshake/disable-timeout sequence.
- Adafruit TinyUSB Library — `hid_generic_inout` example as the Arduino scaffold.

## Open considerations

1. **LEFT/RIGHT input type** — D-pad (recommended default) / left-stick deflection / face buttons.
2. **Logging surface** — TFT only (recommended) / TFT + UART0 mirror for byte-level handshake tracing during P2.
3. **Macro on reconnect** — once per connection then idle (recommended) / re-run every reconnect.
