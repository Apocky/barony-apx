# Builds and runs the U1 pure-logic units with MSVC (no Barony, seconds). Exit 0 = pass, and the
# control run (--control) must fail.
$ErrorActionPreference = 'Stop'
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
$out = Join-Path $env:TEMP 'apx-u1-logic'
New-Item -ItemType Directory -Force $out | Out-Null
$src = Join-Path $PSScriptRoot 'u1_logic_test.cpp'
$exe = Join-Path $out 'u1_logic_test.exe'
$obj = Join-Path $out 'u1_logic_test.obj'
cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && cl /nologo /std:c++17 /EHsc /O2 /W3 `"$src`" /Fe:`"$exe`" /Fo:`"$obj`"" | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'compile failed' }
& $exe; $gate = $LASTEXITCODE
& $exe --control; $ctrl = $LASTEXITCODE
"gate exit $gate (want 0), control exit $ctrl (want 1)"
if ($gate -ne 0 -or $ctrl -eq 0) { exit 1 }
