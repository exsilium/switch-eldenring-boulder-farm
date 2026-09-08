#!/usr/bin/env sh
# Cross-platform firmware build launcher (POSIX: Linux/macOS).
#
# Runs the containerized `pio run` for the feather_s3_idf env, bind-mounting the
# repo so build outputs land back on the host under .pio/build/feather_s3_idf/.
#
# The image is only built when it is missing: rebuilding re-exports ~10 GB and
# is only needed when the Dockerfile or platformio.ini changed.
#
# Usage:
#   ./build.sh                # build firmware (reuses the existing image)
#   ./build.sh --rebuild      # force `docker build` first (or set REBUILD=1)
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

# Strip our own --rebuild flag out of the args forwarded to `pio run`.
rebuild=${REBUILD:-0}
argc=$#
i=0
while [ "$i" -lt "$argc" ]; do
    arg=$1
    shift
    i=$((i + 1))
    if [ "$arg" = "--rebuild" ]; then rebuild=1; else set -- "$@" "$arg"; fi
done

if [ "$rebuild" != "0" ] || ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo ">> Building Docker image '$IMAGE'..."
    docker build -t "$IMAGE" "$SCRIPT_DIR"
else
    echo ">> Reusing Docker image '$IMAGE' (pass --rebuild to rebuild it)."
fi

echo ">> Building firmware (pio run -e feather_s3_idf $*)..."
docker run --rm -v "$SCRIPT_DIR:/project" "$IMAGE" "$@"

echo ">> Done. Artifacts: .pio/build/feather_s3_idf/ (firmware.elf, firmware.bin)"
