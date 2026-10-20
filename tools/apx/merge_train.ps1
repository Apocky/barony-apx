param([string[]]$Branches = @(), [switch]$Push, [string]$Tag = 'train')
# Merge train (D77): merge the named origin branches, build, run the combined self-test and golden parity,
# and push HEAD to apx-main ONLY when the build is fresh (exe newer than HEAD's commit) and both gates pass.
# Stops at the first merge conflict (resolve it, commit, run again with no -Branches).
$ErrorActionPreference = 'Stop'
Set-Location (Resolve-Path (Join-Path $PSScriptRoot '..\..'))
git fetch -q origin
foreach ($b in $Branches) {
    git merge --no-edit -q "origin/$b" 2>&1 | Out-Null
    $u = git diff --name-only --diff-filter=U
    if ($u) { "CONFLICT in $b : $($u -join ', ')"; exit 2 }
    "merged $b -> $(git log --oneline -1)"
}
$t0 = Get-Date
$build = pwsh -NoProfile -File tools\apx\build.ps1 -Cache ccache 2>&1
$exe = 'build\ninja-ccache\barony.exe'
$fresh = (Test-Path $exe) -and ((Get-Item $exe).LastWriteTime -ge $t0.AddSeconds(-5)) -or ($build -match 'no work to do')
$buildErr = $build | Select-String -Pattern ': (fatal )?error [A-Z]+[0-9]+|^FAILED:|ninja: build stopped' | Select-Object -First 3
"build: fresh=$fresh $(if ($buildErr) { 'ERRORS: ' + ($buildErr -join ' | ') })"
if (-not $fresh -or $buildErr) { 'NOT PUSHED: build did not produce a fresh exe'; exit 3 }
$exe = (Resolve-Path $exe).Path
$st = pwsh -NoProfile -File tools\apx\selftest_all.ps1 -Exe $exe -Tag $Tag -Headless 2>&1
$st | Select-String 'SELFTEST-ALL|FAIL' | Select-Object -Last 4
$stOk = [bool]($st | Select-String 'SELFTEST-ALL: PASS')
$gp = pwsh -NoProfile -File tools\apx\golden_parity.ps1 -Exe $exe -Tag $Tag
$gp | Select-Object -Last 1
$gpOk = [bool]($gp | Select-String 'GOLDEN PARITY: 3 pass, 0 fail')
"gates: selftest=$stOk parity=$gpOk head=$(git rev-parse --short HEAD)"
if ($Push -and $stOk -and $gpOk) { git push -q origin HEAD:refs/heads/apx-main 2>&1 | Out-Null; git ls-remote origin apx-main }
elseif ($Push) { 'NOT PUSHED: a gate failed'; exit 4 }
