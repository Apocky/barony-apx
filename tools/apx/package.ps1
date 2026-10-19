param([Parameter(Mandatory)][string]$Version, [string]$Exe = 'build\ninja-ccache\barony.exe')
# Builds dist\ApockysLabyrinth-<Version>-win64.zip: our exe and runtime DLLs, our data (data/apx),
# our Lua scripts (scripts/apx), the launcher, the install guide and every licence file.
# Barony's art, sounds and music are never packaged (players use their own Steam install).
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
Set-Location $root
$exePath = Resolve-Path $Exe
$bin = Split-Path $exePath
$name = "ApockysLabyrinth-$Version-win64"
$stage = Join-Path $root "dist\$name"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force $stage | Out-Null

Copy-Item $exePath $stage
Get-ChildItem $bin -Filter *.dll | Copy-Item -Destination $stage
New-Item -ItemType Directory -Force "$stage\data" | Out-Null
Copy-Item data\apx "$stage\data\apx" -Recurse
New-Item -ItemType Directory -Force "$stage\scripts" | Out-Null
Copy-Item scripts\apx "$stage\scripts\apx" -Recurse
Get-ChildItem tools\apx\release -File | Copy-Item -Destination $stage
Get-ChildItem LICENSE*.txt | Copy-Item -Destination $stage
Set-Content "$stage\VERSION.txt" "Apocky's Labyrinth $Version`r`nbuilt from $(git rev-parse HEAD) on $(Get-Date -Format 'yyyy-MM-dd HH:mm zzz')" -Encoding ascii

# Guard: nothing of Barony's own content may be inside the package.
$banned = Get-ChildItem $stage -Recurse -File | Where-Object { $_.Extension -in '.vox', '.png', '.ogg', '.wav', '.lmp', '.fsb', '.bank' }
if ($banned) { $banned | ForEach-Object { "BANNED: $($_.FullName)" }; throw 'package contains game assets' }

$zip = Join-Path $root "dist\$name.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path "$stage\*" -DestinationPath $zip
$files = (Get-ChildItem $stage -Recurse -File).Count
"package: $zip $([math]::Round((Get-Item $zip).Length / 1MB, 1)) MB, $files files"
