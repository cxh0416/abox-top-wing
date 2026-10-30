[CmdletBinding()]
param(
    [string] $ElfPath = '',
    [string] $JLinkPath = '',
    [string] $NmPath = '',
    [int] $IntervalMs = 1000,
    [int] $Count = 0,
    [int] $Speed = 100
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path

if ([string]::IsNullOrWhiteSpace($ElfPath)) {
    $ElfPath = Join-Path $root 'build/app/TopFlyingWing_App.elf'
}
$ElfPath = (Resolve-Path -LiteralPath $ElfPath).Path

if ([string]::IsNullOrWhiteSpace($JLinkPath)) {
    if (Test-Path -LiteralPath 'E:\SEGGER\JLink_V984\JLink.exe') {
        $JLinkPath = 'E:\SEGGER\JLink_V984\JLink.exe'
    }
    else {
        $command = Get-Command JLink.exe -ErrorAction SilentlyContinue
        if ($command -and $command.Source -notmatch '\\JDK\\') {
            $JLinkPath = $command.Source
        }
        else {
            throw 'SEGGER JLink.exe not found; pass -JLinkPath explicitly.'
        }
    }
}
$JLinkPath = (Resolve-Path -LiteralPath $JLinkPath).Path

if ([string]::IsNullOrWhiteSpace($NmPath)) {
    $command = Get-Command arm-none-eabi-nm -ErrorAction SilentlyContinue
    if ($command) {
        $NmPath = $command.Source
    }
    elseif (Test-Path -LiteralPath 'E:\Tools\arm-gcc-14.2\bin\arm-none-eabi-nm.exe') {
        $NmPath = 'E:\Tools\arm-gcc-14.2\bin\arm-none-eabi-nm.exe'
    }
    else {
        throw 'arm-none-eabi-nm not found; pass -NmPath explicitly.'
    }
}
$NmPath = (Resolve-Path -LiteralPath $NmPath).Path

if ($IntervalMs -lt 100) { throw 'IntervalMs must be at least 100.' }
if ($Count -lt 0) { throw 'Count must be zero or greater.' }

# wing_controller.c contains the largest static g_state object. The smaller
# same-named local symbols belong to other translation units.
$stateSymbols = @(
    & $NmPath -S $ElfPath | ForEach-Object {
        if ($_ -match '^([0-9A-Fa-f]{8})\s+([0-9A-Fa-f]{8})\s+[bB]\s+g_state$') {
            [pscustomobject]@{
                Address = [Convert]::ToUInt32($matches[1], 16)
                Size = [Convert]::ToUInt32($matches[2], 16)
            }
        }
    }
)
if ($stateSymbols.Count -eq 0) {
    throw "Cannot find g_state in ELF: $ElfPath"
}
$state = $stateSymbols | Sort-Object Size -Descending | Select-Object -First 1
if ($state.Size -lt 24) {
    throw "Unexpected g_state size in ELF: $($state.Size)"
}

# WingSnapshot is the first member of WingState. Its byte layout is:
#   0 motion_state, 1 active_action, 2..3 position_mm (little endian),
#   4 have_position, 15 status_fresh, 20 have_feedback.
$positionAddress = [uint32]($state.Address + 2)
$sampleAddress = [uint32]$state.Address

$motionNames = @('unknown', 'stopped', 'ascending', 'descending',
                 'moving_unknown', 'fault', 'offline')
$actionNames = @('none', 'ascend', 'descend', 'stop')
$tempFile = Join-Path ([IO.Path]::GetTempPath()) "top-wing-position-$PID.jlink"

try {
    Write-Host "ELF:     $ElfPath"
    Write-Host ("State:   0x{0:X8}" -f $sampleAddress)
    Write-Host ("Position:0x{0:X4} (16-bit)" -f $positionAddress)
    Write-Host 'Press Ctrl+C to stop.'
    Write-Host ''

    $sample = 0
    while ($Count -eq 0 -or $sample -lt $Count) {
        $commands = @(
            'device STM32F105RC',
            'si SWD',
            "speed $Speed",
            'connect',
            ("mem8 0x{0:X8}, 24" -f $sampleAddress),
            'q'
        )
        [IO.File]::WriteAllLines($tempFile, $commands, [Text.UTF8Encoding]::new($false))

        $output = (& $JLinkPath -NoGui 1 -ExitOnError 1 -CommandFile $tempFile 2>&1) -join "`n"
        if ($LASTEXITCODE -ne 0) {
            throw "J-Link read failed with exit code $LASTEXITCODE.`n$output"
        }

        $hex = @()
        foreach ($line in ($output -split "`r?`n")) {
            $match = [regex]::Match($line, '^[0-9A-Fa-f]{8}\s*=\s*(.+)$')
            if ($match.Success) {
                $hex += @($match.Groups[1].Value -split '\s+' | Where-Object { $_ -match '^[0-9A-Fa-f]{2}$' })
            }
        }
        if ($hex.Count -lt 24) {
            throw "Unexpected J-Link memory response.`n$output"
        }

        $bytes = @($hex[0..23] | ForEach-Object { [Convert]::ToByte($_, 16) })
        $position = [int]$bytes[2] -bor ([int]$bytes[3] -shl 8)
        $havePosition = $bytes[4] -ne 0
        $fresh = $bytes[15] -ne 0
        $haveFeedback = $bytes[20] -ne 0
        $motion = if ($bytes[0] -lt $motionNames.Count) { $motionNames[$bytes[0]] } else { "unknown($($bytes[0]))" }
        $action = if ($bytes[1] -lt $actionNames.Count) { $actionNames[$bytes[1]] } else { "unknown($($bytes[1]))" }

        $time = Get-Date -Format 'HH:mm:ss.fff'
        if ($havePosition) {
            $positionText = '{0,5} mm' -f $position
        }
        else {
            $positionText = '    -- mm'
        }
        $freshText = if ($fresh) { 'fresh' } else { 'cached' }
        Write-Host ("{0}  {1}  raw=0x{2:X4}  valid={3}  {4}  motion={5}  action={6}  feedback={7}" -f
            $time, $positionText, $position, [int]$havePosition, $freshText,
            $motion, $action, [int]$haveFeedback)

        $sample++
        if ($Count -eq 0 -or $sample -lt $Count) {
            Start-Sleep -Milliseconds $IntervalMs
        }
    }
}
finally {
    if (Test-Path -LiteralPath $tempFile -PathType Leaf) {
        Remove-Item -LiteralPath $tempFile -Force
    }
}
