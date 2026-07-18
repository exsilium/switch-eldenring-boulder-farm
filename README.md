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

## Continuous integration

[`.github/workflows/build.yml`](.github/workflows/build.yml) builds the Docker
image and runs the containerized firmware build on every push, pull request and
manual dispatch, then uploads `firmware.elf` / `firmware.bin` as workflow
artifacts. The workflow reclaims runner disk space before building because the
builder image bundles the full ESP-IDF toolchain (~10 GB).
