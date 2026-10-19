# tools/apx/build_cache_bench.ps1 -- measure build times per compiler-cache mode.
#
# For each mode: cold build in worktree A with an empty cache, clean build in worktree B (same commit,
# different path) with that warm cache, then a one-file incremental build in A (src/files.cpp touched).
# Rows -> C:\Apocrypha\diagnostics\barony-build\runs.jsonl. Mode "vs" is the original Visual Studio build,
# "none" is Ninja without a cache (the control: B gains nothing from A).
#
# Usage: pwsh -NoProfile -File tools\apx\build_cache_bench.ps1 -A <worktree> -B <worktree> [-Modes vs,none,sccache,ccache]
param(
    [Parameter(Mandatory)][string]$A,
    [Parameter(Mandatory)][string]$B,
    [string[]]$Modes = @('vs', 'none', 'sccache', 'ccache'),
    [string]$CacheRoot = 'C:\Apocrypha\cache',
    [int]$Jobs = 0
)
$ErrorActionPreference = 'Stop'
$out = 'C:\Apocrypha\diagnostics\barony-build'
New-Item -ItemType Directory -Force $out | Out-Null

function BuildDir([string]$tree, [string]$mode) {
    if ($mode -eq 'vs') { Join-Path $tree 'build\vs2022' } else { Join-Path $tree "build\ninja-$mode" }
}
function Run([string]$mode, [string]$step, [string]$tree) {
    $commit = (& git -C $tree rev-parse --short HEAD).Trim()
    $os = Get-CimInstance Win32_OperatingSystem
    $t0 = Get-Date
    $log = Join-Path $out ("build_{0}_{1}.log" -f $mode, $step)
    & pwsh -NoProfile -File (Join-Path $tree 'tools\apx\build.ps1') -Cache $mode -CacheRoot $CacheRoot -Jobs $Jobs *> $log
    $ok = $LASTEXITCODE -eq 0
    $wall = [math]::Round(((Get-Date) - $t0).TotalSeconds, 1)
    $txt = Get-Content $log -Raw
    $hits = $null; $misses = $null
    if ($mode -eq 'sccache') {
        if ($txt -match 'Cache hits\s+(\d+)') { $hits = [int]$matches[1] }
        if ($txt -match 'Cache misses\s+(\d+)') { $misses = [int]$matches[1] }
    } elseif ($mode -eq 'ccache') {
        if ($txt -match 'Hits:\s+(\d+)') { $hits = [int]$matches[1] }
        if ($txt -match 'Misses:\s+(\d+)') { $misses = [int]$matches[1] }
    }
    $exe = Join-Path (BuildDir $tree $mode) $(if ($mode -eq 'vs') { 'Release\barony.exe' } else { 'barony.exe' })
    $row = [ordered]@{
        ts = $t0.ToString('yyyy-MM-ddTHH:mm:sszzz'); mode = $mode; step = $step; tree = (Split-Path $tree -Leaf)
        commit = $commit; ok = $ok; wall_s = $wall; hits = $hits; misses = $misses
        free_commit_gb = [math]::Round($os.FreeVirtualMemory / 1MB, 1); cpus = [Environment]::ProcessorCount
        exe_sha256 = if (Test-Path $exe) { (Get-FileHash -Algorithm SHA256 $exe).Hash.Substring(0, 16).ToLowerInvariant() } else { $null }
    }
    $json = $row | ConvertTo-Json -Compress
    Add-Content (Join-Path $out 'runs.jsonl') $json
    $json
    if (-not $ok) { throw "build failed: $mode $step (see $log)" }
}

$Modes = @($Modes | ForEach-Object { $_ -split ',' } | Where-Object { $_ })   # pwsh -File passes a list as one string
foreach ($mode in $Modes) {
    # empty cache for a true cold start
    if ($mode -eq 'sccache') {
        $sc = (Get-Command sccache -ErrorAction SilentlyContinue).Source
        if ($sc) { & $sc --stop-server 2>$null | Out-Null }
        if (Test-Path (Join-Path $CacheRoot 'sccache')) { Remove-Item -LiteralPath (Join-Path $CacheRoot 'sccache') -Recurse -Force }
    } elseif ($mode -eq 'ccache') {
        if (Test-Path (Join-Path $CacheRoot 'ccache')) { Remove-Item -LiteralPath (Join-Path $CacheRoot 'ccache') -Recurse -Force }
    }
    foreach ($tree in $A, $B) {
        $bd = BuildDir $tree $mode
        if (Test-Path $bd) { Remove-Item -LiteralPath $bd -Recurse -Force }
    }
    Run $mode 'cold_A' $A
    if ($mode -ne 'vs') { Run $mode 'warm_B' $B }   # vs has no cache: its B build would equal cold_A
    (Get-Item (Join-Path $A 'src\files.cpp')).LastWriteTime = Get-Date
    Run $mode 'incr_A' $A
}
