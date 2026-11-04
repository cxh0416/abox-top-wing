[CmdletBinding()]
param(
    [string]$BootPath = "",
    [string]$AppPath = "",
    [string]$FirmwareVersion = "top-flying-wing-1.1.9",
    [string]$BootVersion = "abox-boot-2.3.0"
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$stateGenerator = Join-Path $PSScriptRoot 'make_bootstate.py'
$packageValidator = Join-Path $PSScriptRoot 'verify_release.py'
$readmeTemplate = Join-Path $PSScriptRoot 'release_readme.md'
$outputDirectory = [System.IO.Path]::GetFullPath((Join-Path $root 'dist'))
if ((Split-Path -Parent $outputDirectory) -ne $root -or
    (Split-Path -Leaf $outputDirectory) -ne 'dist') {
    throw "Refusing to replace unexpected release directory: $outputDirectory"
}

if ([string]::IsNullOrWhiteSpace($BootPath)) {
    $BootPath = Join-Path $root 'platform/targets/stm32f105_abox_boot/dist/ABox_Boot.bin'
}
if ([string]::IsNullOrWhiteSpace($AppPath)) {
    $AppPath = Join-Path $root 'build/app/TopFlyingWing_App.bin'
}
foreach ($path in @($BootPath, $AppPath, $stateGenerator, $packageValidator, $readmeTemplate)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Release artifact not found: $path"
    }
}

$appOffset = 0x8000
$boot = [System.IO.File]::ReadAllBytes($BootPath)
$app = [System.IO.File]::ReadAllBytes($AppPath)
if ($boot.Length -gt $appOffset) {
    throw 'Boot image exceeds 0x8000-byte region'
}
$versionMarker = "TOP_FLYING_WING_FW_VERSION=$FirmwareVersion`0"
if (-not [System.Text.Encoding]::ASCII.GetString($app).Contains($versionMarker)) {
    throw "App artifact does not contain the exact release version marker: $FirmwareVersion"
}

function Get-Crc32([byte[]]$Bytes) {
    [int64]$crc = 0xFFFFFFFF
    foreach ($byte in $Bytes) {
        $crc = $crc -bxor [int64]$byte
        if ($crc -lt 0) { $crc += 4294967296 }
        for ($bit = 0; $bit -lt 8; $bit++) {
            if (($crc -band 1) -ne 0) { $crc = ($crc -shr 1) -bxor 0xEDB88320 }
            else { $crc = $crc -shr 1 }
            if ($crc -lt 0) { $crc += 4294967296 }
        }
    }
    return ([Convert]::ToString([uint32](4294967295 - $crc), 16).PadLeft(8, '0').ToUpperInvariant())
}

function Get-Sha256([byte[]]$Bytes) {
    $algorithm = [System.Security.Cryptography.SHA256]::Create()
    try {
        return [BitConverter]::ToString($algorithm.ComputeHash($Bytes)).Replace('-', '').ToLowerInvariant()
    }
    finally { $algorithm.Dispose() }
}

function Get-Artifact([string]$Path, [string]$Name, [string]$LoadAddress, [bool]$IncludesBootState = $false) {
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    [ordered]@{
        name = $Name
        size = $bytes.Length
        load_address = $LoadAddress
        crc32 = Get-Crc32 $bytes
        sha256 = Get-Sha256 $bytes
        includes_device_config = $false
        includes_boot_state = $IncludesBootState
    }
}

$token = [Guid]::NewGuid().ToString('N')
$stagingDirectory = Join-Path $root ".dist-staging-$token"
$previousDirectory = Join-Path $root ".dist-previous-$token"
$previousMoved = $false

