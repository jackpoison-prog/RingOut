# Assembles the Windows release: the built binaries, the redistributable
# scaffolding, and a self-contained toolchain the user builds their module with.
#
# Runs on the CI runner, not a developer machine -- llvm-mingw alone unpacks to
# roughly 2 GB.
#
# Why a whole toolchain ships: the module is recompiled from the user's own
# disc, so it can never be prebuilt for them, and requiring a multi-GB Visual
# Studio install to play a game is not a real option. llvm-mingw is the only
# self-contained choice -- clang alone is not enough on Windows, it still needs
# a C runtime and headers from somewhere.

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$BinDir,     # built .exe files
    [Parameter(Mandatory = $true)][string]$RepoRoot,
    [Parameter(Mandatory = $true)][string]$OutDir
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'   # Invoke-WebRequest is ~10x slower with a progress bar

# From the VERSION file, exactly as package-dist.sh and package-deck.sh do.
# Hardcoding this shipped 1.0 in the stage directory, the zip and the installer
# name no matter what was actually built -- the same class of bug as the "Ver
# 1.0" title bar that survived every release up to 1.5.1.
$Version = (Get-Content (Join-Path $RepoRoot 'VERSION') -Raw).Trim()
if (-not $Version) { Write-Error "VERSION is empty or missing"; exit 1 }
Write-Host "==> packaging version $Version"

$stage = Join-Path $OutDir "RingOut-$Version"
$dl    = Join-Path $OutDir '_dl'
New-Item -ItemType Directory -Force -Path $stage, $dl | Out-Null

function Get-LatestAsset($repo, $pattern) {
    # Pinning tags rots; asking the API for the current release does not.
    $rel = Invoke-RestMethod "https://api.github.com/repos/$repo/releases/latest" `
        -Headers @{ 'User-Agent' = 'ringout-ci'; 'Accept' = 'application/vnd.github+json' }
    $asset = $rel.assets | Where-Object { $_.name -like $pattern } | Select-Object -First 1
    if (-not $asset) { throw "no asset matching '$pattern' in latest release of $repo" }
    Write-Host "  $repo -> $($asset.name)"
    return $asset.browser_download_url
}

function Get-TaggedAsset($repo, $tag, $pattern) {
    # For a dependency whose VERSION matters, where "latest" is a trap.
    $rel = Invoke-RestMethod "https://api.github.com/repos/$repo/releases/tags/$tag" `
        -Headers @{ 'User-Agent' = 'ringout-ci'; 'Accept' = 'application/vnd.github+json' }
    $asset = $rel.assets | Where-Object { $_.name -like $pattern } | Select-Object -First 1
    if (-not $asset) { throw "no asset matching '$pattern' in $repo release $tag" }
    Write-Host "  $repo@$tag -> $($asset.name)"
    return $asset.browser_download_url
}

function Fetch($url, $file) {
    $path = Join-Path $dl $file
    Write-Host "downloading $file"
    Invoke-WebRequest -Uri $url -OutFile $path
    return $path
}

# --- the built runtime ----------------------------------------------------
Write-Host "==> binaries"
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'bin'), (Join-Path $stage 'tools') | Out-Null
Copy-Item (Join-Path $BinDir 'moderngekko-run.exe') (Join-Path $stage 'bin') -Force
Copy-Item (Join-Path $BinDir 'dolrecomp.exe')       (Join-Path $stage 'tools') -Force
# The artwork extractor, as the Linux package ships it. Standard library only,
# so the bundled embeddable Python runs it with nothing added.
Copy-Item (Join-Path $RepoRoot 'dist\shared\gc-art.py') (Join-Path $stage 'tools') -Force

