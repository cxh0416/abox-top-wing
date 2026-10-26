[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $ServiceRoot,
    [string] $Compiler = 'D:/MinGW/bin/gcc.exe'
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
& (Join-Path $root 'platform/tools/test_v4_product.ps1') -ProductRoot $root -ServiceRoot $ServiceRoot `
    -Product 'top_flying_wing' -PacketTest 'test_mqtt_v4.exe' -IdentityTest 'test_wing_enrollment.exe' -Compiler $Compiler