# Every directory moved or recursively removed below is an absolute child of
# this workspace. Do not follow an existing release directory junction.
foreach ($directory in @($outputDirectory, $stagingDirectory, $previousDirectory)) {
    if ([System.IO.Path]::GetFullPath((Split-Path -Parent $directory)) -ne $root) {
        throw "Refusing to touch a directory outside the workspace: $directory"
    }
    if ((Test-Path -LiteralPath $directory) -and
        ((Get-Item -LiteralPath $directory -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "Refusing to replace a linked release directory: $directory"
    }
}

try {
    New-Item -ItemType Directory -Path $stagingDirectory | Out-Null

    $bootOut = Join-Path $stagingDirectory 'ABox_Boot.bin'
    $appOut = Join-Path $stagingDirectory 'TopFlyingWing_App.bin'
    $fullOut = Join-Path $stagingDirectory 'TopFlyingWing_Full.bin'
    $stateOut = Join-Path $stagingDirectory 'TopFlyingWing_BootState.bin'
    [System.IO.File]::WriteAllBytes($bootOut, $boot)
    [System.IO.File]::WriteAllBytes($appOut, $app)

    & python $stateGenerator --app $appOut --output $stateOut --version $FirmwareVersion | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'Matching Boot State generation failed' }
    $state = [System.IO.File]::ReadAllBytes($stateOut)
    if ($state.Length -ne 0x1000) { throw 'Boot State image must cover exactly two 2 KiB pages' }

    # Include State A/B while ending exactly before the device configuration.
    $full = New-Object byte[] 0x3F800
    for ($index = 0; $index -lt $full.Length; $index++) { $full[$index] = 0xFF }
    [Array]::Copy($boot, 0, $full, 0, $boot.Length)
    [Array]::Copy($app, 0, $full, $appOffset, $app.Length)
    [Array]::Copy($state, 0, $full, 0x3E800, $state.Length)
    [System.IO.File]::WriteAllBytes($fullOut, $full)

    $manifest = [ordered]@{
        manifest_version = 1
        product = 'top_flying_wing'
        firmware_version = $FirmwareVersion
        boot_version = $BootVersion
        ota_scheme = 'boot_v2_ufs'
        ota_downloader = 'app'
        can_session_mode = 'remote_only_idle_silent'
        idle_status_poll_enabled = $false
        app_range = '0x08008000..0x0803E7FF'
        boot_state_pages = @('0x0803E800', '0x0803F000')
        boot_state_initialization = [ordered]@{
            format_version = 3
            state_a = 'CONFIRMED'
            state_b = 'erased'
        }
        config_page = '0x0803F800'
        artifacts = @(
            (Get-Artifact $appOut 'TopFlyingWing_App.bin' '0x08008000'),
            (Get-Artifact $bootOut 'ABox_Boot.bin' '0x08000000'),
            (Get-Artifact $fullOut 'TopFlyingWing_Full.bin' '0x08000000' $true),
            (Get-Artifact $stateOut 'TopFlyingWing_BootState.bin' '0x0803E800' $true)
        )
    }
    $manifestJson = ($manifest | ConvertTo-Json -Depth 6) -replace "`r`n", "`n"
    [System.IO.File]::WriteAllText((Join-Path $stagingDirectory 'manifest.json'),
        $manifestJson + "`n", [System.Text.UTF8Encoding]::new($false))
    $releaseManifest = [ordered]@{
        schema_version = 1
        product = 'top_flying_wing'
        version = $FirmwareVersion
        artifact = 'top_flying_wing_app.bin'
        file = 'TopFlyingWing_App.bin'
        immutable = $true
        can_session_mode = 'remote_only_idle_silent'
        idle_status_poll_enabled = $false
        size = $manifest.artifacts[0].size
        crc32 = $manifest.artifacts[0].crc32
        sha256 = $manifest.artifacts[0].sha256
    }
    $releaseJson = ($releaseManifest | ConvertTo-Json) -replace "`r`n", "`n"
    [System.IO.File]::WriteAllText((Join-Path $stagingDirectory 'release.manifest.json'),
        $releaseJson + "`n", [System.Text.UTF8Encoding]::new($false))

    $readme = [System.IO.File]::ReadAllText($readmeTemplate, [System.Text.Encoding]::UTF8)
    $readme = $readme.Replace('{{FIRMWARE_VERSION}}', $FirmwareVersion)
    $readme = $readme.Replace('{{BOOT_VERSION}}', $BootVersion)
    $readme = $readme.Replace('{{APP_SIZE}}', [string]$app.Length)
    $readme = $readme.Replace('{{APP_CRC32}}', [string]$manifest.artifacts[0].crc32)
    $readme = $readme.Replace('{{FULL_SHA256}}', [string]$manifest.artifacts[2].sha256)
    [System.IO.File]::WriteAllText((Join-Path $stagingDirectory 'README_FLASH.md'),
        ($readme -replace "`r`n", "`n"), [System.Text.UTF8Encoding]::new($false))

    $expectedNames = @(
        'ABox_Boot.bin',
        'TopFlyingWing_App.bin',
        'TopFlyingWing_Full.bin',
        'TopFlyingWing_BootState.bin',
        'README_FLASH.md',
        'manifest.json',
        'release.manifest.json'
    )
    $actualNames = @(Get-ChildItem -LiteralPath $stagingDirectory -File | Select-Object -ExpandProperty Name)
    if (@(Compare-Object $expectedNames $actualNames).Count -ne 0) {
        throw 'Staged release does not contain exactly the expected formal artifacts'
    }

    # Validate the complete staged package before replacing the formal release.
    & python $packageValidator $stagingDirectory --version $FirmwareVersion | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'Staged release validation failed' }

    if (Test-Path -LiteralPath $outputDirectory) {
        if (-not (Test-Path -LiteralPath $outputDirectory -PathType Container)) {
            throw "Release output is not a directory: $outputDirectory"
        }
        Move-Item -LiteralPath $outputDirectory -Destination $previousDirectory
        $previousMoved = $true
    }

    try {
        Move-Item -LiteralPath $stagingDirectory -Destination $outputDirectory
    }
    catch {
        if ($previousMoved -and -not (Test-Path -LiteralPath $outputDirectory)) {
            Move-Item -LiteralPath $previousDirectory -Destination $outputDirectory
            $previousMoved = $false
        }
        throw
    }

    if ($previousMoved) {
        Remove-Item -LiteralPath $previousDirectory -Recurse -Force
        $previousMoved = $false
    }
}
finally {
    if (Test-Path -LiteralPath $stagingDirectory) {
        Remove-Item -LiteralPath $stagingDirectory -Recurse -Force
    }
}

Get-ChildItem -LiteralPath $outputDirectory | Select-Object Name, Length
