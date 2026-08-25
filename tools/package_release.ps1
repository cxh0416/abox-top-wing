param(
    [string]$OutputDirectory = (Join-Path (Split-Path -Parent $PSScriptRoot) "dist"),
    [string]$BootPath = "", [string]$AppPath = "",
    [string]$FirmwareVersion = "top-flying-wing-1.0.1",
    [string]$BootVersion = "abox-boot-2.3.0"
)
$root = Split-Path -Parent $PSScriptRoot
if (-not $BootPath) { $BootPath = Join-Path $root "platform\targets\stm32f105_abox_boot\dist\ABox_Boot.bin" }
if (-not $AppPath) { $AppPath = Join-Path $root "app\build\Release\TopFlyingWing_App.bin" }
$appOffset = 0x8000
foreach ($path in @($BootPath,$AppPath)) { if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Release artifact not found: $path" } }
$boot=[IO.File]::ReadAllBytes($BootPath);$app=[IO.File]::ReadAllBytes($AppPath)
if($boot.Length -gt $appOffset){throw "Boot image exceeds 0x8000-byte region"}
New-Item -ItemType Directory -Force -Path $OutputDirectory|Out-Null
$bootOut=Join-Path $OutputDirectory "ABox_Boot.bin";$appOut=Join-Path $OutputDirectory "TopFlyingWing_App.bin";$fullOut=Join-Path $OutputDirectory "TopFlyingWing_Full.bin"
[IO.File]::WriteAllBytes($bootOut,$boot);[IO.File]::WriteAllBytes($appOut,$app)
$full=New-Object byte[] ($appOffset+$app.Length);for($i=0;$i-lt$full.Length;$i++){$full[$i]=0xFF};[Array]::Copy($boot,0,$full,0,$boot.Length);[Array]::Copy($app,0,$full,$appOffset,$app.Length);[IO.File]::WriteAllBytes($fullOut,$full)
function Get-Crc32([byte[]]$b){[int64]$c=0xFFFFFFFF;foreach($x in $b){$c=$c-bxor[int64]$x;if($c-lt0){$c+=4294967296};for($i=0;$i-lt8;$i++){if($c-band1){$c=($c-shr1)-bxor0xEDB88320}else{$c=$c-shr1};if($c-lt0){$c+=4294967296}}};([Convert]::ToString([uint32](4294967295-$c),16).PadLeft(8,'0').ToUpperInvariant())}
function Artifact($path,$name,$address){$b=[IO.File]::ReadAllBytes($path);[ordered]@{name=$name;size=$b.Length;load_address=$address;crc32=Get-Crc32 $b;sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant();includes_device_config=$false;includes_boot_state=$false}}
$manifest=[ordered]@{manifest_version=1;product="top_flying_wing";firmware_version=$FirmwareVersion;boot_version=$BootVersion;ota_scheme="boot_v2_ufs";ota_downloader="app";app_range="0x08008000..0x0803E7FF";boot_state_pages=@("0x0803E800","0x0803F000");config_page="0x0803F800";artifacts=@((Artifact $appOut "TopFlyingWing_App.bin" "0x08008000"),(Artifact $bootOut "ABox_Boot.bin" "0x08000000"),(Artifact $fullOut "TopFlyingWing_Full.bin" "0x08000000"))}
$manifest|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $OutputDirectory "manifest.json") -Encoding utf8
[ordered]@{schema_version=1;product="top_flying_wing";version=$FirmwareVersion;artifact="top_flying_wing_app.bin";file="TopFlyingWing_App.bin";immutable=$true;size=$manifest.artifacts[0].size;crc32=$manifest.artifacts[0].crc32;sha256=$manifest.artifacts[0].sha256}|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $OutputDirectory "release.manifest.json") -Encoding utf8
Get-ChildItem -LiteralPath $OutputDirectory|Select-Object Name,Length
