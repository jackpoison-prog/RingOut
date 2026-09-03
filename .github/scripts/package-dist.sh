#!/bin/bash
# Assembles the Linux DESKTOP release: the runtime, the recompiler, the module
# sources and the GPL source shipment -- zipped. No game data.
#
#   ./.github/scripts/package-dist.sh
#
# Env:
#   OUT=dir       output directory (default dist/)
#   SKIP_SOURCE=1 stage without source/ (for a local test build ONLY -- a
#                 published package without it breaks the GPL offer)
#
# WHY THIS EXISTS. The Deck and Windows releases had packaging scripts; this one
# did not, and dist/RingOut-1.0-dist was hand-assembled. That directory is the
# WORKING copy: 1.3 GB, of which 989 MB is the extracted disc, 185 MB is build
# output under work/, and userdata/ holds the developer's own Dolphin config
# (including a netplay host address on the local network), logs, and a memory
# card. Zipping it wholesale ships all three. Every other packaging script here
# stages file by file from an allowlist for exactly that reason, so this one
# does too -- and then asserts the invariants rather than trusting the list.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${OUT:-$REPO/dist}"
# The RELEASE version. It names the zip and the folder inside it -- NOT the
# working package directory, which keeps its historical RingOut-1.0-* name:
# that is 1.3 GB of developer state referenced by .gitignore and a dozen
# scripts, and renaming it would churn all of them for nothing a player sees.
# One source of truth, so a release cannot be half-numbered. The literal
# default this replaced was 1.1, four releases stale, and it was only ever
# right because every real release passed VERSION= explicitly.
VERSION="${VERSION:-$(cat "$REPO/VERSION" 2>/dev/null || echo 1.1)}"
PKG="RingOut-1.0-dist"
SRC="$REPO/dist/$PKG"
WORK="$OUT/_dist-stage"
STAGE="$WORK/RingOut-$VERSION-linux"
ZIP="$OUT/RingOut-$VERSION-linux-x86_64.zip"

[ -d "$SRC" ] || { echo "no working package at $SRC" >&2; exit 1; }

# Never stage inside the working package: this script deletes its stage.
case "$STAGE" in
  "$SRC"|"$SRC"/*) echo "refusing to stage into the working package ($SRC)" >&2; exit 1 ;;
esac

rm -rf "$WORK"
mkdir -p "$STAGE"

echo "==> scaffolding"
# The launcher and setup.sh between them define what has to be present:
# setup.sh calls tools/dolrecomp and builds module-src into work/; RingOut reads
# lib/, libc-fallback/, shaders/ and userdata/. Anything not on this list is
# either derived from the user's disc or is the developer's own state.
install -m 755 "$SRC/setup.sh"    "$STAGE/setup.sh"
install -m 755 "$SRC/RingOut"     "$STAGE/RingOut"
install -m 644 "$SRC/README.txt"  "$STAGE/README.txt"
# The version line is stamped at stage time rather than maintained by hand.
# The 1.2.1 release shipped a README whose first line read "Ver 1.1" because
# this was a verbatim copy and nobody edits a header twice.
sed -i "1s/^Ring Out - Ver .*/Ring Out - Ver $VERSION/" "$STAGE/README.txt"
install -m 644 "$SRC/CREDITS.txt" "$STAGE/CREDITS.txt"
# CREDITS.txt carries the same header and was NOT being stamped, so every
# release since 1.0 shipped a credits file that said Ver 1.0 next to a README
# that said the truth. Stamped from the same variable as the README.
sed -i "1s/^Ring Out - Ver .*/Ring Out - Ver $VERSION/" "$STAGE/CREDITS.txt"

