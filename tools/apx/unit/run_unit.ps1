# Compile and run the offline unit tests (no Barony). Uses cl.exe from PATH, else Visual Studio's vcvars64.
$ErrorActionPreference = 'Stop'
$out = Join-Path $env:TEMP 'apx_unit'; New-Item -ItemType Directory $out -Force | Out-Null
$src = Join-Path $PSScriptRoot 'test_moves.cpp'
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
  $vw = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
  $vs = & $vw -latest -property installationPath
  cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && cl /nologo /std:c++17 /EHsc /W3 /Fe:$out\test_moves.exe /Fo:$out\ `"$src`"" | Select-Object -Last 3
} else {
  cl.exe /nologo /std:c++17 /EHsc /W3 /Fe:$out\test_moves.exe /Fo:$out\ $src | Select-Object -Last 3
}
& "$out\test_moves.exe"; exit $LASTEXITCODE
