# tools/apx/inv_qa.ps1 -- D92 inventory window: one real-input QA run through the QA driver (-apxqa).
# Takes a D70 correctness slot (C:\Apocrypha\locks\barony-slots), launches barony.exe windowed 1280x720 in
# its own run folder with -apxqa=<run>, copies the command script into <run>\cmd.txt, waits for the game to
# quit by itself (the script ends with `quit`), releases the slot. The window is non-activating and the
# OS mouse is never touched (apx::noInputGrab). Output: <run>\status.txt, uidump*.txt, *.png, barony log.
param(
    [Parameter(Mandatory)] [string]$Script,
    [string]$Exe = (Join-Path $PSScriptRoot '..\..\build\ninja-ccache\barony.exe'),
    [string]$RunRoot = 'C:\Apocrypha\runs',
    [string]$Tag = 'invqa',
    [string]$Size = '1280x720',
    [int]$TimeoutSec = 300,
    [string[]]$Extra = @(),
    [string[]]$Env = @()   # NAME=VALUE pairs for this launch only (e.g. the control knob APX_VANILLA_INVENTORY=1)
)
$ErrorActionPreference = 'Stop'
$Exe = (Resolve-Path $Exe).Path
$locks = 'C:\Apocrypha\locks'
$slots = Join-Path $locks 'barony-slots'
$excl = Join-Path $locks 'barony'
$want = Join-Path $locks 'barony-want'
New-Item -ItemType Directory -Force $slots | Out-Null
$lock = $null
$deadline = (Get-Date).AddMinutes(60)
while (-not $lock) {
    if ((Get-Date) -gt $deadline) { throw 'no free Barony slot within 60 min' }
    $steam = Get-Process barony -ErrorAction SilentlyContinue | Where-Object { $_.Path -like '*steamapps*' }
    $busy = (Get-ChildItem $excl -Filter *.LOCK -ErrorAction SilentlyContinue) -or (Get-ChildItem $want -Filter *.WANT -ErrorAction SilentlyContinue) -or $steam
    if (-not $busy) {
        foreach ($n in 1..3) {
            $f = Join-Path $slots "slot$n.LOCK"
            try { New-Item -ItemType File -Path $f -Value "inv-qa $Tag $(Get-Date -Format o)" -ErrorAction Stop | Out-Null; $lock = $f; break } catch { }
        }
    }
    if (-not $lock) { Start-Sleep 10 }
}
try {
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $run = Join-Path $RunRoot "$Tag-$stamp"
    New-Item -ItemType Directory -Path $run | Out-Null
    Copy-Item $Script (Join-Path $run 'cmd.txt')
    if ($Size -ne '1280x720') {   # the game's config.json wins over -size=: seed it from toolspx\qa\config_base.json with the resolution
        $w, $h = $Size -split 'x'
        New-Item -ItemType Directory -Force (Join-Path $run 'config') | Out-Null
        (Get-Content (Join-Path $PSScriptRoot 'qa\config_base.json') -Raw) -replace '"resolution_x": \d+', "`"resolution_x`": $w" -replace '"resolution_y": \d+', "`"resolution_y`": $h" | Set-Content (Join-Path $run 'config\config.json')
    }
    $env:BARONY_DATADIR = 'C:\Program Files (x86)\Steam\steamapps\common\Barony'
    foreach ($kv in $Env) { $k, $v = $kv -split '=', 2; Set-Item -Path "Env:$k" -Value $v }
    "slot $lock run $run $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')"
    $args = @('-windowed', "-size=$Size", '-quickstart=barbarian', '-apxfloor=1', '-apxmapseed=1234567', "-apxqa=$run") + $Extra
    $t0 = Get-Date
    $p = Start-Process -FilePath $Exe -ArgumentList $args -WorkingDirectory $run -PassThru
    if (-not $p.WaitForExit($TimeoutSec * 1000)) { Stop-Process -Id $p.Id -Force; "TIMEOUT after $TimeoutSec s" }
    "exit $($p.ExitCode) in $([int]((Get-Date) - $t0).TotalSeconds) s"
    "RUN=$run"
} finally {
    foreach ($kv in $Env) { $k = ($kv -split '=', 2)[0]; Remove-Item -Path "Env:$k" -ErrorAction SilentlyContinue }
    Remove-Item $lock -ErrorAction SilentlyContinue
    "slot released $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')"
}
