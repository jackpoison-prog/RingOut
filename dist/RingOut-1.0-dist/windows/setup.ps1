# Ring Out - Ver 1.0 : first-time setup (Windows)
#
# Builds your personal copy from a GameCube disc image you already have. Nothing
# game-derived ships with this package -- this script extracts the disc and
# recompiles its executable here, on your machine.
#
# Usage:  .\setup.ps1 C:\path\to\your\disc.iso
#
# Unlike the Linux build, the toolchain is bundled: toolchain\ carries clang,
# lld, cmake and ninja, so there is nothing for you to install.

[CmdletBinding()]
param([Parameter(Position = 0)][string]$Iso)

$ErrorActionPreference = 'Stop'
$Here  = Split-Path -Parent $MyInvocation.MyCommand.Path
$Deps  = Join-Path $Here 'module-src\deps'
$Tools = Join-Path $Here 'toolchain'

function Die($msg) { Write-Host $msg -ForegroundColor Red; exit 1 }

if (-not $Iso -or -not (Test-Path -LiteralPath $Iso -PathType Leaf)) {
    Write-Host "Usage: .\setup.ps1 C:\path\to\your\disc.iso"
    Write-Host ""
    Write-Host "Supply a GameCube disc image you already have."
    exit 1
}
$Iso = (Resolve-Path -LiteralPath $Iso).Path

# --- toolchain ------------------------------------------------------------
# Prefer the bundled toolchain; fall back to anything already on PATH so a
# developer with their own LLVM is not forced to use ours.
function Find-Tool($name) {
    $bundled = Join-Path $Tools "bin\$name.exe"
    if (Test-Path -LiteralPath $bundled) { return $bundled }
    $onPath = Get-Command "$name.exe" -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    return $null
}

$Clang = Find-Tool 'clang'
$Cmake = Find-Tool 'cmake'
$Ninja = Find-Tool 'ninja'

# The module's CMake does find_package(Python3 REQUIRED) for gen_module_tables.py.
# The bundled interpreter is the embeddable build, which is deliberately not on
# PATH and not in the registry, so CMake will never discover it on its own --
# it has to be pointed at explicitly.
$Python = Join-Path $Tools 'python\python.exe'
if (-not (Test-Path -LiteralPath $Python)) {
    $onPath = Get-Command 'python.exe' -ErrorAction SilentlyContinue
    $Python = if ($onPath) { $onPath.Source } else { $null }
}

$missing = @()
if (-not $Clang)  { $missing += 'clang' }
if (-not $Cmake)  { $missing += 'cmake' }
if (-not $Ninja)  { $missing += 'ninja' }
if (-not $Python) { $missing += 'python' }
if ($missing.Count -gt 0) {
    Die ("Bundled toolchain is incomplete -- missing: {0}`nExpected it under {1}`nRe-download the release; the toolchain\ folder must be extracted with everything else." -f ($missing -join ', '), $Tools)
}

# A compiler being PRESENT is not the same as a compiler that WORKS. The Linux
# build learned this the hard way on SteamOS (clang present, libc headers
# absent, failure only after several minutes of extracting and recompiling).
# Compile something trivial FIRST -- an unpacked-wrong or AV-quarantined
# toolchain fails here in a second instead of ten minutes in.
$probe = Join-Path ([System.IO.Path]::GetTempPath()) ("ringout-probe-" + [guid]::NewGuid())
New-Item -ItemType Directory -Force -Path $probe | Out-Null
try {
    $probeSrc = Join-Path $probe 'probe.c'
    @'
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
int main(void) { return (int)strlen(""); }
'@ | Set-Content -LiteralPath $probeSrc -Encoding ASCII

    & $Clang $probeSrc -o (Join-Path $probe 'probe.exe') 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Die @"
The bundled C compiler cannot compile a trivial program.

Most likely causes:
  1. Antivirus quarantined part of toolchain\ -- clang and lld are frequently
     false-positived. Check your AV quarantine and restore/exclude this folder.
  2. The zip was extracted with a tool that dropped or truncated files. Extract
     again with Windows Explorer or 7-Zip.
"@
    }
} finally {
    Remove-Item -Recurse -Force -LiteralPath $probe -ErrorAction SilentlyContinue
}
Write-Host "Using C compiler: $Clang"