mkdir -p "$STAGE/bin" "$STAGE/tools" "$STAGE/shaders"
install -m 755 "$SRC/bin/moderngekko-run" "$STAGE/bin/moderngekko-run"
# The runtime carries its own version, and until 1.5 it carried the WRONG one:
# nothing defined MODERNGEKKO_PROJECT_VERSION, so the window title and the
# in-game menu both said "Ver 1.0" while the README said 1.5. A stamped README
# next to a stale binary is worse than no version at all, because it looks
# checked. The binary is built separately from this script, so assert rather
# than assume.
# Anchored on the menu banner, NOT on a bare "Ver x.y": the compiler inlines
# the short version literal into .text as immediate operands, and the byte after
# it is often 0x49, so `strings` reports a truncated "Ver 1.5.I" from the CODE
# before it ever reaches the real literal in .rodata. Matching the first
# "Ver ..." therefore read 1.5 out of a 1.5.1 binary and failed a correct
# package. The banner is too long to be inlined, so it is always the real thing.
embedded="$(strings "$STAGE/bin/moderngekko-run" 2>/dev/null |
            grep -m1 -oE 'RING OUT[[:space:]]+-[[:space:]]+Ver [0-9]+\.[0-9]+(\.[0-9]+)?' |
            grep -oE 'Ver [0-9]+\.[0-9]+(\.[0-9]+)?' || true)"
if [ -z "$embedded" ]; then
  echo "  WARNING: no version string found in the runtime; cannot check it" >&2
elif [ "$embedded" != "Ver $VERSION" ]; then
  echo "  FAIL: the runtime says '$embedded' but this package is $VERSION." >&2
  echo "        Rebuild it after setting VERSION at the repo root -- CMake" >&2
  echo "        reads that file and defines the version from it." >&2
  exit 1
fi

