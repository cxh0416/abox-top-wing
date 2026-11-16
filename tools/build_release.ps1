[CmdletBinding()]
param(
    [ValidateSet('Release')] [string] $Configuration = 'Release',
    [string] $FirmwareVersion = 'top-flying-wing-1.1.17-st'
)
$ErrorActionPreference = 'Stop'
& python (Join-Path $PSScriptRoot 'build_paired_release.py') --version $FirmwareVersion --formal
if ($LASTEXITCODE -ne 0) { throw 'Paired release failed.' }
