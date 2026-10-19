# tools/apx/selftest_all.ps1 -- one launch of -apxselftest=all (D77): every in-game self-test on floor 1.
#
# Usage: pwsh -NoProfile -File tools\apx\selftest_all.ps1 -Exe <barony.exe> [-Tag name] [-Headless] [-Env "K=V,K2=V2"]
# Takes a D70 slot (C:\Apocrypha\locks\barony-slots), runs the game from its own folder against the Steam data,
# waits for the summary (or -TimeoutSec), prints one line per check and exits 0 when every check passed.
# Controls: -Env "APX_MOVES=0" (moves must FAIL), APX_TIER_DAMAGE=0 (tierdmg), APX_GRID=1 (inventory),
# APX_POISE=0 / APX_ZONES=0 / APX_DOWNED=0 (f4c, features).
param(
    [Parameter(Mandatory)][string]$Exe,
    [string]$Tag = 'selftest',
    [switch]$Headless,
    [string]$Env = '',
    [int]$Seed = 1234567,
    [int]$TimeoutSec = 180
)
$slots = 'C:\Apocrypha\locks\barony-slots'; $excl = 'C:\Apocrypha\locks\barony'; $want = 'C:\Apocrypha\locks\barony-want'
New-Item -ItemType Directory -Force $slots | Out-Null
$mine = $null
while (-not $mine) {
    $busy = (Get-ChildItem $excl -Filter *.LOCK -EA SilentlyContinue) -or (Get-ChildItem $want -Filter *.WANT -EA SilentlyContinue)
    if (-not $busy) { foreach ($n in 1..3) { $f = Join-Path $slots "slot$n.LOCK"; try { $fs = [IO.File]::Open($f, 'CreateNew'); $fs.Close(); Set-Content $f "$Tag $(Get-Date -Format o)"; $mine = $f; break } catch {} } }
    if (-not $mine) { Start-Sleep 20 }
}
"slot $mine $(Get-Date -Format HH:mm:ss)"
$dir = "C:\Apocrypha\runs\$Tag-$(Get-Date -Format yyyyMMdd-HHmmss)-$(Get-Random -Maximum 9999)"
New-Item -ItemType Directory -Force $dir | Out-Null
$exitCode = 1
try {
    $env:BARONY_DATADIR = 'C:\Program Files (x86)\Steam\steamapps\common\Barony'
    $env:APX_DIAG_DIR = $dir
    foreach ($kv in ($Env -split ',' | Where-Object { $_ })) { $k, $v = $kv -split '=', 2; [Environment]::SetEnvironmentVariable($k, $v) }
    $args = "-windowed -size=1280x720 -quickstart=barbarian -apxfloor=1 -apxmapseed=$Seed -apxselftest=all"
    if ($Headless) { $args += ' -apxheadless'; $env:APX_HEADLESS_BATCH = '1' } # one logic tick per pass: stages stay seconds of game time, so wandering monsters cannot reach the test character
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $p = Start-Process -FilePath $Exe -WorkingDirectory $dir -ArgumentList $args -PassThru
    $json = Join-Path $dir 'selftest-all.json'
    while (-not $p.HasExited -and $sw.Elapsed.TotalSeconds -lt $TimeoutSec -and -not (Test-Path $json)) { Start-Sleep 1 }
    $p.WaitForExit(15000) | Out-Null
    if (-not $p.HasExited) { $p.Kill() }
    $wall = [math]::Round($sw.Elapsed.TotalSeconds, 1)
    if (-not (Test-Path $json)) { "NO SUMMARY after $wall s (exit $($p.ExitCode)); log: $dir\log.txt"; Select-String "$dir\log.txt" -Pattern '\[apx-selftest\]' | ForEach-Object Line; return }
    $r = Get-Content $json -Raw | ConvertFrom-Json
    foreach ($c in $r.checks.PSObject.Properties) {
        $v = $c.Value; $extra = ($v.PSObject.Properties | Where-Object { $_.Name -notin 'pass', 'ran', 'ms' } | ForEach-Object { "$($_.Name)=$($_.Value)" }) -join ' '
        "{0,-9} {1} {2,6} ms  {3}" -f $c.Name, $(if ($v.pass) { 'PASS' } else { 'FAIL' }), $v.ms, $extra
    }
    "SELFTEST-ALL: $(if ($r.pass) {'PASS'} else {'FAIL'}) ($($r.passed) pass, $($r.failed) fail), game $($r.elapsed_ms) ms, wall $wall s, ended $($r.ended), env '$Env', headless $([bool]$Headless), folder $dir"
    if ($r.pass) { $exitCode = 0 }
} finally {
    Remove-Item $mine -Force -EA SilentlyContinue
    foreach ($kv in ($Env -split ',' | Where-Object { $_ })) { $k = ($kv -split '=', 2)[0]; [Environment]::SetEnvironmentVariable($k, $null) }
}
exit $exitCode
