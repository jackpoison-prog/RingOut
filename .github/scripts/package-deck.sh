#!/bin/bash
# Assembles the Steam Deck / SteamOS release: the runtime built against an old
# glibc, the support libraries that go with it, and the scaffolding -- zipped.
#
# This package ships NO game data, NO save data and NO recompiled module. The
# module is derived from the user's own disc and cannot be redistributed, so
# they build it once on a desktop and copy game/ and bin/g<ID>_recomp.so across
# (README.txt says so, and the launcher checks for both). That is why the stage
# is built file by file from an allowlist rather than by copying
# dist/RingOut-1.0-deck: the working copy of that directory on a developer
# machine holds ~1 GB of extracted disc, the player's memory card and the
# module, and a recursive copy would quietly ship all three.
#
#   ./.github/scripts/package-deck.sh
#
# Env:
#   RUNTIME=path  package an already-built runtime instead of building one.
#                 It must still be a Debian 12 build -- the floor is checked.
#   OUT=dir       output directory (default dist/)
#   SKIP_BUILD=1  reuse build-deck/moderngekko-run from a previous run
#   NATIVE=1      we are ALREADY on Debian 12, so run the inspection steps here
#                 instead of in a container. This is the CI path: the workflow
#                 job runs in a debian:12 container and has no podman. Do not
#                 use it on a developer machine -- ldd would then resolve the
#                 HOST's libraries, and a package of Arch libraries has a glibc
#                 floor far above SteamOS and would not start on a Deck.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
IMAGE="ringout-deck-build"
OUT="${OUT:-$REPO/dist}"
# The RELEASE version. It names the zip and the folder inside it -- NOT the
# working package directory, which keeps its historical RingOut-1.0-* name:
# that is 1.3 GB of developer state referenced by .gitignore and a dozen
# scripts, and renaming it would churn all of them for nothing a player sees.
VERSION="${VERSION:-1.3}"
PKG="RingOut-1.0-deck"
SRC="$REPO/dist/$PKG"

# Staging happens in its own directory. Never stage into dist/RingOut-1.0-deck:
# that is the live working package, and this script would delete it.
WORK="$OUT/_deck-stage"
STAGE="$WORK/RingOut-$VERSION-deck"
ZIP="$OUT/RingOut-$VERSION-steamdeck-x86_64.zip"