# The runtime imports MSVCP140 / VCRUNTIME140; shipping the redist DLLs beats
# telling players to go and install the C++ redistributable first.
Write-Host "==> VC++ runtime"
# Take the NEWEST redist, not whichever the filesystem lists first. The runner
# carries several side by side, and the first match was the VS2019 (VC142) set
# while the binary is built with the v143 toolset. MSVCP140.dll is ABI-stable,
# but VCRUNTIME140_1.dll and MSVCP140_ATOMIC_WAIT.dll gain exports over time, so
# an older redist fails with "entry point not found" -- on a player's machine,
# never here, because CI has the real runtime installed system-wide.
$crt = Get-ChildItem 'C:\Program Files*\Microsoft Visual Studio\2022\*\VC\Redist\MSVC\*\x64\Microsoft.VC*.CRT' `
    -Directory -ErrorAction SilentlyContinue |
    Sort-Object {
        $v = $_.Parent.Parent.Name          # the 14.xx.yyyyy version folder
        try { [version]$v } catch { [version]'0.0.0' }
    } -Descending | Select-Object -First 1
if ($crt) {
    # BOTH bin\ and tools\. Windows resolves DLLs from the executable's OWN
    # directory, so shipping them next to moderngekko-run.exe does nothing for
    # dolrecomp.exe sitting alone in tools\ -- it imports VCRUNTIME140.dll,
    # failed to load on a machine without the redist installed, and died before
    # main() with no output whatsoever. A tool that prints nothing at all, not
    # even an error, is the signature of that failure.
    Copy-Item (Join-Path $crt.FullName '*.dll') (Join-Path $stage 'bin') -Force
    Copy-Item (Join-Path $crt.FullName '*.dll') (Join-Path $stage 'tools') -Force
    Write-Host "  from $($crt.FullName)"
    Get-ChildItem (Join-Path $stage 'bin') -Filter '*.dll' | ForEach-Object {
        Write-Host ("    {0}  {1}" -f $_.Name, $_.VersionInfo.FileVersion)
    }
} else {
    Write-Warning "VC++ redist DLLs not found -- players will need the redistributable installed"
}

# --- scaffolding ----------------------------------------------------------
Write-Host "==> scaffolding"
$src = Join-Path $RepoRoot 'dist\RingOut-1.0-dist'
# Windows-only scaffolding lives in windows\ so the Linux distribution stays
# unambiguous; it is flattened into the root of the package here.
foreach ($f in 'README.txt', 'CREDITS.txt') {
    Copy-Item (Join-Path $src $f) $stage -Force
}
foreach ($f in 'setup.ps1', 'RingOut.ps1', 'RingOut.cmd') {
    Copy-Item (Join-Path $src 'windows' | Join-Path -ChildPath $f) $stage -Force
}

# ffmpeg is bundled on Windows only -- see windows\CREDITS-ffmpeg.txt for why,
# which licence build to use, and the source offer LGPL requires. Appended
# rather than kept in the shared CREDITS.txt, because the Linux and Deck
# packages do not ship ffmpeg and must not claim to.
#
# NOT YET WIRED: nothing here downloads ffmpeg.exe into $stage. Verified by
# hand on the test laptop (2026-09-08) that dropping the BtbN win64-lgpl static
# build beside the launcher makes the movies play -- the runtime invokes a bare
# "ffmpeg", and Windows searches the working directory, which RingOut.cmd sets.
# Wire the download in alongside the llvm-mingw fetch below when this script is
# next actually run. Roughly 132 MB, so it more than doubles the package.
Add-Content -Path (Join-Path $stage 'CREDITS.txt') `
            -Value (Get-Content (Join-Path $src 'windows\CREDITS-ffmpeg.txt') -Raw)
