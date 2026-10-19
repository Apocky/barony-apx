# apx F4b: build, take a D70 slot, run ONE launch of the impact self-test, write results.
# Result JSON: C:\Apocrypha\diagnostics\barony-f4b\impacttest-<time>.json
$ErrorActionPreference = 'Continue'
$root = Resolve-Path "$PSScriptRoot\..\.."
$diag = 'C:\Apocrypha\diagnostics\barony-f4b'; New-Item -ItemType Directory -Force $diag | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$t0 = Get-Date
pwsh -NoProfile -File "$root\tools\apx\build.ps1" *> "$diag\build-$stamp.log"
$buildExit = $LASTEXITCODE; $buildS = [int]((Get-Date) - $t0).TotalSeconds
"build exit $buildExit in $buildS s"
if ($buildExit) { exit 1 }

$slots = 'C:\Apocrypha\locks\barony-slots'; $excl = 'C:\Apocrypha\locks\barony'; $want = 'C:\Apocrypha\locks\barony-want'
New-Item -ItemType Directory -Force $slots | Out-Null
$mine = $null; $w0 = Get-Date
while (-not $mine) {
    if (-not (Get-ChildItem $excl -Filter *.LOCK -EA SilentlyContinue) -and -not (Get-ChildItem $want -EA SilentlyContinue)) {
        foreach ($n in 1..3) {
            $p = Join-Path $slots "slot$n.LOCK"
            try { $fs = [IO.File]::Open($p, 'CreateNew', 'Write'); $b = [Text.Encoding]::ASCII.GetBytes("f4b impacttest $(Get-Date -Format o)`n"); $fs.Write($b, 0, $b.Length); $fs.Close(); $mine = $p; break } catch {}
        }
    }
    if (-not $mine) { Start-Sleep 30 }
}
$waitS = [int]((Get-Date) - $w0).TotalSeconds
try {
    $run = "C:\Apocrypha\runs\f4b-$stamp"; New-Item -ItemType Directory -Force $run | Out-Null
    $env:BARONY_DATADIR = 'C:\Program Files (x86)\Steam\steamapps\common\Barony'
    $r0 = Get-Date
    $p = Start-Process "$root\build\vs2022\Release\barony.exe" -ArgumentList '-windowed', '-size=1280x720', '-quickstart=barbarian', '-apxfloor=1', '-apxmapseed=1234567', '-apximpacttest' -WorkingDirectory $run -PassThru
    if (-not $p.WaitForExit(180000)) { Stop-Process -Id $p.Id -Force; $timedOut = $true } else { $timedOut = $false }
    $runS = [int]((Get-Date) - $r0).TotalSeconds
} finally { Remove-Item $mine -Force }

$log = Get-Content "$run\log.txt" -EA SilentlyContinue
$rows = @($log | Select-String '\[apx impacttest\] monster')
$summary = ($log | Select-String '\[apx impacttest\] rows' | Select-Object -Last 1).Line
$typed = @($log | Select-String 'You say:').Count
$res = [ordered]@{ stamp = $stamp; slot = (Split-Path $mine -Leaf); build_s = $buildS; slot_wait_s = $waitS; run_s = $runS; timed_out = $timedOut
    rows_logged = $rows.Count; summary = $summary; contaminated = ($typed -gt 0); run_dir = $run }
$res | ConvertTo-Json | Set-Content "$diag\impacttest-$stamp.json" -Encoding ascii
$rows | ForEach-Object { $_.Line } | Set-Content "$diag\impacttest-$stamp-rows.txt" -Encoding ascii
$res | ConvertTo-Json
