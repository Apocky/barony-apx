# tools/apx/build.ps1 -- configure and build barony-apx on Windows with CMake + Visual Studio.
#
# Output: <BuildDir>\<Config>\barony.exe with its runtime DLLs beside it. Run it against the Steam
# data with -datadir="<Steam>\Barony" from any working directory; on Windows the game writes its
# log, config and saves to the working directory, so pick an empty one (tools\apx\parity_f0.py does).
#
# Usage (PowerShell 7):  pwsh -NoProfile -File tools\apx\build.ps1 [-Config Release] [-NoFmod] [-Editor] [-Cache <mode>]
#
# -Cache: vs      = Visual Studio generator, no compiler cache (the original build; output build\vs2022\<Config>)
#         none    = Ninja, no compiler cache (control; output build\ninja-none)
#         sccache = Ninja + sccache, cache shared by every worktree (output build\ninja-sccache)
#         ccache  = Ninja + ccache, same sharing (output build\ninja-ccache)
# Ninja modes load the VS 2022 Build Tools x64 environment (vcvars64), the same v143 compiler the VS build
# uses. The caches live in C:\Apocrypha\cache\<tool>; paths are rewritten relative to each worktree's root
# (SCCACHE_BASEDIRS / CCACHE_BASEDIR), so a second worktree reuses objects the first one compiled.

