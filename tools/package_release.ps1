[CmdletBinding()]
param(
    [string]$BootPath = "",
    [string]$AppPath = "",
    [string]$FirmwareVersion = "top-flying-wing-1.1.5",
    [string]$BootVersion = "abox-boot-2.3.0"
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
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
foreach ($path in @($BootPath, $AppPath)) {
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

function Get-Artifact([string]$Path, [string]$Name, [string]$LoadAddress) {
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    [ordered]@{
        name = $Name
        size = $bytes.Length
        load_address = $LoadAddress
        crc32 = Get-Crc32 $bytes
        sha256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
        includes_device_config = $false
        includes_boot_state = $false
    }
}

$token = [Guid]::NewGuid().ToString('N')
$stagingDirectory = Join-Path $root ".dist-staging-$token"
$previousDirectory = Join-Path $root ".dist-previous-$token"
$previousMoved = $false

try {
    New-Item -ItemType Directory -Path $stagingDirectory | Out-Null

    $bootOut = Join-Path $stagingDirectory 'ABox_Boot.bin'
    $appOut = Join-Path $stagingDirectory 'TopFlyingWing_App.bin'
    $fullOut = Join-Path $stagingDirectory 'TopFlyingWing_Full.bin'
    [System.IO.File]::WriteAllBytes($bootOut, $boot)
    [System.IO.File]::WriteAllBytes($appOut, $app)

    $full = New-Object byte[] ($appOffset + $app.Length)
    for ($index = 0; $index -lt $full.Length; $index++) { $full[$index] = 0xFF }
    [Array]::Copy($boot, 0, $full, 0, $boot.Length)
    [Array]::Copy($app, 0, $full, $appOffset, $app.Length)
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
        config_page = '0x0803F800'
        artifacts = @(
            (Get-Artifact $appOut 'TopFlyingWing_App.bin' '0x08008000'),
            (Get-Artifact $bootOut 'ABox_Boot.bin' '0x08000000'),
            (Get-Artifact $fullOut 'TopFlyingWing_Full.bin' '0x08000000')
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

    $expectedNames = @(
        'ABox_Boot.bin',
        'TopFlyingWing_App.bin',
        'TopFlyingWing_Full.bin',
        'manifest.json',
        'release.manifest.json'
    )
    $actualNames = @(Get-ChildItem -LiteralPath $stagingDirectory -File | Select-Object -ExpandProperty Name)
    if (@(Compare-Object $expectedNames $actualNames).Count -ne 0) {
        throw 'Staged release does not contain exactly the expected formal artifacts'
    }

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
