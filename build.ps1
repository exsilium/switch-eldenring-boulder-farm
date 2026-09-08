#!/usr/bin/env pwsh
# Cross-platform firmware build launcher (Windows PowerShell / PowerShell 7+).
#
# Runs the containerized `pio run` for the feather_s3_idf env, bind-mounting the
# repo so build outputs land back on the host under .pio\build\feather_s3_idf\.
#
# The image is only built when it is missing: rebuilding re-exports ~10 GB and
# is only needed when the Dockerfile or platformio.ini changed.
#
# Usage:
#   .\build.ps1                # build firmware (reuses the existing image)
#   .\build.ps1 --rebuild      # force `docker build` first ($env:REBUILD works too)
#   .\build.ps1 -t upload      # pass extra args through to `pio run` (needs a
#                              # device; add USB passthrough for real flashing)
#
# Contributors who already have PlatformIO installed can skip Docker entirely
# and just run:  pio run -e feather_s3_idf
$ErrorActionPreference = 'Stop'

$Image = 'switch-firmware-builder'

if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
    Write-Error @'
Docker is required but was not found on PATH.
Install Docker Desktop (https://docs.docker.com/get-docker/) and retry,
or build natively with: pio run -e feather_s3_idf
'@
    exit 1
}

# Resolve the directory this script lives in (the repo root).
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

# Strip our own --rebuild flag out of the args forwarded to `pio run`.
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

Write-Host ">> Building firmware (pio run -e feather_s3_idf $PioArgs)..."
docker run --rm -v "${ScriptDir}:/project" $Image @PioArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host ">> Done. Artifacts: .pio\build\feather_s3_idf\ (firmware.elf, firmware.bin)"