param(
    [string]$Config = 'Release',
    [string]$DepsRoot = (Join-Path (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')) 'barony-apx-deps'),
    [string]$FmodDir = 'C:\Program Files (x86)\FMOD SoundSystem\FMOD Studio API Windows',
    [string]$Generator = 'Visual Studio 17 2022',
    [string]$BuildDir,
    [switch]$NoFmod,
    [switch]$Editor,
    [int]$Jobs = 0,    # compiler processes (/MP or ninja -j); 0 = derive from free memory at build time
    [ValidateSet('vs', 'none', 'sccache', 'ccache')]
    [string]$Cache = 'vs',
    [string]$CacheRoot = 'C:\Apocrypha\cache',
    [string]$SteamworksDir,
    [string]$VcVars = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
function Step([string]$msg) { Write-Host "[build] $msg" }

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$ninja = $Cache -ne 'vs'
if (-not $BuildDir) {   # build/* is git-ignored upstream
    $BuildDir = if ($ninja) { Join-Path $repo ("build\ninja-$Cache" + $(if ($SteamworksDir) { '-steam' } else { '' })) } else { Join-Path $repo 'build\vs2022' }
}

# Tools installed by winget are on the user PATH, which a shell started before the install may lack.
function Find-Tool([string]$name) {
    $c = Get-Command $name -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    $hit = Get-ChildItem (Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Packages') -Recurse -Filter "$name.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($hit) { return $hit.FullName }
    throw "$name not found (winget install Mozilla.sccache / Ccache.Ccache / Ninja-build.Ninja)"
}

$launcherArgs = @()
$cacheTool = $null
if ($ninja) {
    if (-not (Test-Path $VcVars)) { throw "vcvars64.bat not found: $VcVars" }
    Step "loading the MSVC x64 environment from $VcVars"
    foreach ($line in (& cmd /c "`"$VcVars`" >nul && set")) {
        if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2]) }
    }
    $launcherArgs += "-DCMAKE_MAKE_PROGRAM=$(Find-Tool 'ninja')"
    $launcherArgs += "-DCMAKE_BUILD_TYPE=$Config"
    if ($Cache -eq 'sccache') {
        $cacheTool = Find-Tool 'sccache'
        $env:SCCACHE_DIR = Join-Path $CacheRoot 'sccache'
        $env:SCCACHE_BASEDIRS = $repo
        $env:SCCACHE_CACHE_SIZE = '20G'
        # the server keeps the environment it started with; restart it so it sees this worktree's base dir
        & $cacheTool --stop-server 2>$null | Out-Null
        & $cacheTool --start-server | Out-Null
        & $cacheTool --zero-stats | Out-Null
    } elseif ($Cache -eq 'ccache') {
        $cacheTool = Find-Tool 'ccache'
        $env:CCACHE_DIR = Join-Path $CacheRoot 'ccache'
        $env:CCACHE_BASEDIR = $repo
        $env:CCACHE_NOHASHDIR = 'true'
        $env:CCACHE_MAXSIZE = '20G'
        & $cacheTool --zero-stats | Out-Null
    }
    if ($cacheTool) {
        $launcherArgs += "-DCMAKE_CXX_COMPILER_LAUNCHER=$cacheTool"
        $launcherArgs += "-DCMAKE_C_COMPILER_LAUNCHER=$cacheTool"
    }
}

# Guard: with BARONY_DATADIR in the environment, CMakeLists.txt adds INSTALL rules that copy barony.exe,
# editor.exe and lang/en.txt INTO the data directory. Ours is the Steam install, whose barony.exe is
# Apocky's S.A.M build and must never be overwritten. Configure without it and never build INSTALL.
if (Test-Path Env:BARONY_DATADIR) {
    Step "removing BARONY_DATADIR ($env:BARONY_DATADIR) from this process before configuring"
    Remove-Item Env:BARONY_DATADIR
}

$prefix = Join-Path $DepsRoot 'prefix'
if (-not (Test-Path (Join-Path $prefix 'apx-deps-manifest.json'))) {
    & (Join-Path $PSScriptRoot 'fetch-deps.ps1') -DepsRoot $DepsRoot -Generator $Generator
}
$env:BARONY_WIN32_LIBRARIES = $prefix

$soundArgs = @('-DFMOD_ENABLED=OFF')
if (-not $NoFmod) {
    if (-not (Test-Path (Join-Path $FmodDir 'api\core\inc\fmod.hpp'))) {
        throw "FMOD Core API not found under $FmodDir (install it, or pass -NoFmod to build without sound)"
    }
    $env:FMOD_DIR = $FmodDir
    $soundArgs = @('-DFMOD_ENABLED=ON')
}

# Steamworks (DLC ownership and the rest from Steam): -SteamworksDir <SDK root with public\ and redistributable_bin\>
$steamArgs = @('-DSTEAMWORKS_ENABLED=0')
if ($SteamworksDir) {
    $steamArgs = @('-DSTEAMWORKS_ENABLED=1', "-DSTEAMWORKS_INCLUDE_DIR=$(Join-Path $SteamworksDir 'public')", "-DSTEAMWORKS_LIBRARIES=$(Join-Path $SteamworksDir 'redistributable_bin\win64\steam_api64.lib')", "-DSTEAMWORKS_LIBRARY=$(Join-Path $SteamworksDir 'redistributable_bin\win64\steam_api64.lib')")
    # Barony's Steam build also links nativefiledialog (Workshop upload dialogs): built into <deps>\nfd (nfd.lib + include\nfd.h)
    $nfd = Join-Path $DepsRoot 'nfd'
    if (-not (Test-Path (Join-Path $nfd 'nfd.lib'))) { throw "nativefiledialog not built under $nfd (see tools\apx\README.md, Steam build)" }
    $steamArgs += @("-DNFD_INCLUDE_DIR=$(Join-Path $nfd 'include')", "-DNFD_INCLUDE_DIRS=$(Join-Path $nfd 'include')", "-DNFD_LIBRARY=$(Join-Path $nfd 'nfd.lib')", "-DNFD_LIBRARIES=$(Join-Path $nfd 'nfd.lib')")
}
if ($ninja) { $Generator = 'Ninja'; $platformArgs = @() } else { $platformArgs = @('-A', 'x64') }
Step "configure $BuildDir ($Generator, cache $Cache, x64, $($soundArgs -join ' '))"
& cmake -S $repo -B $BuildDir -G $Generator @platformArgs @launcherArgs `
    @steamArgs -DEOS_ENABLED=0 -DPLAYFAB_ENABLED=0 -DCURL_ENABLED=0 -DOPUS_ENABLED=0 `
    -DTHEORAPLAYER_ENABLED=0 -DOPENAL_ENABLED=OFF @soundArgs
if ($LASTEXITCODE -ne 0) { throw 'configure failed' }

$targets = @('barony')
if ($Editor) { $targets += 'editor' }
foreach ($t in $targets) {
    # CMakeLists.txt passes /MP (MultiProcessorCompilation): one cl.exe per core, and Barony's largest
    # files take gigabytes each. A cold build on a machine whose commit charge is nearly used up (the
    # local model server alone holds ~19 GB) died with C1060 "compiler is out of heap space". Cap the
    # compiler processes by the free commit memory, about 2 GB each, and say what was chosen.
    $jobsNow = $Jobs
    $os = Get-CimInstance Win32_OperatingSystem
    $freeGB = [math]::Round($os.FreeVirtualMemory / 1MB, 1)
    if ($jobsNow -le 0) {
        $jobsNow = [math]::Max(1, [math]::Min([Environment]::ProcessorCount, [math]::Floor(($freeGB - 1) / 2)))
    }
    Step "build $t ($Config), $jobsNow compiler processes (free commit $freeGB GB)"
    $t0 = Get-Date
    if ($ninja) {
        & cmake --build $BuildDir --target $t -- -j $jobsNow
    } else {
        & cmake --build $BuildDir --config $Config --target $t -- /m /nologo /verbosity:minimal "/p:CL_MPCount=$jobsNow"
    }
    if ($LASTEXITCODE -ne 0) { throw "build failed: $t" }
    Step ("built {0} in {1:n0} s" -f $t, ((Get-Date) - $t0).TotalSeconds)
}

# Runtime DLLs beside the exe (Windows loads DLLs from the exe's folder first, so the Steam install's
# own DLLs, including its FMOD 2.02 fmod.dll, are never picked up by this build).
$out = if ($ninja) { $BuildDir } else { Join-Path $BuildDir $Config }   # Ninja is single-config
$cacheStats = $null
if ($Cache -eq 'sccache') { $cacheStats = (& $cacheTool --show-stats) -join "`n" }
elseif ($Cache -eq 'ccache') { $cacheStats = (& $cacheTool --show-stats) -join "`n" }
if ($cacheStats) { Step "$Cache stats:`n$cacheStats" }
Copy-Item (Join-Path $prefix 'bin\*.dll') $out -Force
if (-not $NoFmod) { Copy-Item (Join-Path $FmodDir 'api\core\lib\x64\fmod.dll') $out -Force }
if ($SteamworksDir) {   # the Steam API runtime and Barony's app id, so Steam reports the player's DLC
    Copy-Item (Join-Path $SteamworksDir 'redistributable_bin\win64\steam_api64.dll') $out -Force
    Set-Content (Join-Path $out 'steam_appid.txt') '371970' -Encoding ascii
}

$exe = Join-Path $out 'barony.exe'
$info = [ordered]@{
    built     = (Get-Date -Format 'yyyy-MM-ddTHH:mm:sszzz')
    commit    = (& git -C $repo rev-parse HEAD).Trim()
    dirty     = [bool](& git -C $repo status --porcelain --untracked-files=no)
    config    = $Config
    generator = $Generator
    cache     = $Cache
    fmod      = if ($NoFmod) { $null } else { $FmodDir }
    exe       = $exe
    exe_bytes = (Get-Item $exe).Length
    exe_sha256 = (Get-FileHash -Algorithm SHA256 $exe).Hash.ToLowerInvariant()
}
$info | ConvertTo-Json | Set-Content -Encoding ascii (Join-Path $out 'apx-build-info.json')
Step "done: $exe"
