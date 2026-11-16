[CmdletBinding()]
param([string] $FirmwareVersion = 'top-flying-wing-1.1.17-gd')
$ErrorActionPreference = 'Stop'
if ($FirmwareVersion -notmatch '^top-flying-wing-[0-9]+\.[0-9]+\.[0-9]+-gd$') {
    throw 'Candidate version must end in -gd.'
}
$output = 'build/candidate-pair-' + [Guid]::NewGuid().ToString('N')
$v2Version = $FirmwareVersion -replace '-gd$', '-st'
& python (Join-Path $PSScriptRoot 'build_paired_release.py') --version $v2Version --candidate $output
if ($LASTEXITCODE -ne 0) { throw 'Candidate pair failed.' }
