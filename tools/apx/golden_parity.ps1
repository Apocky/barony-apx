param([string]$Exe, [string]$Tag = 'merge')
# Golden parity: our build vs the F0 floor-1 lines recorded from Steam vanilla (tools\apx\README.md).
$golden = @{ '1234567' = '13 rooms, 12 monsters, 21 gold, 18 items, 4 decorations';
             '7654321' = '14 rooms, 13 monsters, 20 gold, 19 items, 2 decorations' }
$slots = 'C:\Apocrypha\locks\barony-slots'; $excl = 'C:\Apocrypha\locks\barony'; $want = 'C:\Apocrypha\locks\barony-want'
$mine = $null
while (-not $mine) {
  $busy = (Get-ChildItem $excl -Filter *.LOCK -EA SilentlyContinue) -or (Get-ChildItem $want -Filter *.WANT -EA SilentlyContinue)
  if (-not $busy) { foreach ($n in 1..3) { $f = Join-Path $slots "slot$n.LOCK"; try { $fs = [IO.File]::Open($f, 'CreateNew'); $fs.Close(); Set-Content $f "$Tag $(Get-Date -Format o)"; $mine = $f; break } catch {} } }
  if (-not $mine) { Start-Sleep 20 }
}
"slot $mine $(Get-Date -Format HH:mm:ss)"
$pass = 0; $fail = 0
try {
  $env:BARONY_DATADIR = 'C:\Program Files (x86)\Steam\steamapps\common\Barony'
  foreach ($seed in @('1234567', '7654321', '1234567')) {
    $dir = "C:\Apocrypha\runs\$Tag-$seed-$(Get-Random)"; New-Item -ItemType Directory -Force $dir | Out-Null
    $p = Start-Process -FilePath $Exe -WorkingDirectory $dir -ArgumentList "-windowed -size=640x400 -quickstart=barbarian -apxfloor=1 -apxmapseed=$seed" -PassThru
    $line = $null; $deadline = (Get-Date).AddSeconds(90)
    while ((Get-Date) -lt $deadline -and -not $line) { Start-Sleep 2; $l = Select-String -Path "$dir\log.txt" -Pattern 'successfully generated a dungeon with (.*)\.' -EA SilentlyContinue | Select-Object -First 1; if ($l) { $line = $l.Matches[0].Groups[1].Value } }
    if (-not $p.HasExited) { $p.Kill() }
    $ok = ($line -eq $golden[$seed]); if ($ok) { $pass++ } else { $fail++ }
    "seed $seed : got '$line' expected '$($golden[$seed])' -> $(if ($ok) {'PASS'} else {'FAIL'})"
  }
} finally { Remove-Item $mine -Force -EA SilentlyContinue }
"GOLDEN PARITY: $pass pass, $fail fail"
if ($fail -gt 0) { exit 1 }