# --- 1/3 extract ----------------------------------------------------------
# Check the image BEFORE the long extract, and say something useful. dolrecomp
# accepts only .iso and .wbfs, and wants a plain uncompressed image: it reads
# the GameCube magic C2 33 9F 3D at offset 0x1C. A compressed image (NKit, RVZ
# renamed to .iso) passes the extension test and then fails with a message that
# does not explain what to do about it.
$ext = [System.IO.Path]::GetExtension($Iso).ToLowerInvariant()
if ($ext -notin '.iso', '.wbfs') {
    Die @"
'$ext' images cannot be read directly -- only .iso and .wbfs are supported.

Convert it to a plain ISO first. In Dolphin: right-click the game ->
Properties -> Convert File, choose format "ISO" with no compression.
"@
}
if ($ext -eq '.iso') {
    $fs = [System.IO.File]::OpenRead($Iso)
    try {
        $hdr = New-Object byte[] 32
        $null = $fs.Read($hdr, 0, 32)
    } finally { $fs.Dispose() }
    # Big-endian 0xC2339F3D at 0x1C.
    if (-not ($hdr[0x1C] -eq 0xC2 -and $hdr[0x1D] -eq 0x33 -and
              $hdr[0x1E] -eq 0x9F -and $hdr[0x1F] -eq 0x3D)) {
        $id = -join ($hdr[0..5] | ForEach-Object { if ($_ -ge 32 -and $_ -lt 127) { [char]$_ } else { '.' } })
        $sizeMB = [int]((Get-Item -LiteralPath $Iso).Length / 1MB)
        # A GameCube disc holds at most ~1.36 GB. Anything materially larger is
        # a DVD -- the PS2 or Xbox release of the same game, which people pick
        # by mistake because the filename looks right. Telling them to
        # "decompress it in Dolphin" would be useless advice for that case.
        if ($sizeMB -gt 1500) {
            Die @"
That image is $sizeMB MB, which is too large to be a GameCube disc (max ~1400 MB).

It is almost certainly the PlayStation 2 or Xbox release of the game. Those
are the same game but a completely different console, and this port recompiles
GameCube PowerPC code specifically -- it cannot use them.

  disc id read: '$id'

You need the GameCube version, around 1.0-1.4 GB, whose first six bytes are a
disc id like GRSEAF.
"@
        }
        Die @"
That .iso is not a plain GameCube disc image.

The GameCube signature is missing from its header, which usually means the file
is compressed -- an NKit image, or an RVZ/GCZ renamed to .iso. It has to be
decompressed before it can be recompiled.

  disc id read: '$id'
  size:         $sizeMB MB

In Dolphin: right-click the game -> Properties -> Convert File, choose
format "ISO" and compression "None".
"@
    }
}

Write-Host "==> 1/3  Extracting disc"
$Game = Join-Path $Here 'game'
if (Test-Path -LiteralPath $Game) { Remove-Item -Recurse -Force -LiteralPath $Game }
# 2>&1 so dolrecomp's stderr lands in this window; it explains the failure and
# was previously invisible to anyone reporting a problem.
& (Join-Path $Here 'tools\dolrecomp.exe') extract $Iso $Game 2>&1 | Write-Host
if ($LASTEXITCODE -ne 0) { Die "Disc extraction failed (dolrecomp exit $LASTEXITCODE). See the messages above." }

$bootBin = Join-Path $Game 'sys\boot.bin'
if (-not (Test-Path -LiteralPath $bootBin)) { Die "Could not read a disc ID -- is that a GameCube disc image?" }
$idBytes = [System.IO.File]::ReadAllBytes($bootBin)[0..5]
$DiscId  = -join ($idBytes | ForEach-Object { [char]$_ })
if ($DiscId -notmatch '^[A-Za-z0-9]{6}$') { Die "Could not read a disc ID -- is that a GameCube disc image?" }
Write-Host "    disc id: $DiscId"

# --- 2/3 recompile --------------------------------------------------------
Write-Host "==> 2/3  Recompiling the game executable (several minutes)"
$Work = Join-Path $Here 'work'
if (Test-Path -LiteralPath $Work) { Remove-Item -Recurse -Force -LiteralPath $Work }
New-Item -ItemType Directory -Force -Path $Work | Out-Null

