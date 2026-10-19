# tools/apx/vis_fps.ps1 -- BVIS1 fps gate: gate_f2.py w1 (fps p50/p99 in the largest room, uncapped) run EXCLUSIVE
# per D70 (timings are skewed by other games): posts barony-want\vis-fps.WANT, waits until no slot, lock or Barony
# process remains, then gate_f2 takes C:\Apocrypha\locks\barony itself; the WANT is removed at the end.
# Usage: pwsh -NoProfile -File tools\apx\vis_fps.ps1 -Exe <barony.exe> -Tag <name> [-Seeds 1001,1002,1003] [-Sizes 128,vanilla] [-Env "K=V"]
param(
    [Parameter(Mandatory)][string]$Exe,
    [Parameter(Mandatory)][string]$Tag,
    [string]$Seeds = '1001,1002',
    [string]$Sizes = '128',
    [string]$Env = ''
)
$want = 'C:\Apocrypha\locks\barony-want\vis-fps.WANT'
New-Item -ItemType Directory -Force (Split-Path $want) | Out-Null
Set-Content $want "vis lane fps gate $Tag $(Get-Date -Format o)"
$out = "C:\Apocrypha\diagnostics\barony-vis\fps-$Tag"
New-Item -ItemType Directory -Force $out | Out-Null
try {
    while ((Get-Process -Name 'barony*' -EA SilentlyContinue) -or
           (Get-ChildItem 'C:\Apocrypha\locks\barony-slots' -Filter *.LOCK -EA SilentlyContinue) -or
           (Get-ChildItem 'C:\Apocrypha\locks\barony' -Filter *.LOCK -EA SilentlyContinue)) { Start-Sleep 5 }
    # gate_f2 itself waits while any *.WANT exists, ours included: drop ours now that the slots have drained;
    # gate_f2 then takes the exclusive lock C:\Apocrypha\locks\barony\f2-integrate.LOCK around each launch
    if (Test-Path $want) { Remove-Item -LiteralPath $want }
    foreach ($kv in ($Env -split ',' | Where-Object { $_ })) { $k, $v = $kv -split '=', 2; [Environment]::SetEnvironmentVariable($k, $v) }
    & python (Join-Path $PSScriptRoot 'gate_f2.py') w1 --exe $Exe --seeds $Seeds --sizes $Sizes --out $out --lock-dir C:\Apocrypha\locks\barony 2>&1 |
        Tee-Object -FilePath (Join-Path $out 'gate_f2.log') | Where-Object { $_ -match '^\[f2\]' }
} finally {
    if (Test-Path $want) { Remove-Item -LiteralPath $want }
    foreach ($kv in ($Env -split ',' | Where-Object { $_ })) { [Environment]::SetEnvironmentVariable(($kv -split '=', 2)[0], $null) }
}
# summary from the rows
Get-Content (Join-Path $out 'f2.jsonl') -EA SilentlyContinue | ForEach-Object { $r = $_ | ConvertFrom-Json
    if ($r.fps) { "{0,-8} seed {1}: fps p50 {2} p99 {3} (frame ms p99 {4})" -f $r.size, $r.seed_asked, $r.fps.fps_p50, $r.fps.fps_p99, $r.fps.ms_p99 } }
