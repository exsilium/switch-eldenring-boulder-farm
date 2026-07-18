#!/usr/bin/env pwsh
# Cross-platform firmware build launcher (Windows PowerShell / PowerShell 7+).
#
# Builds the Docker image and runs the containerized `pio run` for the
# feather_s3_idf env, bind-mounting the repo so build outputs land back on the
# host under .pio\build\feather_s3_idf\.
#
# Usage:
#   .\build.ps1                # build firmware
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

Write-Host ">> Building Docker image '$Image'..."
docker build -t $Image $ScriptDir
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host ">> Building firmware (pio run -e feather_s3_idf $args)..."
docker run --rm -v "${ScriptDir}:/project" $Image @args
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host ">> Done. Artifacts: .pio\build\feather_s3_idf\ (firmware.elf, firmware.bin)"
