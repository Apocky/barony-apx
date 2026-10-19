# tools/apx/gates_f2.ps1 -- the F2 gate matrix as one batched background job (lane protocol v2).
# Locks are taken per launch by gate_f2.py (rule 1 / D70): correctness sweeps take slot files
# (the three size sweeps run in parallel, one slot each); timing (w1) and host+client (mp) take the
# exclusive lock. Rows: <Out>\f2.jsonl; each step's console: <Out>\gates-<step>.log.
param(
    [string]$Exe = (Join-Path $PSScriptRoot '..\..\build\vs2022-f2\Release\barony.exe'),
    [string]$Out = 'C:\Apocrypha\diagnostics\barony-f2',
    [int]$First = 2001,
    [int]$Count = 50
)
$py = Join-Path $PSScriptRoot 'gate_f2.py'
$common = @('--exe', $Exe, '--out', $Out, '--wait-foreign', '7200')
function Start-Step($name, [string[]]$a) {
    Write-Host "[gates] $name start $((Get-Date).ToString('HH:mm:ss'))"
    Start-Process python -ArgumentList (@("`"$py`"") + $a + $common) -NoNewWindow -PassThru `
        -RedirectStandardOutput (Join-Path $Out "gates-$name.log") -RedirectStandardError (Join-Path $Out "gates-$name.err")
}
function Wait-Steps($procs) {
    foreach ($p in $procs) { $p.WaitForExit(); Write-Host "[gates] pid $($p.Id) exit $($p.ExitCode) $((Get-Date).ToString('HH:mm:ss'))" }
}
# correctness: W2 + D72 at each size in parallel slots, then the two controls and the W5 repeat
Wait-Steps @(
    (Start-Step 'w2-128' @('sweep', '--size', '128', '--first', "$First", '--count', "$Count", '--per-process', '4')),
    (Start-Step 'w2-96'  @('sweep', '--size', '96',  '--first', "$First", '--count', "$Count", '--per-process', '4')),
    (Start-Step 'w2-80'  @('sweep', '--size', '80',  '--first', "$First", '--count', "$Count", '--per-process', '4'))
)
Wait-Steps @(
    (Start-Step 'w2-control'  @('sweep', '--first', "$First", '--count', '4', '--per-process', '4', '--control', '1')),
    (Start-Step 'd72-control' @('sweep', '--first', "$First", '--count', '4', '--per-process', '4', '--control', '2')),
    (Start-Step 'w5-repeat'   @('sweep', '--first', "$First", '--count', '12', '--per-process', '4'))
)
# exclusive: timing, then host+client
Wait-Steps @((Start-Step 'w1' @('w1', '--seeds', '3001', '--sizes', '128,96,80,vanilla')))
Wait-Steps @((Start-Step 'mp' @('mp', '--seeds', '3001', '--sizes', '128,96,80,vanilla')))
Get-ChildItem $Out -Filter 'gates-*.log' | ForEach-Object { Select-String $_.FullName -Pattern 'rows \d+ ok' | ForEach-Object { "$($_.Filename): $($_.Line)" } }
Write-Host "[gates] done $((Get-Date).ToString('HH:mm:ss'))"