$jobs = [Environment]::ProcessorCount
# --idle-pc is NOT optional, and this script omitted it until 1.6.1 (RingOut#12).
# Loop back-edges compile to native gotos, which is where much of the speed comes
# from -- but the game's idle spin loop must stay a dispatcher return, or the
# host never sees the game idling and idle-skip silently stops working. Without
# the flag the recompiler does NOT detect it: the loop becomes a native goto and
# the CPU thread spins at full load, which is exactly "one core pegged, well
# under 60". "auto" finds the loop in THIS disc (0x80185DEC US, 0x8017F35C JP,
# 0x8018D544 PAL) and prints it; setup.sh has passed it since 2026-08-25.
# --leader-cases: switch cases only where control can arrive, not one per
# instruction (identical game state; -8.96% CPU cycles with its retrained
# profile). EVERY KNOWN DISC, exactly as setup.sh: the flag changes every chunk's
# control flow, so a disc gets it only once its own profile has been retrained
# with it. All three have been, each gated on identical frame hashes over 16000
# frames first: JP -20.00% cycles, PAL -21.32%, Plus -20.73%, against what
# those players get today. An unknown disc ID still gets none of this.
$LeaderCases = @()
if ('GRSEAF', 'GRSJAF', 'GRSPAF', 'GRSEPS' -contains $DiscId) { $LeaderCases = @('--leader-cases') }
if ('GRSEAF', 'GRSEPS' -contains $DiscId) {
    # --direct-calls, exactly as setup.sh: cross-chunk calls stay native (-22.6%
    # CPU cycles with its profile). It changes guest timing slightly, so replays
    # carry a timing marker. Every PC the run loop hooks (the movie player's)
    # must still go through the dispatcher, so each is passed as --dispatch-pc.
    # US and Plus take this list: Plus is a hack of the US text, so its movie
    # library is byte-identical at all six of these addresses, and it was
    # playtested on this build. JP is playtested too but needs none of them
    # (see below). PAL is measured and gated but awaits a playtest.
    $LeaderCases = @('--leader-cases', '--direct-calls',
        '--dispatch-pc', '0x8020C1E8', '--dispatch-pc', '0x80209138', '--dispatch-pc', '0x8020D3B8',
        '--dispatch-pc', '0x80207E90', '--dispatch-pc', '0x80207EE8', '--dispatch-pc', '0x80208244',
        # --self-calls, as setup.sh: a call within the same chunk is native too
        # (-5.8% CPU cycles on top, with its own retrained profile).
        '--self-calls')
}
elseif ($DiscId -eq 'GRSPAF') {
    # PAL, playtested, with its OWN six: the movie library sits +0x7750 from the
    # US one (mapped by .github/scripts/map-fmv-hooks.py, all six agreeing on
    # that delta). The runtime's per-disc table (StaticRecompFmvHooks.h) hooks
    # these same six, which needs them to stay dispatch points; it is also what
    # the playtested build was built with.
    $LeaderCases = @('--leader-cases', '--direct-calls',
        '--dispatch-pc', '0x80213938', '--dispatch-pc', '0x80210888', '--dispatch-pc', '0x80214B08',
        '--dispatch-pc', '0x8020F5E0', '--dispatch-pc', '0x8020F638', '--dispatch-pc', '0x8020F994',
        '--self-calls')
}
elseif ($DiscId -eq 'GRSJAF') {
    # JP, playtested too, but with NO --dispatch-pc and that is deliberate: its
    # movie library is not mapped, so the run loop hooks nothing on this disc
    # (no row in the per-disc table), and the US addresses are not entry points
    # here at all (0 entry-switch cases, absent from generated_entries.txt).
    # The idle PC protects itself.
    $LeaderCases = @('--leader-cases', '--direct-calls', '--self-calls')
}
# Chunk overhang, as in setup.sh: US, Plus and PAL only (one chunk boundary on
# those three falls inside a hot loop); not JP, whose layout does not straddle.
if ($DiscId -in @('GRSEAF', 'GRSEPS', 'GRSPAF')) { $LeaderCases += @('--chunk-overhang', '512') }
# RAM bases, as in setup.sh: stores based on r1, r2 or r13 (stack pointer and the
# small-data bases) skip the RAM/MMIO range test. All four discs.
if ($DiscId -in @('GRSEAF', 'GRSEPS', 'GRSJAF', 'GRSPAF')) { $LeaderCases += @('--ram-bases', '1,2,13') }
# One MSR.FP test per straight-line run of FP instructions, as in setup.sh. All four discs.
if ($DiscId -in @('GRSEAF', 'GRSEPS', 'GRSJAF', 'GRSPAF')) { $LeaderCases += @('--fp-check-once') }
# Twin chunks, as in setup.sh: a fast copy of each chunk entered only at the
# entry PCs a training run used (the list ships beside the profile), the
# ordinary chunk as a cold fallback. A disc without a list builds as before.
$Twin = @()
$HotList = Join-Path $Here "module-src\profiles\$DiscId.hot"
# --twin-cr as in setup.sh: CR fields in write-through locals in the fast copy.
if (Test-Path $HotList) { $Twin = @('--twin-hot', $HotList, '--twin-regs', 'ratio', '--twin-cr') }
& (Join-Path $Here 'tools\dolrecomp.exe') --gamecube (Join-Path $Game 'sys\main.dol') --idle-pc auto @LeaderCases @Twin "-j$jobs" (Join-Path $Work 'out')
if ($LASTEXITCODE -ne 0) { Die "Recompilation failed." }

