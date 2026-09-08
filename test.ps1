#!/usr/bin/env pwsh
# Cross-platform host-unit-test launcher (Windows PowerShell / PowerShell 7+).
#
# Runs the dependency-light engine + rumble tests under test\ inside the same
# Docker image used for firmware builds, so no host PlatformIO / g++ toolchain
# is required. The image entrypoint (`pio run -e feather_s3_idf`) is overridden
# with `pio test -e native`.
#
# The image is only built when it is missing (rebuilding re-exports ~10 GB), and
# the PlatformIO workspace is redirected to a named Docker volume so test builds
# never touch -- or invalidate -- the host's .pio\build\feather_s3_idf\.
#
# Usage:
#   .\test.ps1                    # run all host tests
#   .\test.ps1 --rebuild          # force `docker build` first ($env:REBUILD too)
#   .\test.ps1 -f test_engine     # extra args are forwarded to `pio test`
#
# Contributors who already have PlatformIO installed can skip Docker entirely
# and just run:  pio test -e native
$ErrorActionPreference = 'Stop'

$Image = 'switch-firmware-builder'
$Volume = 'switch-pio-test'

if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
    Write-Error @'
Docker is required but was not found on PATH.
Install Docker Desktop (https://docs.docker.com/get-docker/) and retry,
or test natively with: pio test -e native
'@
    exit 1
}

# Resolve the directory this script lives in (the repo root).
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

# Strip our own --rebuild flag out of the args forwarded to `pio test`.
$Rebuild = ($env:REBUILD -and $env:REBUILD -ne '0') -or ($args -contains '--rebuild')
$PioArgs = @($args | Where-Object { $_ -ne '--rebuild' })

docker image inspect $Image *> $null
if ($Rebuild -or $LASTEXITCODE -ne 0) {
    Write-Host ">> Building Docker image '$Image'..."
    docker build -t $Image $ScriptDir
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
} else {
    Write-Host ">> Reusing Docker image '$Image' (pass --rebuild to rebuild it)."
}

Write-Host ">> Running host unit tests (pio test -e native $PioArgs)..."
docker run --rm `
    -v "${ScriptDir}:/project" `
    -v "${Volume}:/pio" `
    -e PLATFORMIO_WORKSPACE_DIR=/pio `
    --entrypoint pio $Image test -e native @PioArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
