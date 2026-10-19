# tools/apx/build.ps1 -- configure and build barony-apx on Windows with CMake + Visual Studio.
#
# Output: <BuildDir>\<Config>\barony.exe with its runtime DLLs beside it. Run it against the Steam
# data with -datadir="<Steam>\Barony" from any working directory; on Windows the game writes its
# log, config and saves to the working directory, so pick an empty one (tools\apx\parity_f0.py does).
#
# Usage (PowerShell 7):  pwsh -NoProfile -File tools\apx\build.ps1 [-Config Release] [-NoFmod] [-Editor]

param(
    [string]$Config = 'Release',
    [string]$DepsRoot = (Join-Path (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')) 'barony-apx-deps'),
    [string]$FmodDir = 'C:\Program Files (x86)\FMOD SoundSystem\FMOD Studio API Windows',
    [string]$Generator = 'Visual Studio 17 2022',
    [string]$BuildDir,
    [switch]$NoFmod,
    [switch]$Editor,
    [int]$Jobs = 0     # compiler processes for /MP; 0 = derive from free memory at build time
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
function Step([string]$msg) { Write-Host "[build] $msg" }

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $BuildDir) { $BuildDir = Join-Path $repo 'build\vs2022' }   # build/* is git-ignored upstream

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

Step "configure $BuildDir ($Generator, x64, $($soundArgs -join ' '))"
& cmake -S $repo -B $BuildDir -G $Generator -A x64 `
    -DSTEAMWORKS_ENABLED=0 -DEOS_ENABLED=0 -DPLAYFAB_ENABLED=0 -DCURL_ENABLED=0 -DOPUS_ENABLED=0 `
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
    & cmake --build $BuildDir --config $Config --target $t -- /m /nologo /verbosity:minimal "/p:CL_MPCount=$jobsNow"
    if ($LASTEXITCODE -ne 0) { throw "build failed: $t" }
    Step ("built {0} in {1:n0} s" -f $t, ((Get-Date) - $t0).TotalSeconds)
}

# Runtime DLLs beside the exe (Windows loads DLLs from the exe's folder first, so the Steam install's
# own DLLs, including its FMOD 2.02 fmod.dll, are never picked up by this build).
$out = Join-Path $BuildDir $Config
Copy-Item (Join-Path $prefix 'bin\*.dll') $out -Force
if (-not $NoFmod) { Copy-Item (Join-Path $FmodDir 'api\core\lib\x64\fmod.dll') $out -Force }

$exe = Join-Path $out 'barony.exe'
$info = [ordered]@{
    built     = (Get-Date -Format 'yyyy-MM-ddTHH:mm:sszzz')
    commit    = (& git -C $repo rev-parse HEAD).Trim()
    dirty     = [bool](& git -C $repo status --porcelain --untracked-files=no)
    config    = $Config
    generator = $Generator
    fmod      = if ($NoFmod) { $null } else { $FmodDir }
    exe       = $exe
    exe_bytes = (Get-Item $exe).Length
    exe_sha256 = (Get-FileHash -Algorithm SHA256 $exe).Hash.ToLowerInvariant()
}
$info | ConvertTo-Json | Set-Content -Encoding ascii (Join-Path $out 'apx-build-info.json')
Step "done: $exe"
