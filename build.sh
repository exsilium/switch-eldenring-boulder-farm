#!/usr/bin/env sh
# Cross-platform firmware build launcher (POSIX: Linux/macOS).
#
# Builds the Docker image and runs the containerized `pio run` for the
# feather_s3_idf env, bind-mounting the repo so build outputs land back on the
# host under .pio/build/feather_s3_idf/.
#
# Usage:
#   ./build.sh                # build firmware
#   ./build.sh -t upload      # pass extra args through to `pio run` (needs a
#                             # device; add USB passthrough for real flashing)
#
# Contributors who already have PlatformIO installed can skip Docker entirely
# and just run:  pio run -e feather_s3_idf
set -eu

IMAGE=switch-firmware-builder

if ! command -v docker >/dev/null 2>&1; then
    echo "Error: Docker is required but was not found on PATH." >&2
    echo "Install Docker (https://docs.docker.com/get-docker/) and retry," >&2
    echo "or build natively with: pio run -e feather_s3_idf" >&2
    exit 1
fi

# Resolve the directory this script lives in (the repo root).
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

echo ">> Building Docker image '$IMAGE'..."
docker build -t "$IMAGE" "$SCRIPT_DIR"

echo ">> Building firmware (pio run -e feather_s3_idf $*)..."
docker run --rm -v "$SCRIPT_DIR:/project" "$IMAGE" "$@"

echo ">> Done. Artifacts: .pio/build/feather_s3_idf/ (firmware.elf, firmware.bin)"
