# tools/apx/u1_selftest.ps1 -- U1 steps 1-2 gates in a live game, under the machine's Barony lock.
# Runs barony.exe -apxfloor=1 -apxselftest=inventory twice: our inventory (default) and the vanilla
# grid control (APX_GRID=1). Each run writes JSONL to C:\Apocrypha\diagnostics\barony-u1 and two
# screenshots into its run folder, then quits by itself.
param(
    [string]$Exe = (Join-Path $PSScriptRoot '..\..\build\vs2022\Release\barony.exe'),
    [string]$LockDir = 'C:\Apocrypha\locks\barony',
    [string]$RunRoot = 'C:\Apocrypha\runs',
    [int]$WaitMinutes = 120
)
$ErrorActionPreference = 'Stop'
$Exe = (Resolve-Path $Exe).Path
$lock = Join-Path $LockDir 'barony-u1.LOCK'
$deadline = (Get-Date).AddMinutes($WaitMinutes)
while ($true) {
    $others = Get-ChildItem $LockDir -Filter *.LOCK -ErrorAction SilentlyContinue | Where-Object Name -ne 'barony-u1.LOCK'
    $procs = Get-Process barony* -ErrorAction SilentlyContinue
    if (-not $others -and -not $procs) {
        try { New-Item -ItemType File -Path $lock -ErrorAction Stop | Out-Null } catch { Start-Sleep 5; continue }
        # someone may have taken it in the same second: back off if so
        if (Get-ChildItem $LockDir -Filter *.LOCK | Where-Object Name -ne 'barony-u1.LOCK') { Remove-Item $lock; Start-Sleep 20; continue }
        break
    }
    if ((Get-Date) -gt $deadline) { throw "lock not free within $WaitMinutes min" }
    Start-Sleep 15
}
"lock taken $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')"
try {
    $env:BARONY_DATADIR = 'C:\Program Files (x86)\Steam\steamapps\common\Barony'
    foreach ($mode in @('apx', 'vanilla')) {
        $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
        $run = Join-Path $RunRoot "u1-$mode-$stamp"
        New-Item -ItemType Directory -Path $run | Out-Null
        if ($mode -eq 'vanilla') { $env:APX_GRID = '1' } else { Remove-Item Env:APX_GRID -ErrorAction SilentlyContinue }
        $t0 = Get-Date
        $p = Start-Process -FilePath $Exe -ArgumentList '-windowed', '-size=1280x720', '-quickstart=barbarian', '-apxfloor=1', '-apxmapseed=1234567', '-apxselftest=inventory' -WorkingDirectory $run -PassThru
        if (-not $p.WaitForExit(300000)) { Stop-Process -Id $p.Id -Force; "run $mode TIMEOUT" }
        "run $mode exit $($p.ExitCode) in $([int]((Get-Date) - $t0).TotalSeconds) s at $run"
    }
} finally {
    Remove-Item Env:APX_GRID -ErrorAction SilentlyContinue
    Remove-Item $lock -ErrorAction SilentlyContinue
    "lock released $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')"
}
