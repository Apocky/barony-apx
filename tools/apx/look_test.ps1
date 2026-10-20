# tools/apx/look_test.ps1 -- BX1 LOOK network tests: a headless host + client on this PC (APX_LOOK_TEST in visshots.cpp).
#   live : the host builds a blade after the client joined; the client must receive it (LOOK announce + relay).
#   late : the host builds without announcing (APX_LOOK_NOANNOUNCE=1: the state of a later joiner) and hands the client the sword; the client must ask for the look (LKRQ)
#          and receive it. Control (-Control): APX_LOOK_NOREQ=1 on the client, which must then NOT receive it.
# Holds the exclusive Barony lock (D70). Logs: C:\Apocrypha\diagnostics\barony-vox\looktest\<mode>-<role>-<time>\
param([ValidateSet('live','late')][string]$Mode = 'late', [switch]$Control, [string]$Exe = "$PSScriptRoot\..\..\build\ninja-ccache\barony.exe")
$ErrorActionPreference = 'Stop'
$root = 'C:\Apocrypha\diagnostics\barony-vox\looktest'
$want = 'C:\Apocrypha\locks\barony-want\vox-looktest.WANT'
$lock = 'C:\Apocrypha\locks\barony\vox-looktest.LOCK'
$roles = if ($Mode -eq 'live') { @('host', 'client') } else { @('latehost', 'lateclient') }
Set-Content $want (Get-Date -Format o)
try {
	while ((Get-Process barony* -ea 0) -or (Get-ChildItem C:\Apocrypha\locks\barony-slots -Filter *.LOCK -ea 0) -or (Get-ChildItem C:\Apocrypha\locks\barony -Filter *.LOCK -ea 0)) { Start-Sleep 5 }
	Set-Content $lock (Get-Date -Format o); Remove-Item -LiteralPath $want
	$env:BARONY_DATADIR = 'C:\Program Files (x86)\Steam\steamapps\common\Barony'
	$stamp = Get-Date -Format HHmmss
	$tag = if ($Control) { "$Mode-control" } else { $Mode }
	$d = @{}; foreach ($r in $roles) { $d[$r] = Join-Path $root "$tag-$r-$stamp"; New-Item -ItemType Directory -Force $d[$r] | Out-Null }
	if ($Mode -eq 'late') { $env:APX_LOOK_NOANNOUNCE = '1' }
	$env:APX_LOOK_TEST = $roles[0]; $h = Start-Process -FilePath $Exe -WorkingDirectory $d[$roles[0]] -PassThru -ArgumentList '-apxhost -apxheadless'
	Start-Sleep 8
	Remove-Item Env:APX_LOOK_NOANNOUNCE -ea 0
	$env:APX_LOOK_TEST = $roles[1]; if ($Control) { $env:APX_LOOK_NOREQ = '1' }
	$c = Start-Process -FilePath $Exe -WorkingDirectory $d[$roles[1]] -PassThru -ArgumentList '-apxjoin=127.0.0.1 -apxheadless'
	Remove-Item Env:APX_LOOK_TEST; Remove-Item Env:APX_LOOK_NOREQ -ea 0
	$null = $h.WaitForExit(240000); $null = $c.WaitForExit(60000); foreach ($p in $h, $c) { if (-not $p.HasExited) { $p.Kill() } }
	foreach ($r in $roles) {
		"--- $tag $r ($($d[$r]))"
		Get-ChildItem $d[$r] -Recurse -Filter *.txt | ForEach-Object { Select-String -LiteralPath $_.FullName -Pattern '\[apx-vox\]' | ForEach-Object Line }
	}
}
finally { Remove-Item -LiteralPath $lock, $want -ea 0 }