# gen_module_tables.py reads main.dol from alongside the generated sources.
Copy-Item (Join-Path $Game 'sys\main.dol') (Join-Path $Work 'out\generated\main.dol')

# --- 3/3 build the module -------------------------------------------------
# ZIP stores DOS timestamps with no timezone, so files written by CI at 21:49
# UTC extract as 21:49 LOCAL. Anyone west of UTC therefore ends up with build
# inputs dated in the future, and ninja can never make build.ninja newer than
# CMakeLists.txt:
#
#   ninja: error: manifest 'build.ninja' still dirty after 100 tries,
#                 perhaps system time is not set
#
# The clock is fine; the files are ahead of it. Pull anything future-dated back
# to now before configuring.
$now = Get-Date
$future = @(Get-ChildItem (Join-Path $Here 'module-src') -Recurse -File -ErrorAction SilentlyContinue |
           Where-Object { $_.LastWriteTime -gt $now })
if ($future.Count -gt 0) {
    Write-Host "    normalising $($future.Count) future-dated file(s) from the archive"
    foreach ($f in $future) { try { $f.LastWriteTime = $now } catch { } }
}

Write-Host "==> 3/3  Building the module"
# PROFILE-GUIDED BUILD, PICKED BY DISC ID -- the same choice setup.sh makes, and
# another thing this script never did until 1.6.1 (RingOut#12). The profiles and
# llvm-profdata have shipped in this package all along; nothing passed them in,
# and the module's CMake builds unprofiled unless MODULE_PGO_PROFILE is set.
# Worth 10-14% of CPU time on a real match.
#
# A profile only fits the disc it was trained on (chunk functions are named by
# guest address), and a MISMATCHED one is worse than none -- so this disc's
# profile or nothing. SC2 Plus hooks the US executable in place, so its chunks
# are the US disc's and it takes the US profile. The CMakeLists still probes the
# file with this clang and builds normally if it cannot use it.
# Not $Profile: PowerShell variables are case-insensitive, and $PROFILE is an
# automatic variable holding the user's profile-script path.
$Profiles = Join-Path $Here 'module-src\profiles'
$PgoProfile = Join-Path $Profiles "$DiscId.profdata"
if (-not (Test-Path -LiteralPath $PgoProfile) -and $DiscId -eq 'GRSEPS') {
    $PgoProfile = Join-Path $Profiles 'GRSEAF.profdata'
}
$PgoArgs = @()
if (Test-Path -LiteralPath $PgoProfile) {
    $PgoArgs = @("-DMODULE_PGO_PROFILE=$PgoProfile")
    Write-Host "    profile-guided build for $DiscId (this takes longer, and is worth it)"
} else {
    Write-Host "    no profile ships for $DiscId, so this builds without one."
    Write-Host "    Your module is correct; a profiled build is 10-14% faster."
}

# Inline paired-single fast path: every known disc, same gate as --leader-cases
# -- it changes chunk control flow, so a disc needs a profile trained with it,
# and all four now have one.
# MODULE_MEM_FAST, as setup.sh: leaner inlined RAM accesses (-6.3% CPU cycles
# with its retrained profile; the game's behaviour is unchanged).
$PsqFast = @()
if ('GRSEAF', 'GRSJAF', 'GRSPAF', 'GRSEPS' -contains $DiscId) {
    $PsqFast = @('-DMODULE_PSQ_FAST=ON', '-DMODULE_MEM_FAST=ON')
}

