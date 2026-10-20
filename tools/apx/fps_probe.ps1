# tools/apx/fps_probe.ps1 -- frame-time profile of a normal windowed play path (the merge train's fps gate).
#
# Each arm runs twice: the start map (-quickstart only) and floor 1 (-apxfloor=1 -apxmapseed=1234567), windowed
# 1280x720 (the frame cost is the draw; noInputGrab: the mouse is never touched), with -apxfpsprobe: 10 s of game time
# uncapped with the inventory closed, then 10 s with it open, each "apxfps: ... p50 ... p99" line parsed into a row
# of C:\Apocrypha\diagnostics\barony-fps\runs.jsonl. Timing runs are EXCLUSIVE (D70): posts barony-want\fps-probe.WANT,
# waits until no slot, lock or Barony remains, holds C:\Apocrypha\locks\barony\fps-probe.LOCK.
#
# Usage: pwsh -NoProfile -File tools\apx\fps_probe.ps1 -Arms "name|exe|extra args|K=V,K2=V2;name2|..." [-Repeat 1]
param([Parameter(Mandatory)][string]$Arms, [int]$Repeat = 1, [string]$Tag = 'fps', [string]$Size = '1280x720')
$out = 'C:\Apocrypha\diagnostics\barony-fps'; New-Item -ItemType Directory -Force $out | Out-Null
$want = 'C:\Apocrypha\locks\barony-want\fps-probe.WANT'; $lock = 'C:\Apocrypha\locks\barony\fps-probe.LOCK'
Set-Content $want "fps probe $(Get-Date -Format o)"
try {
    while ((Get-Process -Name 'barony*' -EA SilentlyContinue) -or (Get-ChildItem 'C:\Apocrypha\locks\barony-slots' -Filter *.LOCK -EA SilentlyContinue) -or
           (Get-ChildItem 'C:\Apocrypha\locks\barony' -Filter *.LOCK -EA SilentlyContinue | Where-Object Name -ne 'fps-probe.LOCK')) { Start-Sleep 5 }
    Set-Content $lock (Get-Date -Format o)
    if (Test-Path $want) { Remove-Item -LiteralPath $want }
    $env:BARONY_DATADIR = 'C:\Program Files (x86)\Steam\steamapps\common\Barony'
    foreach ($r in 1..$Repeat) {
        foreach ($arm in ($Arms -split ';' | Where-Object { $_ })) {
            $name, $exe, $extra, $envs = $arm -split '\|', 4
            foreach ($floor in @(@{ n = 'start'; a = '' }, @{ n = 'floor1'; a = '-apxfloor=1 -apxmapseed=1234567' })) {
                $dir = Join-Path $out ("runs\{0}-{1}-{2}-{3}" -f $Tag, $name, $floor.n, $r)
                if (Test-Path $dir) { Remove-Item -LiteralPath $dir -Recurse -Force }
                New-Item -ItemType Directory -Force $dir | Out-Null
                $appid = Join-Path (Split-Path $exe) 'steam_appid.txt'; if (Test-Path $appid) { Copy-Item $appid $dir } # Steam builds read it from the working folder
                $kvs = @($envs -split ',' | Where-Object { $_ })
                foreach ($kv in $kvs) { $k, $v = $kv -split '=', 2; [Environment]::SetEnvironmentVariable($k, $v) }
                $p = Start-Process -FilePath $exe -WorkingDirectory $dir -PassThru -ArgumentList "-windowed -size=$Size -quickstart=barbarian $($floor.a) $extra -apxfpsprobe"
                if (-not $p.WaitForExit(180000)) { $p.Kill() }
                foreach ($kv in $kvs) { [Environment]::SetEnvironmentVariable(($kv -split '=', 2)[0], $null) }
                $log = Get-Content "$dir\log.txt" -EA SilentlyContinue
                $phase = $null
                foreach ($line in $log) {
                    if ($line -match '\[apx-fps\] phase (\w+)') { $phase = $matches[1] }
                    if ($line -match 'apxfps: frames (\d+) seconds ([\d.]+) frame-ms p50 ([\d.]+) p99 ([\d.]+) max ([\d.]+) fps-p50 ([\d.]+) fps-p99 ([\d.]+) map (\d+)x(\d+)') {
                        $row = [ordered]@{ ts = (Get-Date -Format o); tag = $Tag; size = $Size; arm = $name; floor = $floor.n; phase = $phase; run = $r; env = $envs; args = $extra
                            frames = [int]$matches[1]; ms_p50 = [double]$matches[3]; ms_p99 = [double]$matches[4]; ms_max = [double]$matches[5]
                            fps_p50 = [double]$matches[6]; fps_p99 = [double]$matches[7]; map = "$($matches[8])x$($matches[9])" }
                        Add-Content (Join-Path $out 'runs.jsonl') ($row | ConvertTo-Json -Compress)
                        "{0,-14} {1,-6} {2,-16} fps p50 {3,6} p99 {4,6}  frame ms p99 {5,6} max {6,6}  map {7}" -f $name, $floor.n, $phase, $row.fps_p50, $row.fps_p99, $row.ms_p99, $row.ms_max, $row.map
                    }
                }
            }
        }
    }
} finally {
    if (Test-Path $lock) { Remove-Item -LiteralPath $lock }
    if (Test-Path $want) { Remove-Item -LiteralPath $want }
}
