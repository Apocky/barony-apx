# tools/apx/u1_selftest.ps1 -- U1 gates (steps 1-5) in a live game.
# Runs barony.exe -apxfloor=1 -apxselftest=inventory twice: our inventory (default) and the vanilla
# grid control (APX_GRID=1). Each run writes JSONL to C:\Apocrypha\diagnostics\barony-u1 and its
# screenshots into its own run folder, then quits by itself.
# -Slot (D70, rule 1 of specs/barony-fork/11-MASTER-PLAN.md): a correctness run. Before EACH launch it
# waits until C:\Apocrypha\locks\barony is empty and no barony-want\*.WANT exists, takes a free
# barony-slots\slotN.LOCK, runs windowed, and deletes the slot file when that launch ends.
# Without -Slot: the exclusive lock (no other Barony process at all).
param(
    [string]$Exe = (Join-Path $PSScriptRoot '..\..\build\vs2022\Release\barony.exe'),
    [string]$LockRoot = 'C:\Apocrypha\locks',
    [string]$RunRoot = 'C:\Apocrypha\runs',
    [int]$WaitMinutes = 120,
    [switch]$Slot
)
$ErrorActionPreference = 'Stop'
$Exe = (Resolve-Path $Exe).Path
$excl = Join-Path $LockRoot 'barony'
$slots = Join-Path $LockRoot 'barony-slots'
$want = Join-Path $LockRoot 'barony-want'
$deadline = (Get-Date).AddMinutes($WaitMinutes)

function Take-Lock {
    while ($true) {
        if ((Get-Date) -gt $deadline) { throw "no free Barony run within $WaitMinutes min" }
        $exclLocks = Get-ChildItem $excl -Filter *.LOCK -ErrorAction SilentlyContinue | Where-Object Name -ne 'barony-u1.LOCK'
        $wants = Get-ChildItem $want -Filter *.WANT -ErrorAction SilentlyContinue
        if ($Slot) {
            if (-not $exclLocks -and -not $wants) {
                New-Item -ItemType Directory -Force $slots | Out-Null
                foreach ($n in 1..3) {
                    $f = Join-Path $slots "slot$n.LOCK"
                    try { New-Item -ItemType File -Path $f -Value "u1 $(Get-Date -Format o)" -ErrorAction Stop | Out-Null; return $f } catch { }
                }
            }
        } else {
            $procs = Get-Process barony* -ErrorAction SilentlyContinue
            $slotLocks = Get-ChildItem $slots -Filter *.LOCK -ErrorAction SilentlyContinue
            if (-not $exclLocks -and -not $procs -and -not $slotLocks -and -not $wants) {
                $f = Join-Path $excl 'barony-u1.LOCK'
                try { New-Item -ItemType File -Path $f -ErrorAction Stop | Out-Null } catch { Start-Sleep 5; continue }
                if (Get-ChildItem $excl -Filter *.LOCK | Where-Object Name -ne 'barony-u1.LOCK') { Remove-Item $f; Start-Sleep 20; continue }
                return $f
            }
        }
        Start-Sleep 15
    }
}

$env:BARONY_DATADIR = 'C:\Program Files (x86)\Steam\steamapps\common\Barony'
try {
    foreach ($mode in @('apx', 'vanilla')) {
        $lock = Take-Lock
        "lock $lock taken $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')"
        try {
            $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
            $run = Join-Path $RunRoot "u1-$mode-$stamp"
            New-Item -ItemType Directory -Path $run | Out-Null
            if ($mode -eq 'vanilla') { $env:APX_GRID = '1' } else { Remove-Item Env:APX_GRID -ErrorAction SilentlyContinue }
            $t0 = Get-Date
            $p = Start-Process -FilePath $Exe -ArgumentList '-windowed', '-size=1280x720', '-quickstart=barbarian', '-apxfloor=1', '-apxmapseed=1234567', '-apxselftest=inventory' -WorkingDirectory $run -PassThru
            if (-not $p.WaitForExit(300000)) { Stop-Process -Id $p.Id -Force; "run $mode TIMEOUT" }
            "run $mode exit $($p.ExitCode) in $([int]((Get-Date) - $t0).TotalSeconds) s at $run"
        } finally {
            Remove-Item $lock -ErrorAction SilentlyContinue
            "lock released $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')"
        }
    }
} finally {
    Remove-Item Env:APX_GRID -ErrorAction SilentlyContinue
}
