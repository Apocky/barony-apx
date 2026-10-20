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
if (Test-Path (Join-Path $bin 'steam_appid.txt')) { Copy-Item (Join-Path $bin 'steam_appid.txt') $stage }   # Steam build: Barony's app id, read from the working folder
Get-ChildItem $bin -Filter *.dll | Copy-Item -Destination $stage
New-Item -ItemType Directory -Force "$stage\data" | Out-Null
Copy-Item data\apx "$stage\data\apx" -Recurse
New-Item -ItemType Directory -Force "$stage\scripts" | Out-Null
Copy-Item scripts\apx "$stage\scripts\apx" -Recurse
New-Item -ItemType Directory -Force "$stage\apxgen" | Out-Null   # floor generator rules and kits (read from <exe dir>/apxgen)
Copy-Item src\apx\gen\grammar "$stage\apxgen\grammar" -Recurse
Copy-Item src\apx\gen\kits "$stage\apxgen\kits" -Recurse
Get-ChildItem tools\apx\release -File | Copy-Item -Destination $stage
Get-ChildItem LICENSE*.txt | Copy-Item -Destination $stage
Set-Content "$stage\VERSION.txt" "Apocky's Labyrinth $Version`r`nbuilt from $(git rev-parse HEAD) on $(Get-Date -Format 'yyyy-MM-dd HH:mm zzz')" -Encoding ascii

# Guard: nothing of Barony's own content may be inside the package.
$banned = Get-ChildItem $stage -Recurse -File | Where-Object { $_.Extension -in '.vox', '.png', '.ogg', '.wav', '.lmp', '.fsb', '.bank' }
if ($banned) { $banned | ForEach-Object { "BANNED: $($_.FullName)" }; throw 'package contains game assets' }

Copy-Item barony.ico $stage   # installer icon and shortcuts (our flaming anarchy icon, D61)

# The installer (Inno Setup 6): dist\ApockysLabyrinth-<Version>-Setup.exe
$iscc = @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe", "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { throw 'Inno Setup 6 (ISCC.exe) not found: winget install JRSoftware.InnoSetup' }
& $iscc /Q "/DAppVersion=$Version" "/DSrcDir=$stage" "/O$(Join-Path $root 'dist')" tools\apx\installer\ApockysLabyrinth.iss
if ($LASTEXITCODE -ne 0) { throw "installer build failed ($LASTEXITCODE)" }
$setup = Join-Path $root "dist\ApockysLabyrinth-$Version-Setup.exe"
"installer: $setup $([math]::Round((Get-Item $setup).Length / 1MB, 1)) MB"

$zip = Join-Path $root "dist\$name.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path "$stage\*" -DestinationPath $zip
$files = (Get-ChildItem $stage -Recurse -File).Count
"package: $zip $([math]::Round((Get-Item $zip).Length / 1MB, 1)) MB, $files files"
