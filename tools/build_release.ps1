[CmdletBinding()]
param(
    [ValidateSet('Release')] [string] $Configuration = 'Release',
    [string] $FirmwareVersion = 'top-flying-wing-1.1.0'
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$appSource = Join-Path $root 'app'
$appBuild = Join-Path $root 'build/app'
$toolchain = Join-Path $appSource 'cmake/gcc-arm-none-eabi.cmake'
$bootLinker = Join-Path $root 'platform/targets/stm32f105_abox_boot/STM32F105xx_FLASH.ld'
$appLinker = Join-Path $appSource 'STM32F105XX_FLASH.ld'
$bootArtifact = Join-Path $root 'platform/targets/stm32f105_abox_boot/dist/ABox_Boot.bin'
$validator = Join-Path $PSScriptRoot 'test_boot_v2_contract.py'

foreach ($required in @($toolchain, $bootLinker, $appLinker, $bootArtifact, $validator)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) { throw "Missing release input: $required" }
}

# Top Flying Wing retains App through 0x0803E7FF and has no extra recovery
# page at 0x0803E000. Validate its frozen contract rather than another layout.
& python $validator | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'Frozen Flash layout validation failed' }

& cmake -S $appSource -B $appBuild -G Ninja `
    "-DCMAKE_TOOLCHAIN_FILE=$toolchain" `
    "-DCMAKE_BUILD_TYPE=$Configuration" `
    "-DTOP_FLYING_WING_APP_FW_VERSION=$FirmwareVersion" | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'App configure failed' }
& cmake --build $appBuild --parallel 4 | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'App build failed' }

$appArtifact = Join-Path $appBuild 'TopFlyingWing_App.bin'
if (-not (Test-Path -LiteralPath $appArtifact -PathType Leaf)) { throw "Missing App artifact: $appArtifact" }

$packageArgs = @{
    BootPath = $bootArtifact
    AppPath = $appArtifact
    FirmwareVersion = $FirmwareVersion
}
& (Join-Path $PSScriptRoot 'package_release.ps1') @packageArgs
exit $LASTEXITCODE
