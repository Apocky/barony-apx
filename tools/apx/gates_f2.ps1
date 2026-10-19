# tools/apx/gates_f2.ps1 -- the F2 gate matrix in one run (W2 50 seeds + control, W5 repeat, W1 sizes).
# Each step waits for other sessions' *.LOCK in C:\Apocrypha\locks and any running Barony.
param(
    [string]$Exe = (Join-Path $PSScriptRoot '..\..\build\vs2022-f2\Release\barony.exe'),
    [string]$Out = 'C:\Apocrypha\diagnostics\barony-f2',
    [string]$LockDir = 'C:\Apocrypha\locks',
    [int]$First = 2001,
    [int]$Count = 50
)
$py = Join-Path $PSScriptRoot 'gate_f2.py'
$common = @('--exe', $Exe, '--out', $Out, '--lock-dir', $LockDir, '--wait-foreign', '5400')
function Step($name, [string[]]$a) {
    $t = Get-Date
    Write-Host "[gates] $name start $($t.ToString('HH:mm:ss'))"
    python $py @a @common
    Write-Host ("[gates] {0} exit {1} after {2:n0} s" -f $name, $LASTEXITCODE, ((Get-Date) - $t).TotalSeconds)
}
Step 'w2-sweep'   @('sweep', '--first', "$First", '--count', "$Count", '--per-process', '5')
Step 'w2-control' @('sweep', '--first', "$First", '--count', '5', '--per-process', '5', '--control', '1')
Step 'w5-repeat'  @('sweep', '--first', "$First", '--count', '10', '--per-process', '5')
Step 'w1'         @('w1', '--seeds', '3001', '--sizes', '128,96,48,vanilla')
