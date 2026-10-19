# tools/apx/fetch-deps.ps1 -- fetch, verify and build Barony's Windows dependencies.
#
# Downloads only libraries (code, headers, import libs, DLLs) from their official sources,
# checks each archive against a pinned SHA256, and assembles one prefix folder:
#   <DepsRoot>\prefix\{include,lib,bin}
# which build.ps1 hands to CMake as BARONY_WIN32_LIBRARIES (see INSTALL.md).
# No Barony game assets are fetched or touched here.
#
# Usage (PowerShell 7):  pwsh -NoProfile -File tools\apx\fetch-deps.ps1 [-DepsRoot <dir>] [-Clean]

param(
    [string]$DepsRoot = (Join-Path (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')) 'barony-apx-deps'),
    [string]$Generator = 'Visual Studio 17 2022',
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# SDL family versions match the DLLs shipped with Steam Barony v5.0.2 (SDL2 2.0.22, SDL2_image 2.0.5,
# SDL2_net 2.0.1, SDL2_ttf 2.0.15, GLEW 2.1.0). zlib and libpng are current releases (security fixes;
# image decoding is lossless, so the version does not change what the game sees).
$Archives = @(
    @{ Name = 'SDL2-devel-2.0.22-VC.zip'; Sha256 = '32adc96d8b25e5671189f1f38a4fc7deb105fbb1b3ed78ffcb23f5b8f36b3922'
       Url = 'https://github.com/libsdl-org/SDL/releases/download/release-2.0.22/SDL2-devel-2.0.22-VC.zip' }
    @{ Name = 'SDL2_image-devel-2.0.5-VC.zip'; Sha256 = 'a180f9b75c4d3fbafe02af42c42463cc7bc488e763cfd1ec2ffb75678b4387ac'
       Url = 'https://www.libsdl.org/projects/SDL_image/release/SDL2_image-devel-2.0.5-VC.zip' }
    @{ Name = 'SDL2_net-devel-2.0.1-VC.zip'; Sha256 = 'c1e423f2068adc6ff1070fa3d6a7886700200538b78fd5adc36903a5311a243e'
       Url = 'https://www.libsdl.org/projects/SDL_net/release/SDL2_net-devel-2.0.1-VC.zip' }
    @{ Name = 'SDL2_ttf-devel-2.0.15-VC.zip'; Sha256 = 'aab0d81f1aa6fe654be412efc85829f2b188165dca6c90eb4b12b673f93e054b'
       Url = 'https://www.libsdl.org/projects/SDL_ttf/release/SDL2_ttf-devel-2.0.15-VC.zip' }
    @{ Name = 'physfs-3.0.1.tar.bz2'; Sha256 = 'b77b9f853168d9636a44f75fca372b363106f52d789d18a2f776397bf117f2f1'
       Url = 'https://icculus.org/physfs/downloads/physfs-3.0.1.tar.bz2' }
    @{ Name = 'zlib-1.3.2.tar.gz'; Sha256 = 'bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16'
       Url = 'https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.gz' }
    @{ Name = 'libpng-1.6.59.tar.gz'; Sha256 = '2540302a1844ad2b2b501977abecfa850f265f97b78f065a712ab4074a89f5b5'
       Url = 'https://github.com/pnggroup/libpng/archive/refs/tags/v1.6.59.tar.gz' }
    @{ Name = 'glew-2.1.0-win32.zip'; Sha256 = '80cfc88fd295426b49001a9dc521da793f8547ac10aebfc8bdc91ddc06c5566c'
       Url = 'https://github.com/nigels-com/glew/releases/download/glew-2.1.0/glew-2.1.0-win32.zip' }
    @{ Name = 'rapidjson-24b5e7a8.zip'; Sha256 = 'df07f5ddfebbc2940181039f6c939ec2764a7303ef79b17958d9792a364306bb'
       Url = 'https://github.com/Tencent/rapidjson/archive/24b5e7a8b27f42fa16b96fc70aade9106cf7102f.zip' }
    @{ Name = 'dirent-1.26.zip'; Sha256 = '6ffcc318f00be192acb611c58aa58ee0cfd96776010680d7f38cf24f3dd8baf9'
       Url = 'https://github.com/tronkko/dirent/archive/refs/tags/1.26.zip' }
)

$Downloads = Join-Path $DepsRoot 'downloads'
$SrcRoot = Join-Path $DepsRoot 'src'
$BuildRoot = Join-Path $DepsRoot 'build'
$Prefix = Join-Path $DepsRoot 'prefix'
# bsdtar ships with Windows and reads zip, tar.gz and tar.bz2 (Git's GNU tar cannot read zip).
$Tar = Join-Path $env:SystemRoot 'System32\tar.exe'

function Step([string]$msg) { Write-Host "[fetch-deps] $msg" }

if ($Clean) {
    foreach ($d in @($SrcRoot, $BuildRoot, $Prefix)) {
        if (Test-Path $d) { Remove-Item -Recurse -Force $d }
    }
}
foreach ($d in @($Downloads, $SrcRoot, $BuildRoot, $Prefix, "$Prefix\include", "$Prefix\lib", "$Prefix\bin")) {
    New-Item -ItemType Directory -Force -Path $d | Out-Null
}

# 1. Download and verify.
foreach ($a in $Archives) {
    $file = Join-Path $Downloads $a.Name
    if (-not (Test-Path $file)) {
        Step "download $($a.Url)"
        & curl.exe -sSL --fail -o $file $a.Url
        if ($LASTEXITCODE -ne 0) { throw "download failed: $($a.Url)" }
    }
    $hash = (Get-FileHash -Algorithm SHA256 $file).Hash.ToLowerInvariant()
    if ($hash -ne $a.Sha256) {
        throw "SHA256 mismatch for $($a.Name): got $hash, pinned $($a.Sha256). Delete the file to re-download, or re-pin after checking the source."
    }
    Step "verified $($a.Name)"
}

# 2. Extract each archive into its own fresh folder.
function Expand-Dep([string]$archiveName) {
    $dest = Join-Path $SrcRoot ([IO.Path]::GetFileNameWithoutExtension($archiveName) -replace '\.tar$', '')
    if (-not (Test-Path $dest)) {
        New-Item -ItemType Directory -Force -Path $dest | Out-Null
        & $Tar -xf (Join-Path $Downloads $archiveName) -C $dest
        if ($LASTEXITCODE -ne 0) { throw "extract failed: $archiveName" }
    }
    # every archive has exactly one top-level folder
    return (Get-ChildItem -Directory $dest | Select-Object -First 1).FullName
}

function Copy-Into([string]$pattern, [string]$destDir) {
    # Get-Item (not Get-ChildItem): a folder pattern such as include\GL must copy the folder itself
    $items = @(Get-Item -Path $pattern -ErrorAction SilentlyContinue)
    if ($items.Count -eq 0) { throw "nothing matched $pattern" }
    New-Item -ItemType Directory -Force -Path $destDir | Out-Null
    $items | Copy-Item -Destination $destDir -Recurse -Force
}

# 3. Prebuilt SDL family: headers flat in include (Barony includes "SDL.h"), import libs in lib, DLLs in bin.
New-Item -ItemType Directory -Force -Path "$Prefix\licenses" | Out-Null
foreach ($n in @('SDL2-devel-2.0.22-VC.zip', 'SDL2_image-devel-2.0.5-VC.zip', 'SDL2_net-devel-2.0.1-VC.zip', 'SDL2_ttf-devel-2.0.15-VC.zip')) {
    $root = Expand-Dep $n
    Copy-Into "$root\include\*.h" "$Prefix\include"
    Copy-Into "$root\lib\x64\*.lib" "$Prefix\lib"
    Copy-Into "$root\lib\x64\*.dll" "$Prefix\bin"
    # licences of the bundled image/font codecs sit next to the DLLs; the package's own licence at its root
    Get-ChildItem "$root\lib\x64\LICENSE*.txt" -ErrorAction SilentlyContinue | Copy-Item -Destination "$Prefix\licenses" -Force
    $pkg = (Split-Path -Leaf $root) -replace '-[0-9.]+$', ''
    Get-ChildItem "$root\COPYING.txt", "$root\LICENSE.txt" -ErrorAction SilentlyContinue |
        Select-Object -First 1 | Copy-Item -Destination "$Prefix\licenses\LICENSE.$pkg.txt" -Force
    Step "assembled $n"
}

# GLEW (Barony's Windows build includes GL/glew.h and calls glewInit).
$glew = Expand-Dep 'glew-2.1.0-win32.zip'
Copy-Into "$glew\include\GL" "$Prefix\include"
Copy-Into "$glew\lib\Release\x64\glew32.lib" "$Prefix\lib"
Copy-Into "$glew\bin\Release\x64\glew32.dll" "$Prefix\bin"
Copy-Item "$glew\LICENSE.txt" "$Prefix\licenses\LICENSE.glew.txt" -Force
Step 'assembled glew 2.1.0'

# Header-only: dirent.h (POSIX port for MSVC) and RapidJSON.
$dirent = Expand-Dep 'dirent-1.26.zip'
Copy-Into "$dirent\include\dirent.h" "$Prefix\include"
Copy-Item "$dirent\LICENSE" "$Prefix\licenses\LICENSE.dirent.txt" -Force
$rj = Expand-Dep 'rapidjson-24b5e7a8.zip'
Copy-Into "$rj\include\rapidjson" "$Prefix\include"
Copy-Item "$rj\license.txt" "$Prefix\licenses\LICENSE.rapidjson.txt" -Force
Step 'assembled dirent 1.26 and rapidjson 24b5e7a8'

# 4. Build zlib, libpng and PhysFS from source with the same toolset Barony uses.
function Build-CMakeDep([string]$name, [string]$srcDir, [string[]]$extra) {
    $bdir = Join-Path $BuildRoot $name
    Step "configure $name"
    & cmake -S $srcDir -B $bdir -G $Generator -A x64 "-DCMAKE_INSTALL_PREFIX=$Prefix" "-DCMAKE_PREFIX_PATH=$Prefix" @extra
    if ($LASTEXITCODE -ne 0) { throw "configure failed: $name" }
    Step "build $name"
    & cmake --build $bdir --config Release --target INSTALL -- /m /nologo /verbosity:minimal
    if ($LASTEXITCODE -ne 0) { throw "build failed: $name" }
}

$zlib = Expand-Dep 'zlib-1.3.2.tar.gz'
Build-CMakeDep 'zlib' $zlib @('-DZLIB_BUILD_TESTING=OFF', '-DZLIB_BUILD_STATIC=OFF', '-DZLIB_BUILD_SHARED=ON')
Copy-Item "$zlib\LICENSE" "$Prefix\licenses\LICENSE.zlib.txt" -Force

$png = Expand-Dep 'libpng-1.6.59.tar.gz'
Build-CMakeDep 'libpng' $png @('-DPNG_SHARED=ON', '-DPNG_STATIC=OFF', '-DPNG_TESTS=OFF', '-DPNG_TOOLS=OFF', '-DPNG_FRAMEWORK=OFF')
Copy-Item "$png\LICENSE" "$Prefix\licenses\LICENSE.libpng.txt" -Force

# PhysFS 3.0.1 declares cmake_minimum_required(2.8.4); CMake 4 needs the policy floor raised to configure it.
$physfs = Expand-Dep 'physfs-3.0.1.tar.bz2'
Build-CMakeDep 'physfs' $physfs @('-DCMAKE_POLICY_VERSION_MINIMUM=3.5', '-DPHYSFS_BUILD_STATIC=OFF', '-DPHYSFS_BUILD_SHARED=ON', '-DPHYSFS_BUILD_TEST=OFF')
Copy-Item "$physfs\LICENSE.txt" "$Prefix\licenses\LICENSE.physfs.txt" -Force

# 5. Manifest: what is in the prefix and where it came from.
$manifest = [ordered]@{
    written   = (Get-Date -Format 'yyyy-MM-ddTHH:mm:sszzz')
    generator = $Generator
    archives  = @($Archives | ForEach-Object { [ordered]@{ name = $_.Name; url = $_.Url; sha256 = $_.Sha256 } })
    lib       = @(Get-ChildItem "$Prefix\lib\*.lib" | ForEach-Object Name)
    bin       = @(Get-ChildItem "$Prefix\bin\*.dll" | ForEach-Object Name)
}
$manifest | ConvertTo-Json -Depth 4 | Set-Content -Encoding ascii (Join-Path $Prefix 'apx-deps-manifest.json')
Step "done: prefix $Prefix"