& $Cmake -S (Join-Path $Here 'module-src') -B (Join-Path $Work 'build') -GNinja `
    "-DCMAKE_MAKE_PROGRAM=$Ninja" `
    -DCMAKE_BUILD_TYPE=Release `
    "-DCMAKE_C_COMPILER=$Clang" `
    "-DPython3_EXECUTABLE=$Python" `
    -DCMAKE_C_FLAGS="-march=native" `
    @PgoArgs `
    @PsqFast `
    "-DGAME_ID=$DiscId" `
    "-DGENERATED_DIR=$(Join-Path $Work 'out\generated')" `
    "-DDOLRECOMP_SRC=$(Join-Path $Deps 'dolrecomp-src')" `
    "-DGXRUNTIME_INC=$(Join-Path $Deps 'gxruntime-include')" `
    "-DCHASSIS_ABI_DIR=$(Join-Path $Deps 'chassis-abi')" `
    "-DMODULE_TEMPLATE=$(Join-Path $Deps 'module-template')"
if ($LASTEXITCODE -ne 0) { Die "Module configure failed." }

& $Cmake --build (Join-Path $Work 'build')
if ($LASTEXITCODE -ne 0) { Die "Module build failed." }

$dll = Join-Path $Work "build\g${DiscId}_recomp.dll"
if (-not (Test-Path -LiteralPath $dll)) { Die "Module built but g${DiscId}_recomp.dll was not produced." }
Copy-Item $dll (Join-Path $Here 'bin') -Force

# The game's own artwork, taken from the disc you supplied -- the same step
# setup.sh runs on Linux, and the same rule: NONE of it ships in this package.
# It belongs to the publisher, so it is extracted here on your machine exactly
# as the module above is. gc-art.py is standard library only, so the bundled
# Python runs it with nothing installed.
#
# art\icon.ico is what the shortcuts point at, preferring the memory-card icon
# and falling back to the disc banner. The card icon only exists once you have
# saved, so this is worth re-running after you have played -- the shortcut then
# picks up the real icon.
Write-Host "==> artwork"
$gcArt = Join-Path $Here 'tools\gc-art.py'
if ((Test-Path -LiteralPath $gcArt) -and (Test-Path -LiteralPath $Python)) {
    & $Python $gcArt $Here
    $ico = Join-Path $Here 'art\icon.ico'
    if (Test-Path -LiteralPath $ico) {
        # Repoint the shortcuts the installer made. They were created before a
        # disc existed, so they could not have had the game's icon until now.
        # ONLY a shortcut that launches THIS folder: the names are fixed, so a
        # second copy (an unzipped package, a test folder) finds the INSTALLED
        # game's shortcuts too, and used to point their icon at itself -- the
        # icon then broke when that copy was deleted.
        $shell = New-Object -ComObject WScript.Shell
        $mine = [IO.Path]::GetFullPath($Here).TrimEnd('\') + '\'
        foreach ($lnk in @(
            (Join-Path ([Environment]::GetFolderPath('Desktop')) 'Ring Out.lnk'),
            (Join-Path ([Environment]::GetFolderPath('Programs')) 'Ring Out\Ring Out.lnk'))) {
            if (Test-Path -LiteralPath $lnk) {
                $sc = $shell.CreateShortcut($lnk)
                $target = $sc.TargetPath
                if ($target -and [IO.Path]::GetFullPath($target).StartsWith($mine, [StringComparison]::OrdinalIgnoreCase)) {
                    $sc.IconLocation = $ico
                    $sc.Save()
                    Write-Host "    icon set on $(Split-Path $lnk -Leaf)"
                } else {
                    Write-Host "    $(Split-Path $lnk -Leaf) launches another copy ($target) -- left alone"
                }
            }
        }
    }
} else {
    Write-Host "    skipped (no gc-art.py or python)"
}

# Bundled post-processing filters (scanlines, CRT). Dolphin only searches
# <userdir>\Shaders, so they are installed there. Existing files are left
# alone so an edited filter is never overwritten.
$shaderSrc = Join-Path $Here 'shaders'
if (Test-Path -LiteralPath $shaderSrc) {
    $shaderDst = Join-Path $Here 'userdata\Shaders'
    New-Item -ItemType Directory -Force -Path $shaderDst | Out-Null
    Get-ChildItem $shaderSrc -Filter *.glsl | ForEach-Object {
        $target = Join-Path $shaderDst $_.Name
        if (-not (Test-Path -LiteralPath $target)) { Copy-Item $_.FullName $target }
    }
}

Write-Host ""
Write-Host "Setup complete. Run RingOut.exe to play."