Copy-Item (Join-Path $src 'module-src') $stage -Recurse -Force
Copy-Item (Join-Path $src 'shaders')    $stage -Recurse -Force
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'userdata\GameSettings') | Out-Null
Copy-Item (Join-Path $RepoRoot 'work\mg_userdir\GameSettings\GRSEAF.ini') `
          (Join-Path $stage 'userdata\GameSettings') -Force

# --- toolchain ------------------------------------------------------------
$tc = Join-Path $stage 'toolchain'
New-Item -ItemType Directory -Force -Path $tc | Out-Null

Write-Host "==> llvm-mingw"
# PINNED, and it must stay pinned. The player's setup compiles the module with
# this clang against module-src/profiles/*.profdata, and a profile only applies
# where the compiler computes the same control-flow hash per function as the
# clang that trained it. "latest" moved to clang 23 on 2026-08-26 and every
# Windows package from 1.6.2 on shipped it: the build still said "PGO enabled",
# but clang 23 discarded the profile for 61 functions including the three
# hottest chunks (3.8 billion counts; the same chunks under clang 22 -- for Linux
# or for Windows -- mismatch zero). Found 2026-09-29 by recompiling with
# -Wbackend-plugin, which the module build silences.
# 20260616 is LLVM 22.1.8, the exact clang the shipped profiles are trained with.
# Move this only together with retraining every profile, and keep the check
# below in step.
$llvmMingwTag = '20260616'
$profileClangMajor = 22
$llvmUrl = Get-TaggedAsset 'mstorsjo/llvm-mingw' $llvmMingwTag '*ucrt-x86_64.zip'
$llvmZip = Fetch $llvmUrl 'llvm-mingw.zip'
Expand-Archive $llvmZip -DestinationPath $dl -Force
$llvmRoot = Get-ChildItem $dl -Directory -Filter 'llvm-mingw-*' | Select-Object -First 1
if (-not $llvmRoot) { throw "llvm-mingw did not extract as expected" }
Copy-Item (Join-Path $llvmRoot.FullName '*') $tc -Recurse -Force
$clangLine = (& (Join-Path $tc 'bin\clang.exe') --version | Select-Object -First 1)
if ($clangLine -notmatch "clang version $profileClangMajor\.") {
    throw "bundled clang is '$clangLine' but the profiles need clang $profileClangMajor -- see the pin above"
}
Write-Host "  $clangLine (matches the profiles)"

# --- ffmpeg ---------------------------------------------------------------
# The game's movies are Sofdec (MPEG) inside movie.afs, and the FMV path decodes
# them by running ffmpeg. Linux gets it from the distribution; Windows has none,
# so without this the intro and cutscenes are a BLACK SCREEN with the audio
# still playing -- seen on the test laptop, and fixed there by dropping this
# exact binary beside the launcher. The runtime invokes a bare "ffmpeg" and
# Windows searches the working directory, which RingOut.cmd sets.
#
# PINNED, and LGPL not GPL. BtbN's "-gpl" builds add libx264/libx265/libxvid and
# are GPL-3.0, which would conflict with the GPL-2.0 runtime and relicense the
# whole distribution. The static build is one exe with no DLLs to ship.
# Attribution and the source offer LGPL requires are in
# windows\CREDITS-ffmpeg.txt, appended to CREDITS.txt above.
Write-Host "==> ffmpeg"
# Via Get-LatestAsset for the same reason llvm-mingw is: a hand-built URL rots.
# The pattern deliberately excludes "-shared": the static build is a single exe
# with no DLLs to ship alongside it.
$ffUrl = Get-LatestAsset 'BtbN/FFmpeg-Builds' '*win64-lgpl-9.0.zip'
$ffZip = Fetch $ffUrl 'ffmpeg.zip'
Expand-Archive $ffZip -DestinationPath $dl -Force
$ffExe = Get-ChildItem $dl -Recurse -File -Filter 'ffmpeg.exe' | Select-Object -First 1
if (-not $ffExe) { throw "ffmpeg.exe not found in $ffUrl" }
Copy-Item $ffExe.FullName $stage -Force
Write-Host ("  ffmpeg.exe {0:N0} bytes" -f (Get-Item (Join-Path $stage 'ffmpeg.exe')).Length)

# Trim what a module build never touches.
#
# ** Do not remove toolchain\include. ** It looks like stray headers and it is
# the mingw-w64 sysroot -- windows.h, string.h, all of it. Deleting it produced
# a toolchain that unpacked perfectly and could not compile "#include
# <windows.h>". The sysroot is include\, x86_64-w64-mingw32\ and lib\clang\.
Write-Host "==> trimming toolchain"
$before = (Get-ChildItem $tc -Recurse -File | Measure-Object Length -Sum).Sum
Get-ChildItem (Join-Path $tc 'bin') -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match '^(lldb|clangd|clang-repl|clang-check|clang-tidy|clang-format|clang-refactor|clang-rename|clang-scan-deps|llvm-lto|llvm-reduce|llvm-exegesis|bugpoint|opt|llc|lli|clang-doc|llvm-cfi-verify|clang-linker-wrapper|cmake-gui|ctest|cpack)' } |
    Remove-Item -Force -ErrorAction SilentlyContinue
Remove-Item (Join-Path $tc 'bin\liblldb.dll') -Force -ErrorAction SilentlyContinue
# The other three target triples are dead weight for an x86_64-only release.
foreach ($t in 'aarch64-w64-mingw32', 'armv7-w64-mingw32', 'i686-w64-mingw32') {
    Remove-Item (Join-Path $tc $t) -Recurse -Force -ErrorAction SilentlyContinue
}
# Sanitizer runtimes, and the Linux ones in particular, on a Windows-only bundle.
Get-ChildItem (Join-Path $tc 'lib\clang') -Directory -ErrorAction SilentlyContinue | ForEach-Object {
    Remove-Item (Join-Path $_.FullName 'lib\linux') -Recurse -Force -ErrorAction SilentlyContinue
}
Remove-Item (Join-Path $tc 'share\doc'), (Join-Path $tc 'share\man') `
    -Recurse -Force -ErrorAction SilentlyContinue
