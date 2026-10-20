param([string]$Exe = 'build\ninja-ccache\barony.exe', [string]$Out = 'C:\Apocrypha\diagnostics\verify-all', [switch]$NoShots)
# verify_all.ps1 -- run every feature's own test on ONE build, then the screenshot steps, and write one table.
# Offline tests first (seconds), then headless launches (each takes a D70 slot inside its own tool), then
# windowed screenshot runs (automated launches never move or grab the mouse: apx::noInputGrab).
# Every step records exit code, wall seconds and its last lines; nothing stops the run early.
$ErrorActionPreference = 'Continue'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
Set-Location $root
$exe = (Resolve-Path $Exe).Path
New-Item -ItemType Directory -Force $Out | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$rows = @()
function Step([string]$area, [string]$name, [scriptblock]$cmd, [string]$passPattern) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $log = Join-Path $Out "$stamp-$($name -replace '[^A-Za-z0-9]+','_').log"
    $text = (& $cmd 2>&1 | Out-String)
    $code = $LASTEXITCODE
    Set-Content $log $text
    $pass = if ($passPattern) { $text -match $passPattern -and $code -eq 0 } else { $code -eq 0 }
    $tail = (($text -split "`n") | Where-Object { $_.Trim() } | Select-Object -Last 2) -join ' / '
    $script:rows += [pscustomobject]@{ area = $area; test = $name; result = $(if ($pass) { 'PASS' } else { 'FAIL' }); exit = $code; seconds = [int]$sw.Elapsed.TotalSeconds; tail = $tail.Substring(0, [Math]::Min(180, $tail.Length)); log = $log }
    "{0,-5} {1,-10} {2,-34} {3,5}s  {4}" -f $(if ($pass) { 'PASS' } else { 'FAIL' }), $area, $name, [int]$sw.Elapsed.TotalSeconds, $tail.Substring(0, [Math]::Min(110, $tail.Length))
}
$env:BARONY_DATADIR = 'C:\Program Files (x86)\Steam\steamapps\common\Barony'

# --- offline ---
Step 'content' 'hollow lint (H1b)' { python -I tools\apx\hollow_lint.py } ''
Step 'content' 'variants lint (BV1)' { python -I tools\apx\variants_lint.py } ''
Step 'content' 'props lint (BP1)' { python -I tools\apx\props_lint.py } ''
Step 'gear' 'gear content test (F6 data)' { python -I tests\apx\test_gear_content.py } ''
Step 'combat' 'impact units (F4b)' { pwsh -NoProfile -File tools\apx\impact_test.ps1 } ''
Step 'combat' 'move units (F4a/F4d)' { pwsh -NoProfile -File tools\apx\unit\run_unit.ps1 } ''
Step 'ui' 'inventory logic units (U1)' { pwsh -NoProfile -File tools\apx\tests\run_u1_logic.ps1 } ''
Step 'voxel' 'BX1 voxel units' { pwsh -NoProfile -File tools\apx\bx1_test.ps1 } ''
Step 'tools' 'test selection units' { python -I tools\apx\test_select_test.py } 'PASS'

# --- headless in-game ---
Step 'all' 'selftest-all (7 stages)' { pwsh -NoProfile -File tools\apx\selftest_all.ps1 -Exe $exe -Tag verify -Headless } 'SELFTEST-ALL: PASS'
Step 'parity' 'golden parity (vanilla floors)' { pwsh -NoProfile -File tools\apx\golden_parity.ps1 -Exe $exe -Tag verify } 'GOLDEN PARITY: 3 pass, 0 fail'
Step 'combat' 'F4c poise/zones/downed' { python tools\apx\f4c_selftest.py --exe $exe } ''
Step 'gear' 'F6 save, visual, net gates' { python tools\apx\gate_f6.py --exe $exe --all } ''
Step 'world' 'props probe (BP1)' { python tools\apx\props_probe.py --exe $exe } ''
Step 'world' 'Hollow content probe (H1b)' { python tools\apx\bh1b_probe.py --exe $exe } ''
Step 'ui' 'inventory/bench selftest (U1)' { pwsh -NoProfile -File tools\apx\u1_selftest.ps1 -Exe $exe } ''
Step 'world' 'F2 floors sweep 8 seeds' { python tools\apx\gate_f2.py sweep --exe $exe --first 2001 --count 8 --per-process 4 } ''
Step 'world' 'F2 host = client' { python tools\apx\gate_f2.py mp --exe $exe --seeds 3001 --sizes 128 } ''
Step 'tactics' 'F3 crowd trials' { python tools\apx\bf3_verify.py --tag verify --skip-exclusive --skip-parity } ''

# --- screenshots (windowed; the mouse is never touched) ---
if (-not $NoShots) {
    Step 'shots' 'vision shots (gen128)' { pwsh -NoProfile -File tools\apx\vis_shots.ps1 -Exe $exe -Tag verify -Floor gen128 } ''
    Step 'shots' 'inventory/bench shots' { pwsh -NoProfile -File tools\apx\u1_selftest.ps1 -Exe $exe -Shots } ''
}

$csv = Join-Path $Out "$stamp-summary.csv"
$rows | Export-Csv $csv -NoTypeInformation
"SUMMARY: $(@($rows | Where-Object result -eq 'PASS').Count) pass, $(@($rows | Where-Object result -eq 'FAIL').Count) fail -> $csv"