case "$STAGE" in
  "$SRC"|"$SRC"/*) echo "refusing to stage into the working package ($SRC)" >&2; exit 1 ;;
esac

# Run a snippet where Debian 12's linker and libraries are the ones in scope.
# The container mounts are at IDENTICAL paths, so a snippet reads the same
# either way and nothing has to translate /src-style prefixes.
box() {
  if [ "${NATIVE:-0}" = "1" ]; then
    bash -c "$1"
  else
    podman run --rm --userns=keep-id \
      -v "$REPO:$REPO:Z" -v "$WORK:$WORK:Z" -w "$REPO" "$IMAGE" bash -c "$1"
  fi
}

# --- the runtime ----------------------------------------------------------
# This package's payload IS the prebuilt runtime -- unlike the desktop one, the
# player compiles nothing. So a stale binary here ships silently: it passes the
# glibc floor, the forbidden-file list and the privacy scan, because none of
# those look at age.
#
# That is not hypothetical. This script used to rebuild only when the runtime
# was ABSENT, so on 2026-08-12 it packaged a three-day-old binary missing a
# merged chassis change, reported success, and every check passed. The desktop
# path already refuses to package a runtime older than its ABI header
# (package-dist.sh); this is the same guard for the package where it matters
# more.
RUNTIME_EXPLICIT=0
[ -n "${RUNTIME:-}" ] && RUNTIME_EXPLICIT=1
RUNTIME="${RUNTIME:-$REPO/build-deck/moderngekko-run}"

# Newest source wins: if anything the runtime is built from is newer than the
# runtime, the binary does not contain it. mtime rather than a hash because the
# build is out-of-tree and there is nothing to compare against.
newer_source_than_runtime() {
  [ -f "$RUNTIME" ] || return 0
  find "$REPO/ModernGekko/src" "$REPO/ModernGekko/tools" \
       "$REPO/ModernGekko/vendor/dolphin/Source" "$REPO/ModernGekko/CMakeLists.txt" \
       -newer "$RUNTIME" -type f -print -quit 2>/dev/null | grep -q .
}

if [ "${SKIP_BUILD:-0}" != "1" ] && [ "$RUNTIME_EXPLICIT" != "1" ]; then
  if [ ! -f "$RUNTIME" ]; then
    echo "==> building the runtime (no $RUNTIME yet)"
    "$REPO/.github/scripts/build-deck.sh"
  elif newer_source_than_runtime; then
    echo "==> runtime is older than the sources -- rebuilding"
    "$REPO/.github/scripts/build-deck.sh"
  fi
fi

[ -f "$RUNTIME" ] || { echo "no runtime at $RUNTIME" >&2; exit 1; }

# Assert even after building, and assert for the override paths too: SKIP_BUILD
# and RUNTIME= are for packaging a runtime built elsewhere (CI), not a licence
# to ship an old one. ALLOW_STALE_RUNTIME=1 is the deliberate escape hatch.
if [ "${ALLOW_STALE_RUNTIME:-0}" != "1" ] && newer_source_than_runtime; then
  echo "  FAIL: $RUNTIME is older than the sources it is built from." >&2
  echo "        newer: $(find "$REPO/ModernGekko/src" "$REPO/ModernGekko/tools" \
                              "$REPO/ModernGekko/vendor/dolphin/Source" \
                              "$REPO/ModernGekko/CMakeLists.txt" \
                              -newer "$RUNTIME" -type f -print -quit 2>/dev/null)" >&2
  echo "        Rebuild with .github/scripts/build-deck.sh, or set" >&2
  echo "        ALLOW_STALE_RUNTIME=1 if this is deliberate." >&2
  exit 1
fi
echo "==> runtime: $RUNTIME"

rm -rf "$WORK"
mkdir -p "$STAGE/bin" "$STAGE/lib" "$STAGE/shaders" "$STAGE/tools"

# --- support libraries ----------------------------------------------------
# Computed, not hand-maintained. The rule is the runtime's ldd closure minus
# the three the target OS must always provide itself: libc, libm and libpthread
# are the C library, and a Debian 12 copy of those cannot be dropped onto
# SteamOS. Everything else is fair game because the LAUNCHER decides at run time
# which of these to actually use -- it asks ldconfig what the host already has
# and links only the remainder into userdata/lib-fallback. That indirection is
# load-bearing: putting all 66 on LD_LIBRARY_PATH shadowed SteamOS's own copies
# for the whole process, including the ones Mesa dlopens, and no Vulkan driver
# would load.
echo "==> collecting support libraries"
box '
    set -e
    ldd "'"$RUNTIME"'" | awk "/=>/ && \$3 ~ /^\// {print \$1, \$3}" |
    while read -r soname path; do
      case "$soname" in
        libc.so.6|libm.so.6|libpthread.so.0) continue ;;
      esac
      # -L because the closure names sonames but the files are usually symlinks
      # to a versioned real name; the package wants the real content under the
      # soname the loader will ask for.
      cp -L "$path" "'"$STAGE"'/lib/$soname"
    done
  '
echo "    $(ls "$STAGE/lib" | wc -l) libraries"

# --- binaries and scaffolding --------------------------------------------
echo "==> scaffolding"
install -m 755 "$RUNTIME"        "$STAGE/bin/moderngekko-run"
install -m 755 "$SRC/RingOut"    "$STAGE/RingOut"
# Same extractor the desktop package ships. The Deck package has no setup.sh --
# it is prebuilt -- so the launcher runs this the first time a game is present.
install -m 755 "$REPO/dist/shared/gc-art.py" "$STAGE/tools/gc-art.py"
install -m 644 "$SRC/README.txt" "$STAGE/README.txt"
# The version line is stamped at stage time rather than maintained by hand.
# The 1.2.1 release shipped a README whose first line read "Ver 1.1" because
# this was a verbatim copy and nobody edits a header twice.
sed -i "1s/^Ring Out - Ver .*/Ring Out - Ver $VERSION/" "$STAGE/README.txt"
install -m 644 "$SRC/CREDITS.txt" "$STAGE/CREDITS.txt"
for f in "$SRC"/shaders/*.glsl; do install -m 644 "$f" "$STAGE/shaders/"; done

# --- checks ---------------------------------------------------------------
# A package that ships the disc, the save card or the module is the failure
# that matters here, so assert it rather than trusting the allowlist above.
echo "==> checks"
# art/ holds the game's own banner and icon, extracted from the player's own
# disc at run time. Fine to generate locally, never to ship.
for forbidden in game userdata bin/gGRSEAF_recomp.so art; do
  [ -e "$STAGE/$forbidden" ] && { echo "  FAIL: $forbidden is in the stage" >&2; exit 1; }
done
if find "$STAGE" -name '*_recomp.so' -o -name '*.gci' -o -name '*.iso' | grep -q .; then
  echo "  FAIL: disc-derived files in the stage" >&2; exit 1
fi

floor=$(box 'objdump -T "'"$STAGE"'/bin/moderngekko-run" | grep -o "GLIBC_[0-9.]*" | sort -uV | tail -1')
echo "  glibc floor: $floor"
case "$floor" in
  GLIBC_2.3[0-6]|GLIBC_2.2*|GLIBC_2.1*) ;;
  *) echo "  FAIL: $floor is above SteamOS's ~2.37 -- this will not start on a Deck" >&2; exit 1 ;;
esac

# The checks above are the copyright axis -- disc, save card, module. Privacy is
# a separate axis they do not cover: a stage can be entirely free of game data
# and still carry the builder's home directory baked into a generated file, LAN
# addressing in a config, or an author email inside a source patch shipped for
# GPL compliance. Run against the STAGE, never the working package -- that one
# legitimately holds the disc, the save card and a config naming the LAN.
"$REPO/.github/scripts/privacy-scan.sh" "$STAGE"

# --- zip ------------------------------------------------------------------
# Info-ZIP stores unix modes and unzip restores them. That matters more than it
# looks: RingOut and bin/moderngekko-run arrive non-executable otherwise and the
# package fails with "permission denied" on a player's Deck, so the unpack below
# is a canary rather than a formality.
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

verify="$WORK/_verify"
mkdir -p "$verify"
unzip -qq "$ZIP" -d "$verify"
# Check the extracted tree by the name it actually has, not by $PKG: the stage
# is named for the RELEASE version while $PKG is the working directory, and
# assuming they match silently checked a path that did not exist.
unpacked="$verify/$(basename "$STAGE")"
[ -x "$unpacked/RingOut" ] || { echo "  FAIL: RingOut is not executable after unzip" >&2; exit 1; }
[ -x "$unpacked/bin/moderngekko-run" ] || { echo "  FAIL: runtime is not executable after unzip" >&2; exit 1; }
rm -rf "$verify"

echo
echo "$(basename "$ZIP")  $(du -h "$ZIP" | cut -f1)"
( cd "$STAGE" && du -sh -- * | sort -h )
