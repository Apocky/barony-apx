# tools/apx/vis_lightprobe.ps1 -- BVIS1: how far monsters notice the player, per tile, curve off vs on.
# Barony's rule (actmonster.cpp): a monster notices a player when distance <= its sight range and distance <=
# the player's light after reductions, which starts from Entity::entityLight() = entityLightAt(tile) in world
# units (16 per tile). -apxlightprobe writes entityLightAt for every open tile (the floor's lights only).
# Headless, APX_HEADLESS_BATCH=1, one D70 slot per launch. Output C:\Apocrypha\diagnostics\barony-vis\probe\<tag>\.
# Usage: pwsh -NoProfile -File tools\apx\vis_lightprobe.ps1 -Exe <barony.exe> [-Arms "off:APX_VIS_FALLOFF=0;on:"]
param(
    [Parameter(Mandatory)][string]$Exe,
    [string]$Arms = 'off:APX_VIS_FALLOFF=0;on:',
    [string]$Floors = 'gen1234567:-apxmapseed=1234567;gen1001:-apxmapseed=1001;vanilla:-apxmapseed=1234567|APXGEN=0'
)
$root = 'C:\Apocrypha\diagnostics\barony-vis\probe'
$slots = 'C:\Apocrypha\locks\barony-slots'; $excl = 'C:\Apocrypha\locks\barony'; $want = 'C:\Apocrypha\locks\barony-want'
function Take-Slot {
    while ($true) {
        $busy = (Get-ChildItem $excl -Filter *.LOCK -EA SilentlyContinue) -or (Get-ChildItem $want -Filter *.WANT -EA SilentlyContinue)
        if (-not $busy) { foreach ($n in 1..3) { $f = Join-Path $slots "slot$n.LOCK"; try { $fs = [IO.File]::Open($f, 'CreateNew'); $fs.Close(); Set-Content $f "vis-probe $(Get-Date -Format o)"; return $f } catch {} } }
        Start-Sleep 10
    }
}
foreach ($fl in ($Floors -split ';')) {
    $fname, $fspec = $fl -split ':', 2; $fargs, $fenv = $fspec -split '\|', 2
    foreach ($arm in ($Arms -split ';')) {
        $aname, $aenv = $arm -split ':', 2
        $tag = "$fname-$aname"; $dir = Join-Path $root $tag
        if (Test-Path $dir) { Remove-Item -LiteralPath $dir -Recurse -Force }
        New-Item -ItemType Directory -Force $dir | Out-Null
        $envs = @("APX_HEADLESS_BATCH=1", "APX_DIAG_DIR=$dir") + (@($fenv, $aenv) | Where-Object { $_ } | ForEach-Object { $_ -split ',' })
        $slot = Take-Slot
        try {
            $env:BARONY_DATADIR = 'C:\Program Files (x86)\Steam\steamapps\common\Barony'
            foreach ($kv in $envs) { $k, $v = $kv -split '=', 2; [Environment]::SetEnvironmentVariable($k, $v) }
            $p = Start-Process -FilePath $Exe -WorkingDirectory $dir -PassThru -ArgumentList "-quickstart=barbarian -apxfloor=1 $fargs -apxheadless -apxlightprobe"
            if (-not $p.WaitForExit(120000)) { $p.Kill() }
        } finally {
            Remove-Item $slot -Force -EA SilentlyContinue
            foreach ($kv in $envs) { [Environment]::SetEnvironmentVariable(($kv -split '=', 2)[0], $null) }
        }
        $line = (Select-String "$dir\log.txt" -Pattern '\[apx-vis\] lightprobe.*' -EA SilentlyContinue | Select-Object -Last 1).Matches.Value
        "$tag : $line"
    }
}
