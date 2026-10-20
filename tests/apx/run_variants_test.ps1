# Compile and run the offline variants unit test (no Barony). Finds vcvars64 under Visual Studio (any edition) when cl.exe is not on PATH.
$ErrorActionPreference = 'Stop'
$out = Join-Path $env:TEMP 'apx_variants'; New-Item -ItemType Directory $out -Force | Out-Null
$src = Join-Path $PSScriptRoot 'variants_test.cpp'
if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
  cl.exe /nologo /std:c++17 /EHsc /W3 /O2 /Fe:$out\variants_test.exe /Fo:$out\ $src | Select-Object -Last 3
} else {
  $vc = Get-ChildItem "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022", "$env:ProgramFiles\Microsoft Visual Studio\2022" -Recurse -Filter vcvars64.bat -ErrorAction SilentlyContinue | Select-Object -First 1
  cmd /c "`"$($vc.FullName)`" >nul && cl /nologo /std:c++17 /EHsc /W3 /O2 /Fe:$out\variants_test.exe /Fo:$out\ `"$src`"" | Select-Object -Last 3
}
& "$out\variants_test.exe"; exit $LASTEXITCODE
