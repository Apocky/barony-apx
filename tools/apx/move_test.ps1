# apx M1 offline gate: compile and run move_test (seconds, no Barony).
$ErrorActionPreference = 'Stop'
$root = Resolve-Path "$PSScriptRoot\..\.."
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
if (-not $vs) { $vs = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools" }
$out = "$root\build\apx-tests"
New-Item -ItemType Directory -Force $out | Out-Null
$cmd = "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && cl /nologo /std:c++17 /O2 /W4 /EHsc " +
       "`"$root\tools\apx\move_test.cpp`" /Fo`"$out\\`" /Fe`"$out\move_test.exe`""
cmd /c $cmd
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& "$out\move_test.exe"
exit $LASTEXITCODE
