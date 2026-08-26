$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root "tests\test_wing.exe"
& "D:\MinGW\bin\gcc.exe" -std=c11 -Wall -Wextra `
  -I "$root\app\Core\User\Inc" `
  "$root\tests\test_wing.c" "$root\app\Core\User\Src\wing_protocol.c" "$root\app\Core\User\Src\wing_state.c" -o $out
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $out
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "Wing protocol/state host tests passed"

$controllerOut = Join-Path $root "tests\test_wing_controller.exe"
& "D:\MinGW\bin\gcc.exe" -std=c11 -Wall -Wextra `
  -DTOP_WING_CAN_BUS=2 `
  -I "$root\app\Core\User\Inc" `
  -I "$root\tests\stubs" `
  "$root\tests\test_wing_controller.c" `
  "$root\app\Core\User\Src\wing_controller.c" `
  "$root\app\Core\User\Src\wing_protocol.c" `
  "$root\app\Core\User\Src\wing_state.c" -o $controllerOut
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $controllerOut
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "Wing controller remote-session host tests passed"

$logOut = Join-Path $root "tests\test_app_log.exe"
& "D:\MinGW\bin\gcc.exe" -std=c11 -Wall -Wextra `
  -I "$root\tests\stubs" `
  -I "$root\app\Core\User\Inc" `
  "$root\tests\test_app_log.c" "$root\app\Core\User\Src\app_log.c" -o $logOut
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $logOut
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "App log batching host tests passed"

$otaOut = Join-Path $root "tests\test_ota_response.exe"
& "D:\MinGW\bin\gcc.exe" -std=c11 -Wall -Wextra -ffunction-sections -fdata-sections `
  -DTOP_WING_HOST_TEST -DTOP_WING_CAN_BUS=2 `
  -I "$root\tests\stubs" `
  -I "$root\app\Core\User\Inc" `
  -I "$root\platform\include" `
  -I "$root\platform\components\cjson" `
  "$root\tests\test_ota_response.c" `
  "$root\app\Core\User\Src\mqtt_app_cmd.c" `
  "$root\platform\components\cjson\cJSON.c" `
  "-Wl,--gc-sections" -o $otaOut
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $otaOut
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "OTA response sequencing host tests passed"