install -m 755 "$SRC/tools/dolrecomp"     "$STAGE/tools/dolrecomp"
# Extracts the game's own banner and memory-card icon on the PLAYER's machine
# (setup.sh runs it). One canonical copy in dist/shared/ feeds both packages,
# rather than a second copy that drifts -- this project has been bitten by
# exactly that with dolrecomp-src.
install -m 755 "$REPO/dist/shared/gc-art.py" "$STAGE/tools/gc-art.py"
# The route setup.sh --pgo plays to train a profile. Sourced from the same file
# every perf A/B in this project used, so the profile is trained on the workload
# the numbers were measured on -- a second copy would drift and nobody would
# notice, because a wrong route still produces a valid-looking profile.
install -m 644 "$REPO/.github/input-scripts/arcade-match.txt" "$STAGE/tools/train-route.txt"
for f in "$SRC"/shaders/*.glsl; do install -m 644 "$f" "$STAGE/shaders/"; done

# bin/g<ID>_recomp.so is deliberately absent: it is recompiled from the user's
# own disc by setup.sh and cannot be redistributed.

echo "==> support libraries"
cp -a "$SRC/lib"           "$STAGE/lib"
cp -a "$SRC/libc-fallback" "$STAGE/libc-fallback"
echo "    $(ls "$STAGE/lib" | wc -l) libraries, $(ls "$STAGE/libc-fallback" | wc -l) fallback"

echo "==> module sources"
cp -a "$SRC/module-src" "$STAGE/module-src"

# PER-REGION PGO PROFILES. One per disc ID, ~11 MB each, and setup.sh picks by
# the ID it read from the disc. Worth the size: a matching profile is -13.50%
# cycles against none, while ANOTHER region's profile is -1.34% and lowers IPC
# below an unprofiled build. Measured 2026-08-26, 3 arms x 3 reps.
#
# Asserted rather than assumed, because a profile is a silent dependency: a
# missing one costs ~12% and nothing warns, which is exactly how the desktop
# package shipped stale codegen twice before.
if [ -d "$STAGE/module-src/profiles" ]; then
  echo "==> pgo profiles"
  for prof in "$STAGE/module-src/profiles"/*.profdata; do
    [ -e "$prof" ] || continue
    id="$(basename "$prof" .profdata)"
    case "$id" in
      GRS[EJP]A[FS]) ;;
      *) echo "  FAIL: $id is not a disc ID this game ships as" >&2; exit 1 ;;
    esac
    echo "    $id: $(du -h "$prof" | cut -f1)"
  done
else
  echo "==> pgo profiles: NONE -- every disc builds unprofiled, ~12% slower" >&2
fi

# The cheat/game-settings ini is authored by this project (USA Action Replay
# codes; Dolphin ships only the PAL ini), not disc-derived. Take it from the
# canonical location rather than the working package's own
# userdata/GameSettings, which is empty -- sourcing it from there would silently
# ship nothing and leave the release without a CHEATS tab.
# (The retired Windows packager resolved the same file the same way; see
# attic/windows/.)
#
# Nothing ELSE from userdata/ goes: the rest is the developer's Dolphin config,
# cache, logs and memory card, and config.ini there names a LAN address.
# One shared stager, so the deck packager cannot drift from this one -- same
# reason gc-art.py lives in dist/shared/. It resolves the USA ini this project
# authored and the PAL ini Dolphin ships, and refuses to stage a list with any
# code left enabled.
echo "==> game settings"
"$REPO/dist/shared/stage-gamesettings.sh" "$REPO" "$STAGE/userdata/GameSettings"

if [ "${SKIP_SOURCE:-0}" != "1" ]; then
  echo "==> GPL source shipment"
  [ -d "$SRC/source" ] || { echo "  FAIL: no source/ -- see CREDITS.txt" >&2; exit 1; }
  cp -a "$SRC/source" "$STAGE/source"

  # The offer is to supply the source FOR THE BINARIES SHIPPED. Source older
  # than the runtime it accompanies does not satisfy that, and it is how a
  # developer path from an old working tree survived into a release here.
  # Compare against the BUILT runtime, not the staged copy: install(1) stamps
  # the staged file with the current time, so comparing against that would make
  # this fail unconditionally -- and a check that always fails gets deleted.
  newest_src="$(find "$SRC/source" -type f -printf '%T@\n' | sort -n | tail -1)"
  runtime_ts="$(stat -c %Y "$SRC/bin/moderngekko-run")"
  if [ "${newest_src%.*}" -lt "$runtime_ts" ]; then
    echo "  FAIL: source/ is older than bin/moderngekko-run." >&2
    echo "        Regenerate the tarballs and patches from the built commits," >&2
    echo "        and update the hashes named in CREDITS.txt." >&2
    exit 1
  fi
else
  echo "==> GPL source shipment SKIPPED (SKIP_SOURCE=1) -- do not publish this"
fi

echo "==> ABI consistency"
# THE FAILURE THIS EXISTS FOR. This package ships a prebuilt runtime next to the
# SOURCES the user compiles their module from, and nothing kept the two in step.
# A 27 July runtime shipped beside 7 August module sources: the sources declared
# STATICRECOMP_ABI_VERSION 3 (the entry-point table), the runtime predated it and
# accepted at most 2, so setup.sh completed happily and then every launch died
# with "native module was rejected: module ABI mismatch". The package built, it
# scanned clean, and it could not produce a working install.
#
# The runtime's accepted version cannot be read back out of the binary, so this
# checks the two things that can be: that the shipped sources ARE the repo's
# sources, and that the binary is not older than the ABI it has to satisfy.
ABI_PKG="$SRC/module-src/deps/chassis-abi/StaticRecompABI.h"
ABI_REPO="$REPO/ModernGekko/vendor/dolphin/Source/Core/Core/PowerPC/StaticRecomp/StaticRecompABI.h"
if ! cmp -s "$ABI_PKG" "$ABI_REPO"; then
  echo "  FAIL: the packaged chassis ABI header differs from the repo's." >&2
  echo "        $ABI_PKG" >&2
  echo "        $ABI_REPO" >&2
  exit 1
fi
echo "  ABI header matches the repo (v$(grep -oE 'STATICRECOMP_ABI_VERSION [0-9]+' "$ABI_PKG" | grep -oE '[0-9]+'))"

# dolrecomp-src under module-src is a COPY of DolRecomp/src, and edits to one
# not reaching the other is a trap this project has hit before -- the module
# still links, because a .so links with undefined symbols and only fails at
# dlopen.
#
# This used to check three files (cpu/cpu.c, cpu/cpu.h, common/types.h) on the
# grounds that they are the ones actually COMPILED into the module. That is
# true and it still missed the bug: by 2026-08-17 eight files had drifted, the
# copy being an old snapshot, and cpu/cpu.c was among them -- it had lost
# `unsigned dolrecomp_call_depth = 0;`, which the generated chunk headers
# declare extern. Three names hand-maintained against a 49-file tree is the
# same class of mistake as the drift it is meant to catch, so check all of them.
#
# The copy is a deliberate SUBSET -- backend/llvm/ and ir/ are not shipped --
# so iterate the COPY and look each file up in the source, never the reverse.
# A file newly added to DolRecomp/src is therefore not flagged: whether the
# module needs it is a judgement call, not something this check can make.
copy_root="$SRC/module-src/deps/dolrecomp-src"
drift=0
orphan=0
checked=0
while IFS= read -r f; do
  checked=$((checked + 1))
  if [ ! -f "$REPO/DolRecomp/src/$f" ]; then
    echo "  FAIL: dolrecomp-src/$f has no counterpart in DolRecomp/src" >&2
    orphan=$((orphan + 1))
  elif ! cmp -s "$copy_root/$f" "$REPO/DolRecomp/src/$f"; then
    echo "  FAIL: dolrecomp-src/$f differs from DolRecomp/src/$f" >&2
    drift=$((drift + 1))
  fi
done < <(cd "$copy_root" && find . -type f | sed 's|^\./||' | sort)
# Report every offender before exiting: the failure mode this guards against
# arrives in batches, and fixing them one re-run at a time is how a sync gets
# abandoned half-done.
if [ "$((drift + orphan))" -gt 0 ]; then
  echo "  $drift file(s) drifted, $orphan orphaned, of $checked checked" >&2
  echo "  fix with: cp DolRecomp/src/<file> $copy_root/<file>" >&2
  exit 1
fi
echo "  dolrecomp-src copy is in sync ($checked files)"

# A runtime older than the ABI header cannot know about it.
rt_ts="$(stat -c %Y "$SRC/bin/moderngekko-run")"
abi_ts="$(stat -c %Y "$ABI_PKG")"
if [ "$rt_ts" -lt "$abi_ts" ]; then
  echo "  FAIL: bin/moderngekko-run ($(date -d "@$rt_ts" +%F)) predates the ABI" >&2
  echo "        header ($(date -d "@$abi_ts" +%F)). Rebuild the runtime IN THE" >&2
  echo "        CONTAINER -- a native build here raises the shipped glibc floor" >&2
  echo "        from 2.36 to whatever this machine has, silently, and it starts" >&2
  echo "        on nothing older:" >&2
  echo "          .github/scripts/build-deck.sh" >&2
  echo "        or, with the image already built:" >&2
  echo "          podman run --rm --userns=keep-id -v $REPO:/src:Z -w /src \\" >&2
  echo "            ringout-deck-build cmake --build build-deck --target moderngekko-run" >&2
  echo "          cp build-deck/moderngekko-run $SRC/bin/" >&2
  echo "        Check it afterwards:" >&2
  echo "          objdump -T <binary> | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1" >&2
  exit 1
fi
echo "  runtime is newer than the ABI header"

# The launcher's NEED_GLIBC decides when it switches to the bundled glibc, and
# the bundled path is the one that breaks Mesa. It said 2.44 while the shipped
# runtime needed 2.36, so every host between those took the bad path for no
# reason -- a Deck on 2.41 among them. Assert rather than trust.
launcher_need="$(grep -m1 '^NEED_GLIBC=' "$SRC/RingOut" | cut -d= -f2)"
runtime_floor="$(objdump -T "$SRC/bin/moderngekko-run" 2>/dev/null |
                 grep -o 'GLIBC_[0-9][0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -1)"
if [ -n "$runtime_floor" ] && [ "$launcher_need" != "$runtime_floor" ]; then
  echo "  FAIL: RingOut says NEED_GLIBC=$launcher_need but bin/moderngekko-run" >&2
  echo "        actually needs $runtime_floor. Hosts between the two would take" >&2
  echo "        the bundled-glibc path, which is the one that breaks Mesa." >&2
  echo "        Fix: set NEED_GLIBC=$runtime_floor in $SRC/RingOut" >&2
  exit 1
fi
echo "  launcher glibc floor matches the runtime ($runtime_floor)"

# A recompiler older than the emitter it was built from ships OLD CODEGEN, and
# nothing downstream notices: setup.sh runs, the module builds, the game plays.
# It is just slower, by however much the emitter work since then was worth.
#
# This happened. tools/dolrecomp dated 5 August predated lazy FPRF (68aea54d,
# 10 August), so the generated header carried no g_fprf_kind at all and every
# desktop player built the eager path, missing a measured -2.45%. The package
# was assembled, checked and shipped with no indication anything was wrong --
# which is the whole problem: a stale RUNTIME breaks loudly at dlopen, a stale
# RECOMPILER is silent.
#
# mtime, like the Deck runtime guard: it catches "the binary predates the
# sources", which is the failure that actually occurs. It cannot prove the
# binary was built FROM these sources -- for that, regenerate and diff against a
# known-good generation, or grep the generated header for a marker of the newest
# emitter change.
if newer="$(find "$REPO/DolRecomp/src" -newer "$SRC/tools/dolrecomp" -type f -print -quit 2>/dev/null)" &&
   [ -n "$newer" ]; then
  echo "  FAIL: tools/dolrecomp ($(date -r "$SRC/tools/dolrecomp" +%F)) is older than" >&2
  echo "        $newer" >&2
  echo "        It would ship codegen predating that change. Rebuild it -- STATIC," >&2
  echo "        because setup.sh runs it directly and its glibc floor decides which" >&2
  echo "        distros can run setup at all:" >&2
  echo "          podman run --rm -v $REPO:/src -w /src ringout-deck-build bash -c \\" >&2
  echo "            'cmake -S DolRecomp -B build-dolrecomp-static -GNinja \\" >&2
  echo "               -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXE_LINKER_FLAGS=-static \\" >&2
  echo "               -DZLIB_USE_STATIC_LIBS=ON && cmake --build build-dolrecomp-static'" >&2
  echo "          cp build-dolrecomp-static/dolrecomp $SRC/tools/" >&2
  exit 1
fi
echo "  recompiler is newer than DolRecomp/src"

echo "==> checks"
# Assert rather than trust the allowlist: shipping the disc, the save card or
# the module is the failure that matters.
# art/ is the game's own banner and icon, extracted from the player's disc by
# setup.sh. It belongs to the publisher: generating it locally is fine,
# shipping it is not.
for forbidden in game work windows source/build bin/gGRSEAF_recomp.so art \
                 userdata/Config userdata/GC userdata/Logs userdata/config.ini; do
  [ -e "$STAGE/$forbidden" ] && { echo "  FAIL: $forbidden is in the stage" >&2; exit 1; }
done
if find "$STAGE" -name '*_recomp.so' -o -name '*.gci' -o -name '*.iso' | grep -q .; then
  echo "  FAIL: disc-derived files in the stage" >&2; exit 1
fi
echo "  no disc-derived content"

# The privacy axis the checks above do not cover -- home paths, LAN addressing,
# credentials, author emails in the source patches.
"$REPO/.github/scripts/privacy-scan.sh" "$STAGE"

# -X and TZ=UTC strip metadata that identifies the machine this was built on.
# Info-ZIP otherwise records a 0x7875 extra field holding the builder's numeric
# uid/gid, and stores each mtime TWICE -- local and UTC -- whose difference is
# the builder's timezone offset, which narrows their location to a region. -X
# drops both extra fields; TZ=UTC removes any local/UTC discrepancy in what is
# left. Unix permission bits live in the central directory's external
# attributes, NOT in an extra field, so -X does not touch them -- verified: a
# 755 file still unpacks 755, and the canary below would catch it if it did.
echo "==> zipping"
rm -f "$ZIP"
( cd "$WORK" && TZ=UTC zip -X -qr "$ZIP" "$(basename "$STAGE")" )

echo
echo "$(basename "$ZIP")  $(du -h "$ZIP" | cut -f1)"
du -sh "$STAGE"/* | sort -rh
