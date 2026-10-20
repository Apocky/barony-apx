# tools/apx/bx1_test.ps1 -- BX1 offline gates (seconds, no Barony): compile tools/apx/bx1_test.cpp with src/apx/vox/bx1.cpp and run.
# Pass -Break to run the control (APX_BX1_BREAK=1): G-gen must FAIL.
param([switch]$Break, [string]$ModelsDir = 'C:\Program Files (x86)\Steam\steamapps\common\Barony\models')
$ErrorActionPreference = 'Stop'
$root = Resolve-Path "$PSScriptRoot\..\.."
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
$out = "$root\build\apx-tests"
New-Item -ItemType Directory -Force $out | Out-Null
$cmd = "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && cl /nologo /std:c++17 /O2 /W4 /EHsc " +
       "`"$root\tools\apx\bx1_test.cpp`" `"$root\src\apx\vox\bx1.cpp`" /Fo`"$out\\`" /Fe`"$out\bx1_test.exe`""
cmd /c $cmd | Where-Object { $_ -match 'error|warning' }
if ($LASTEXITCODE) { exit $LASTEXITCODE }
if ($Break) { $env:APX_BX1_BREAK = '1' }
try { & "$out\bx1_test.exe" $ModelsDir } finally { Remove-Item Env:APX_BX1_BREAK -ErrorAction SilentlyContinue }
exit $LASTEXITCODE
