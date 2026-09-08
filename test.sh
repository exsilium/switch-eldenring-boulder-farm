#!/usr/bin/env sh
# Cross-platform host-unit-test launcher (POSIX: Linux/macOS).
#
# Runs the dependency-light engine + rumble tests under test/ inside the same
# Docker image used for firmware builds, so no host PlatformIO / g++ toolchain
# is required. The image entrypoint (`pio run -e feather_s3_idf`) is overridden
# with `pio test -e native`.
#
# The image is only built when it is missing (rebuilding re-exports ~10 GB), and
# the PlatformIO workspace is redirected to a named Docker volume so test builds
# never touch -- or invalidate -- the host's .pio/build/feather_s3_idf/.
#
# Usage:
#   ./test.sh                     # run all host tests
#   ./test.sh --rebuild           # force `docker build` first (or REBUILD=1)
#   ./test.sh -f test_engine      # extra args are forwarded to `pio test`
#
# Contributors who already have PlatformIO installed can skip Docker entirely
# and just run:  pio test -e native
set -eu

IMAGE=switch-firmware-builder
VOLUME=switch-pio-test

if ! command -v docker >/dev/null 2>&1; then
    echo "Error: Docker is required but was not found on PATH." >&2
    echo "Install Docker (https://docs.docker.com/get-docker/) and retry," >&2
    echo "or test natively with: pio test -e native" >&2
    exit 1
fi

# Resolve the directory this script lives in (the repo root).
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

# Strip our own --rebuild flag out of the args forwarded to `pio test`.
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

echo ">> Running host unit tests (pio test -e native $*)..."
docker run --rm \
    -v "$SCRIPT_DIR:/project" \
    -v "$VOLUME:/pio" \
    -e PLATFORMIO_WORKSPACE_DIR=/pio \
    --entrypoint pio "$IMAGE" test -e native "$@"
