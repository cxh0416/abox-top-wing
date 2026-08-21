[CmdletBinding()]
param([ValidateSet('Debug', 'Release')] [string] $Configuration = 'Release')
& "$PSScriptRoot\..\platform\tools\build_product_release.ps1" -ProductRoot "$PSScriptRoot\.." -AppTarget TopFlyingWing_App -BootTarget TopFlyingWing_Boot -CombinedName TopFlyingWing_Full.bin -Configuration $Configuration -AppLinker app/STM32F105XX_FLASH.ld -BootLinker boot/STM32F105xx_FLASH.ld
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& "$PSScriptRoot\package_release.ps1" `
    -BootPath "$PSScriptRoot\..\boot\build\ABoxUnifiedRelease\TopFlyingWing_Boot.bin" `
    -AppPath "$PSScriptRoot\..\app\build\ABoxUnifiedRelease\TopFlyingWing_App.bin"
exit $LASTEXITCODE
