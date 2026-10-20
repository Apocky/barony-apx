# tools/apx/fps_gate.ps1 -- merge-train fps gate: a candidate build against the apx-main build, same machine, same run.
#
# Runs tools\apx\fps_probe.ps1 (exclusive, windowed, never touches the mouse) for both exes, -Repeat times each,
# start map and floor 1, inventory closed and open. FAIL when, in any (floor, phase), the candidate's median fps p50
# is below -MinP50 (default 0.85) of the base's, or its median fps p99 below -MinP99 (default 0.80). Comparing two
# builds measured minutes apart cancels most of the machine's load; absolute numbers are printed for the record.
# Usage: pwsh -NoProfile -File tools\apx\fps_gate.ps1 -Candidate <exe> -Base <exe> [-Repeat 2]
param(
    [Parameter(Mandatory)][string]$Candidate,
    [Parameter(Mandatory)][string]$Base,
    [int]$Repeat = 2,
    [double]$MinP50 = 0.85,
    [double]$MinP99 = 0.80
)
$tag = "gate-$(Get-Date -Format yyyyMMdd-HHmmss)"
& pwsh -NoProfile -File (Join-Path $PSScriptRoot 'fps_probe.ps1') -Arms "candidate|$Candidate||;base|$Base||" -Tag $tag -Repeat $Repeat | Out-Null
$rows = Get-Content 'C:\Apocrypha\diagnostics\barony-fps\runs.jsonl' | ForEach-Object { $_ | ConvertFrom-Json } | Where-Object { $_.tag -eq $tag }
$median = { param($xs) $s = @($xs | Sort-Object); if ($s.Count -eq 0) { return $null }; $s[[int][math]::Floor(($s.Count - 1) / 2)] }
$fail = 0
foreach ($g in ($rows | Group-Object floor, phase)) {
    $c = @($g.Group | Where-Object arm -eq 'candidate'); $b = @($g.Group | Where-Object arm -eq 'base')
    $c50 = & $median ($c.fps_p50); $b50 = & $median ($b.fps_p50); $c99 = & $median ($c.fps_p99); $b99 = & $median ($b.fps_p99)
    if ($null -eq $c50 -or $null -eq $b50) { "{0,-26} MISSING rows" -f $g.Name; $fail++; continue }
    $ok = $c50 -ge $MinP50 * $b50 -and $c99 -ge $MinP99 * $b99
    if (-not $ok) { $fail++ }
    "{0,-26} {1}  p50 {2,6} vs base {3,6} ({4:P0})  p99 {5,6} vs base {6,6} ({7:P0})" -f $g.Name, $(if ($ok) { 'PASS' } else { 'FAIL' }), $c50, $b50, ($c50 / $b50), $c99, $b99, ($c99 / $b99)
}
"FPS GATE: $(if ($fail -eq 0) { 'PASS' } else { "FAIL ($fail)" }) (tag $tag, rows in C:\Apocrypha\diagnostics\barony-fps\runs.jsonl)"
if ($fail -gt 0) { exit 1 }
