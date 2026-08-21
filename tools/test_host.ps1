$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root "tests\test_wing.exe"
& "D:\MinGW\bin\gcc.exe" -std=c11 -Wall -Wextra `
  -I "$root\app\Core\User\Inc" `
  "$root\tests\test_wing.c" "$root\app\Core\User\Src\wing_protocol.c" "$root\app\Core\User\Src\wing_state.c" -o $out
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $out
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "Wing protocol/state host tests passed"
