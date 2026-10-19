# apx F4b offline gate: regenerate the material table, compile and run impact_test (seconds, no Barony).
$ErrorActionPreference = 'Stop'
$root = Resolve-Path "$PSScriptRoot\..\.."
python "$root\tools\apx\gen_materials.py"
if ($LASTEXITCODE) { exit $LASTEXITCODE }
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
$out = "$root\build\apx-tests"
New-Item -ItemType Directory -Force $out | Out-Null
$cmd = "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && cl /nologo /std:c++17 /O2 /W4 /EHsc " +
       "`"$root\tools\apx\impact_test.cpp`" `"$root\src\apx\combat\impact.cpp`" /Fo`"$out\\`" /Fe`"$out\impact_test.exe`""
cmd /c $cmd
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& "$out\impact_test.exe"
exit $LASTEXITCODE