$after = (Get-ChildItem $tc -Recurse -File | Measure-Object Length -Sum).Sum
Write-Host ("  {0:N0} MB -> {1:N0} MB" -f ($before / 1MB), ($after / 1MB))

# --- launcher -------------------------------------------------------------
# Built with the toolchain we just unpacked, so no dev shell is needed, and it
# is a real .exe so players double-click something that looks like a game rather
# than a .cmd that bypasses PowerShell's execution policy.
#
# This step doubles as the TRIM CANARY, and has already earned it: an
# over-aggressive trim removed the mingw sysroot and this is what caught it.
# Without a compile here, a broken toolchain ships and fails on a player's
# machine halfway through setup.ps1.
#
# -flto=thin is deliberate even though the launcher does not need it: the module
# build uses ThinLTO, so this exercises the same clang + lld + LTO plugin path
# the user's own build depends on.
Write-Host "==> launcher (also verifies the trimmed toolchain)"
$clang = Join-Path $tc 'bin\clang.exe'
if (-not (Test-Path -LiteralPath $clang)) { throw "bundled clang missing at $clang" }
$launcherSrc = Join-Path $RepoRoot 'dist\RingOut-1.0-dist\windows\launcher\RingOut.c'
$launcherExe = Join-Path $stage 'RingOut.exe'
& $clang $launcherSrc -o $launcherExe -municode -O2 -s -flto=thin -lcomdlg32
if ($LASTEXITCODE -ne 0) { throw "launcher build failed -- the bundled toolchain is broken (over-trimmed?)" }
Write-Host ("  RingOut.exe {0:N0} KB" -f ((Get-Item $launcherExe).Length / 1KB))

Write-Host "==> ninja"
$ninjaZip = Fetch (Get-LatestAsset 'ninja-build/ninja' 'ninja-win.zip') 'ninja.zip'
Expand-Archive $ninjaZip -DestinationPath (Join-Path $tc 'bin') -Force

Write-Host "==> cmake"
$cmakeZip = Fetch (Get-LatestAsset 'Kitware/CMake' 'cmake-*-windows-x86_64.zip') 'cmake.zip'
Expand-Archive $cmakeZip -DestinationPath $dl -Force
$cmakeRoot = Get-ChildItem $dl -Directory -Filter 'cmake-*-windows-x86_64' | Select-Object -First 1
if (-not $cmakeRoot) { throw "cmake did not extract as expected" }
# Merge into the same prefix: cmake finds its modules at ../share/cmake-X.Y
# relative to the executable, so bin/ and share/ must stay siblings.
Copy-Item (Join-Path $cmakeRoot.FullName 'bin\*')   (Join-Path $tc 'bin')   -Recurse -Force
Copy-Item (Join-Path $cmakeRoot.FullName 'share\*') (Join-Path $tc 'share') -Recurse -Force

Write-Host "==> python"
# gen_module_tables.py needs an interpreter; the embeddable build is ~10 MB.
$pyVer = '3.11.9'
$pyZip = Fetch "https://www.python.org/ftp/python/$pyVer/python-$pyVer-embed-amd64.zip" 'python.zip'
Expand-Archive $pyZip -DestinationPath (Join-Path $tc 'python') -Force

# --- installer ------------------------------------------------------------
# The .exe is the primary download; the .zip stays for people who prefer a
# portable folder they can drop anywhere.
Write-Host "==> installer"
$iscc = Get-ChildItem 'C:\Program Files (x86)\Inno Setup 6\ISCC.exe',
                      'C:\Program Files\Inno Setup 6\ISCC.exe' -ErrorAction SilentlyContinue |
        Select-Object -First 1
if (-not $iscc) { throw "ISCC.exe (Inno Setup 6) not found on this runner" }
Write-Host "  using $($iscc.FullName)"

