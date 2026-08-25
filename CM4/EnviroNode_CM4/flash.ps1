# flash.ps1 â€” Build (optional) and flash the KN-1.1 dual-core firmware to the
# STM32WL55 via ST-Link. Programs BOTH cores in one shot:
#   CM0+  (radio / LoRaWAN)  -> flash @ 0x08020000   EnviroNode_CM0PLUS.elf
#   CM4   (application)      -> flash @ 0x08000000   EnviroNode_CM4.elf
# The ELF files carry their own load addresses, so no -a offset is needed.
#
# Usage:
#   .\flash.ps1                 # build both cores, then flash (the default)
#   .\flash.ps1 -NoBuild        # flash the existing ELFs without rebuilding
#   .\flash.ps1 -Core cm4       # build+flash only CM4 (cm0 | cm4 | both)
#   .\flash.ps1 -Config Release
#   .\flash.ps1 -Build -NoFlash # build only (e.g. no board connected)
#
# ⚠️ Building is the DEFAULT since r23. It used to require -Build, and a bare
# invocation silently flashed whatever stale ELF was lying around — which cost
# a half-day chasing "bugs" that were already fixed in source (LOGBOOK r23).
# -Build is still accepted (now redundant); -NoBuild restores the old behavior.

param(
    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Debug',
    [ValidateSet('both', 'cm4', 'cm0')]
    [string]$Core = 'both',
    [switch]$Build,
    [switch]$NoBuild,
    [switch]$NoFlash
)

# Note: 'Continue' (not 'Stop') so that harmless stderr text from native tools
# (e.g. CMake's "manually-specified variables were not used" warning) does not
# abort the script under Windows PowerShell 5.1. Every critical step below is
# gated explicitly on $LASTEXITCODE / Test-Path and throws on real failure.
$ErrorActionPreference = 'Continue'

# --- Locate STM32CubeCLT tools (adjust version here if you update CLT) ---
$CLT = 'C:\ST\STM32CubeCLT_1.19.0'
$CLI = Join-Path $CLT 'STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe'
$env:PATH = "$CLT\CMake\bin;$CLT\Ninja\bin;$CLT\GNU-tools-for-STM32\bin;$env:PATH"

# --- Project locations (this script lives in the CM4 project folder) ---
$cm4Dir = $PSScriptRoot
$cm0Dir = (Resolve-Path (Join-Path $PSScriptRoot '..\..\CM0\EnviroNode_CM0PLUS')).Path

$cm4Elf = Join-Path $cm4Dir "build\$Config\EnviroNode_CM4.elf"
$cm0Elf = Join-Path $cm0Dir "build\$Config\EnviroNode_CM0PLUS.elf"

function Build-Core([string]$name, [string]$dir) {
    Write-Host "==> Building $name ($Config)..." -ForegroundColor Cyan
    $buildDir = Join-Path $dir "build\$Config"
    if (-not (Test-Path (Join-Path $buildDir 'CMakeCache.txt'))) {
        Write-Host "    (configuring $name preset $Config)" -ForegroundColor DarkGray
        Push-Location $dir
        try { cmake --preset $Config | Out-Null } finally { Pop-Location }
        if ($LASTEXITCODE -ne 0) { throw "$name configure failed." }
    }
    cmake --build $buildDir
    if ($LASTEXITCODE -ne 0) { throw "$name build failed." }
}

if (-not $NoBuild) {
    if ($Core -ne 'cm4') { Build-Core 'CM0+' $cm0Dir }
    if ($Core -ne 'cm0') { Build-Core 'CM4'  $cm4Dir }
}

if ($NoFlash) {
    Write-Host "==> Build only (skipping flash)." -ForegroundColor Green
    return
}

# --- Collect the ELF download arguments for the selected core(s) ---
$dl = @()
if ($Core -ne 'cm4') {
    if (-not (Test-Path $cm0Elf)) { throw "CM0+ firmware not found: $cm0Elf  (run with -Build first)" }
    $dl += @('-d', $cm0Elf)
}
if ($Core -ne 'cm0') {
    if (-not (Test-Path $cm4Elf)) { throw "CM4 firmware not found: $cm4Elf  (run with -Build first)" }
    $dl += @('-d', $cm4Elf)
}

Write-Host "==> Flashing $Core core(s) over SWD..." -ForegroundColor Cyan
# Single connection: program CM0+ and/or CM4, then reset and run.
& $CLI -c port=SWD mode=UR @dl -rst
if ($LASTEXITCODE -ne 0) { throw "Flash failed. Is the ST-Link connected and the board powered?" }

Write-Host "==> Done. Both cores programmed; target reset and running." -ForegroundColor Green
