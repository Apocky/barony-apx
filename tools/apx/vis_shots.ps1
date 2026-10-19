# tools/apx/vis_shots.ps1 -- BVIS1 (D84): far and near screenshots on a test floor, with eye.py luma numbers.
#
# Usage: pwsh -NoProfile -File tools\apx\vis_shots.ps1 -Exe <barony.exe> -Tag <name> [-Floor vanilla|gen128] [-Env "K=V,..."]
# One windowed launch (screenshots need a visible window, rule 14) in a D70 slot: -quickstart=barbarian -apxfloor=1,
# -apxvisshots aims at the longest line of sight (vis-far.png) and the nearest wall (vis-near.png), then quits.
# Output: C:\Apocrypha\diagnostics\barony-vis\<Tag>-<Floor>\ (PNGs, eye JSON, log) and one row in runs.jsonl.
param(
    [Parameter(Mandatory)][string]$Exe,
    [Parameter(Mandatory)][string]$Tag,
    [ValidateSet('vanilla', 'gen128')][string]$Floor = 'vanilla',
    [string]$Env = '',
    [int]$TimeoutSec = 120,
    [string]$Eye = 'C:\Users\Apocky\source\repos\0Labyrinth\tools\eye.py'
)
$root = 'C:\Apocrypha\diagnostics\barony-vis'
$out = Join-Path $root "$Tag-$Floor"
New-Item -ItemType Directory -Force $out | Out-Null
$slots = 'C:\Apocrypha\locks\barony-slots'; $excl = 'C:\Apocrypha\locks\barony'; $want = 'C:\Apocrypha\locks\barony-want'
New-Item -ItemType Directory -Force $slots | Out-Null
$mine = $null
while (-not $mine) {
    $busy = (Get-ChildItem $excl -Filter *.LOCK -EA SilentlyContinue) -or (Get-ChildItem $want -Filter *.WANT -EA SilentlyContinue)
    if (-not $busy) { foreach ($n in 1..3) { $f = Join-Path $slots "slot$n.LOCK"; try { $fs = [IO.File]::Open($f, 'CreateNew'); $fs.Close(); Set-Content $f "vis-$Tag $(Get-Date -Format o)"; $mine = $f; break } catch {} } }
    if (-not $mine) { Start-Sleep 20 }
}
$run = "C:\Apocrypha\runs\vis-$Tag-$Floor-$(Get-Random -Maximum 99999)"
New-Item -ItemType Directory -Force $run | Out-Null
try {
    $env:BARONY_DATADIR = 'C:\Program Files (x86)\Steam\steamapps\common\Barony'
    foreach ($kv in ($Env -split ',' | Where-Object { $_ })) { $k, $v = $kv -split '=', 2; [Environment]::SetEnvironmentVariable($k, $v) }
    $floorArgs = if ($Floor -eq 'gen128') { '-apxgen -apxgen-size=128 -apxmapseed=1001' } else { '-apxmapseed=1234567' }
    $p = Start-Process -FilePath $Exe -WorkingDirectory $run -PassThru `
        -ArgumentList "-windowed -size=1280x720 -quickstart=barbarian -apxfloor=1 $floorArgs -apxvisshots"
    if (-not $p.WaitForExit($TimeoutSec * 1000)) { $p.Kill() }
} finally {
    Remove-Item $mine -Force -EA SilentlyContinue
    foreach ($kv in ($Env -split ',' | Where-Object { $_ })) { [Environment]::SetEnvironmentVariable(($kv -split '=', 2)[0], $null) }
}
Copy-Item "$run\log.txt" (Join-Path $out 'log.txt') -Force -EA SilentlyContinue
$visLine = (Select-String "$run\log.txt" -Pattern '\[apx-vis\] shots done.*' -EA SilentlyContinue | Select-Object -Last 1).Matches.Value
$row = [ordered]@{ ts = (Get-Date -Format o); tag = $Tag; floor = $Floor; env = $Env; vis = $visLine }
foreach ($v in 'far', 'near') {
    $png = Join-Path $run "vis-$v.png"
    if (-not (Test-Path $png)) { $row[$v] = 'missing'; continue }
    Copy-Item $png (Join-Path $out "vis-$v.png") -Force
    $j = Join-Path $out "eye-$v.json"
    & python $Eye $png --json $j --quiet 2>&1 | Out-Null
    $row[$v] = if (Test-Path $j) { Get-Content $j -Raw | ConvertFrom-Json } else { 'eye failed' }
}
$line = $row | ConvertTo-Json -Compress -Depth 6
Add-Content (Join-Path $root 'runs.jsonl') $line
"$Tag $Floor -> $out"
$visLine