Copy-Item (Join-Path $PSScriptRoot 'ringout.iss') $OutDir -Force
Push-Location $OutDir
try {
    # /D overrides the .iss's own #define, so VERSION is the single source of
    # truth and ringout.iss no longer decides what it is building.
    # StageDir as well as AppVersion: the .iss packages a directory by name, and
    # deriving it there from AppVersion would still be a second place that has
    # to agree with $stage. Pass the one this script actually created.
    & $iscc.FullName "/DAppVersion=$Version" "/DStageDir=$(Split-Path $stage -Leaf)" 'ringout.iss'
    if ($LASTEXITCODE -ne 0) { throw "ISCC failed with exit code $LASTEXITCODE" }
} finally {
    Pop-Location
}
Remove-Item (Join-Path $OutDir 'ringout.iss') -Force -ErrorAction SilentlyContinue

# --- checks ---------------------------------------------------------------
# The staging above is an allowlist, which is the right shape -- but
# package-deck.sh asserts the same invariants anyway, on the grounds that a
# package shipping the disc, the save card or the module is the failure that
# matters and should not rest on nobody having edited the allowlist. Windows
# had no equivalent. It also ships a userdata\ subtree (GameSettings only), so
# "no userdata" cannot be the rule here; name the sensitive files instead.
Write-Host "==> checks"
# 'art' matches package-dist.sh's list. setup.ps1 now extracts the game's own
# banner and memory-card icon into art\ on the PLAYER's machine, and that is
# publisher artwork -- exactly what must never end up in a package.
$forbidden = @('game', 'work', 'source', 'art', 'userdata\Config', 'userdata\GC',
               'userdata\Logs', 'bin\gGRSEAF_recomp.so')
foreach ($f in $forbidden) {
    if (Test-Path (Join-Path $stage $f)) {
        Write-Error "  FAIL: $f is in the stage"; exit 1
    }
}
$leaks = Get-ChildItem $stage -Recurse -File -Include `
    '*.gci','*.iso','*.sav','*_recomp.so','dolphin.log','RetroAchievements.ini', `
    'TimePlayed.ini','.netrc','id_rsa','id_ed25519' -ErrorAction SilentlyContinue
if ($leaks) {
    Write-Error "  FAIL: disc-, save- or credential-derived files in the stage:"
    $leaks | ForEach-Object { Write-Error "    $($_.FullName)" }
    exit 1
}
# Home paths compiled into a binary or left in a text file are the privacy axis
# the checks above do not cover; privacy-scan.sh is the Linux equivalent.
# Get-ChildItem -Recurse, THEN Select-String. Select-String has no -Recurse of
# its own, and passing it one is a hard error -- which is why this step failed
# with "A parameter cannot be found that matches parameter name 'Recurse'" the
# first time this script was ever run, after the installer had already built.
# The filter runs BEFORE Select-String so the bundled toolchain (~2 GB) is
# skipped rather than scanned.
$paths = Get-ChildItem $stage -Recurse -File -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -notmatch '\\toolchain\\|\\llvm-mingw|\\Externals\\|\\ffmpeg\.exe$' } |
    Select-String -Pattern 'C:\\Users\\[A-Za-z0-9_-]+|/home/[a-z0-9_-]+/|/Users/[A-Za-z0-9_-]+/' `
                  -List -ErrorAction SilentlyContinue
if ($paths) {
    Write-Error "  FAIL: developer paths in the stage:"
    $paths | ForEach-Object { Write-Error "    $($_.Path): $($_.Matches[0].Value)" }
    exit 1
}
Write-Host "  clean"

# --- zip ------------------------------------------------------------------
Write-Host "==> zipping"
$zip = Join-Path $OutDir "RingOut-$Version-windows-x64.zip"
Compress-Archive -Path $stage -DestinationPath $zip -CompressionLevel Optimal -Force
Remove-Item $dl -Recurse -Force -ErrorAction SilentlyContinue

Write-Host ""
Get-ChildItem $OutDir -File | Where-Object { $_.Extension -in '.exe', '.zip' } |
    ForEach-Object { Write-Host ("{0,-44} {1,6:N0} MB" -f $_.Name, ($_.Length / 1MB)) }
Get-ChildItem $stage | Select-Object Name, Length | Format-Table -AutoSize
