# Reproducible firmware build image for the Switch Pro Controller emulator.
#
# Builds the PlatformIO/ESP-IDF firmware (env:feather_s3_idf) with no host
# toolchain required. The espressif32 platform, the ESP-IDF toolchain and the
# managed components are fetched by PlatformIO; the platform/toolchain download
# is baked into an image layer so repeat builds only recompile changed sources.
#
# Usage (see build.sh / build.ps1 for the cross-platform launchers):
#   docker build -t switch-firmware-builder .
#   docker run --rm -v "$PWD:/project" switch-firmware-builder
#
# Extra arguments are appended to `pio run -e feather_s3_idf`, e.g.:
#   docker run --rm -v "$PWD:/project" switch-firmware-builder -t upload
#
# The same image also runs the host unit tests (see test.sh / test.ps1) by
# overriding the entrypoint:
#   docker run --rm -v "$PWD:/project" --entrypoint pio switch-firmware-builder \
#       test -e native
FROM python:3.12-slim

# Minimal system dependencies PlatformIO / ESP-IDF need to build.
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        git \
        cmake \
        ninja-build \
        ccache \
        build-essential \
        libusb-1.0-0 \
    && rm -rf /var/lib/apt/lists/*

# PlatformIO Core.
RUN pip install --no-cache-dir platformio

# Keep the PlatformIO home inside the image so the platform + toolchain download
# is cached in a layer (not on the bind-mounted repo).
ENV PLATFORMIO_CORE_DIR=/root/.platformio

WORKDIR /project

# Pre-fetch the espressif32 platform and its toolchain packages so they are baked
# into the image. Only the project config is copied here to maximise layer reuse;
# the actual sources are bind-mounted at run time.
COPY platformio.ini ./platformio.ini
RUN pio pkg install -e feather_s3_idf

# Same for the host-test env. Its workspace is baked at /pio, which is where the
# test launchers mount their named volume -- Docker seeds an empty volume from
# the image, so Unity is already there on the first `pio test -e native` run.
RUN PLATFORMIO_WORKSPACE_DIR=/pio pio pkg install -e native

# `pio run` for the firmware env. Additional args (e.g. `-t upload`) are appended.
ENTRYPOINT ["pio", "run", "-e", "feather_s3_idf"]
