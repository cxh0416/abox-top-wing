[CmdletBinding()]
param([string] $FirmwareVersion = 'top-flying-wing-1.1.16-gd')
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
if ($FirmwareVersion -notmatch '^top-flying-wing-[0-9]+\.[0-9]+\.[0-9]+-gd$') {
    throw 'GD32 V1 candidate requires a numeric product version ending in -gd.'
}
$appBuild = Join-Path $root 'build/wing-gd-candidate/app'
$bootBuild = Join-Path $root 'build/wing-gd-candidate/boot'
$appToolchain = Join-Path $root 'app/cmake/gcc-arm-none-eabi.cmake'
$bootSource = Join-Path $root 'platform/targets/gd32f105_abox_boot'
$bootToolchain = Join-Path $bootSource 'gcc-arm-none-eabi.cmake'
& cmake -S (Join-Path $root 'app') -B $appBuild -G Ninja "-DCMAKE_TOOLCHAIN_FILE=$appToolchain" `
    '-DCMAKE_BUILD_TYPE=Release' '-DABOX_BOARD_VERSION=v1' "-DTOP_FLYING_WING_APP_FW_VERSION=$FirmwareVersion"
if ($LASTEXITCODE -ne 0) { throw 'GD App configure failed' }
& cmake --build $appBuild --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'GD App build failed' }
& cmake -S $bootSource -B $bootBuild -G Ninja "-DCMAKE_TOOLCHAIN_FILE=$bootToolchain" '-DCMAKE_BUILD_TYPE=Release'
if ($LASTEXITCODE -ne 0) { throw 'GD Boot configure failed' }
& cmake --build $bootBuild --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'GD Boot build failed' }
$boot = Join-Path $bootBuild 'ABox_GD_Boot.bin'
$app = Join-Path $appBuild 'TopFlyingWing_App.bin'
& python (Join-Path $bootSource 'tools/verify_boot_artifact.py') $boot
if ($LASTEXITCODE -ne 0) { throw 'GD Boot verification failed' }
$bytes = [IO.File]::ReadAllBytes($app)
$text = [Text.Encoding]::ASCII.GetString($bytes)
if ($bytes.Length -gt 0x36000 -or
    !$text.Contains("TOP_FLYING_WING_MCU=GD32F105RCT6`0") -or
    !$text.Contains("TOP_FLYING_WING_FW_VERSION=$FirmwareVersion`0")) {
    throw 'GD App size or identity verification failed'
}
Write-Host "Candidate App: $app"
Write-Host "Candidate Boot: $boot"
Write-Host 'Candidate files only; no formal dist or device flash was changed.'
