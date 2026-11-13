[CmdletBinding()]
param(
    [string]$FullPath = "",
    [string]$JLinkPath = "",
    [int]$Speed = 100,
    [switch]$ValidateOnly
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path

if ([string]::IsNullOrWhiteSpace($FullPath)) {
    $FullPath = Join-Path $root "build/app/TopFlyingWing_Full.bin"
}
$FullPath = (Resolve-Path -LiteralPath $FullPath).Path
$full = [IO.File]::ReadAllBytes($FullPath)
if ($full.Length -le 0x8000 -or $full.Length -gt 0x3E000) {
    throw "Full image must contain Boot and fit below the configuration recovery page: $FullPath"
}

# Production Boot and App ranges end before the two 2 KiB Boot State pages.
$commandFile = Join-Path $root "build/app/flash_full_auto.jlink"
$jlinkFullPath = $FullPath.Replace("\", "/")
$commands = @(
    "device STM32F105RC",
    "si SWD",
    "speed $Speed",
    "connect",
    "erase 0x0803E800, 0x0803F7FF",
    "loadbin `"$jlinkFullPath`", 0x08000000",
    "verifybin `"$jlinkFullPath`", 0x08000000",
    "r",
    "g",
    "mem32 0xE000ED08, 1",
    "q"
)
[IO.File]::WriteAllLines($commandFile, $commands, [Text.UTF8Encoding]::new($false))

$sha256 = (Get-FileHash -LiteralPath $FullPath -Algorithm SHA256).Hash
Write-Host "Full:    $FullPath"
Write-Host "Size:    $($full.Length) bytes"
Write-Host "SHA-256: $sha256"
Write-Host "State:   0x0803E800..0x0803F7FF will be erased"

if ($ValidateOnly) {
    Write-Host "Command: $commandFile"
    return
}

if ([string]::IsNullOrWhiteSpace($JLinkPath)) {
    $command = Get-Command JLink.exe -ErrorAction SilentlyContinue
    if ($command) {
        $JLinkPath = $command.Source
    }
    elseif (Test-Path -LiteralPath "E:\SEGGER\JLink_V984\JLink.exe") {
        $JLinkPath = "E:\SEGGER\JLink_V984\JLink.exe"
    }
    else {
        throw "JLink.exe not found; pass -JLinkPath explicitly"
    }
}
$JLinkPath = (Resolve-Path -LiteralPath $JLinkPath).Path

& $JLinkPath -NoGui 1 -ExitOnError 1 -CommandFile $commandFile
if ($LASTEXITCODE -ne 0) {
    throw "J-Link programming failed with exit code $LASTEXITCODE"
}
Write-Host "Programming, verification, Boot State reset and target reset completed."
