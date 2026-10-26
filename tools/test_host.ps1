[CmdletBinding()]
param([string] $Compiler = 'D:/MinGW/bin/gcc.exe')
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$build = Join-Path $root 'build/host'
& cmake -S (Join-Path $root 'tests') -B $build -G Ninja "-DCMAKE_C_COMPILER=$Compiler" '-DCMAKE_BUILD_TYPE=Debug'
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& cmake --build $build --parallel 4
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& ctest --test-dir $build --output-on-failure
exit $LASTEXITCODE
