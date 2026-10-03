#!/bin/bash
# train-runtime-pgo.sh <module.so> -- retrain the runtime's GCC profile
# (ModernGekko/pgo/runtime-linux). Developer-only: it needs the US disc and save
# data under private-artifacts/, which CI does not have -- that is why the
# profile is committed rather than generated in the workflow.
#
# Run it after runtime source changes (a stale profile is safe, just weaker:
# see runtime-pgo-flags.sh). ~15 minutes on a desktop:
#   1. instrumented LTO build in build-rtpgo-train (the Debian 12 container, so
#      the compiler matches the one that consumes the profile)
#   2. training on THIS machine, WINDOWED with Vulkan -- the way players and the
#      Deck run it: 16000 frames of the arcade route and 14000 of the VS
#      stage-10 route. A window opens and plays itself; leave it alone. The
#      instrumented binary writes under /src (the container's mount), so on the
#      host GCOV_PREFIX_STRIP=1 + GCOV_PREFIX=<repo> put the .gcda files in the
#      same place. Windowed Vulkan training measured -0.94% cycles in windowed
#      play against the old headless (Null renderer) training (2026-09-30,
#      desktop, 8000 frames, n=5, no overlap). Needs a desktop session.
#   3. (unless DRY_RUN=1) the .gcda files replace ModernGekko/pgo/runtime-linux (rsync --delete, so
#      files of objects that no longer exist do not linger).
# Then rebuild with build-deck.sh and measure before committing.
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
MOD="$(readlink -f "${1:?usage: train-runtime-pgo.sh <module.so built from the same tree>}")"
case "$MOD" in "$REPO"/*) ;; *) echo "the module must live under $REPO (it is mounted as /src)"; exit 2;; esac
IMAGE=ringout-deck-build
BUILD=build-rtpgo-train
STAMP=$(date +%Y%m%d-%H%M%S)
OUT="$REPO/work/rtpgo-train/$STAMP"   # fresh each run: GCC ADDS to existing .gcda files
INST=private-artifacts/relcheck-1.6.1/RingOut-1.6.1-linux
[ -d "$REPO/$INST/game" ] || { echo "no US game at $INST"; exit 2; }
mkdir -p "$OUT/gcda"
GEN="-fprofile-generate=/src/work/rtpgo-train/$STAMP/gcda -fprofile-prefix-path=/src/$BUILD -fprofile-update=atomic"
C() { podman run --rm --userns=keep-id --shm-size=2g -v "$REPO:/src:Z" -w /src "$IMAGE" bash -c "$1"; }

echo "==> instrumented build"
C "cmake -S ModernGekko -B $BUILD -GNinja -DCMAKE_BUILD_TYPE=Release -DENABLE_LTO=ON -DENABLE_QT=OFF \
     -DENABLE_TESTS=OFF -DENABLE_ANALYTICS=OFF -DENABLE_AUTOUPDATE=OFF \
     -DCMAKE_C_FLAGS='$GEN' -DCMAKE_CXX_FLAGS='$GEN' -DCMAKE_EXE_LINKER_FLAGS='$GEN' >/dev/null &&
   cmake --build $BUILD --target moderngekko-run >/dev/null"

echo "==> training (windowed, Vulkan)"
for spec in "arcade|.github/input-scripts/arcade-match.txt|16000" \
            "vs|.github/input-scripts/vs-stage10.txt|14000"; do
    IFS='|' read -r name route frames <<< "$spec"
    U="$REPO/work/rtpgo-train/$STAMP/user-$name"
    mkdir -p "$U/Config" && cp -r "$REPO/$INST/userdata/GC" "$U/" && cp "$REPO/$INST/userdata/Config/"*.ini "$U/Config/"
    GCOV_PREFIX="$REPO" GCOV_PREFIX_STRIP=1 \
      RINGOUT_DETERMINISM_LOG="$U/frames.log" RINGOUT_DETERMINISM_FRAMES=$frames RINGOUT_DETERMINISM_NOHASH=1 \
      RINGOUT_DETERMINISM_INPUT="$REPO/$route" "$REPO/$BUILD/moderngekko-run" --graphics Vulkan --user-dir "$U" \
      --game "$REPO/$INST/game" --module "$MOD" > "$U/run.out" 2>&1
    got=$(wc -l < "$U/frames.log" 2>/dev/null || echo 0)
    echo "    $name: $got / $frames frames, $(grep -m1 'renderer' "$U/run.out")"
    [ "$got" = "$frames" ] || { echo "training run $name fell short -- see $U/run.out"; exit 1; }
    grep -q 'renderer: Vulkan' "$U/run.out" || { echo "training run $name did not use Vulkan -- see $U/run.out"; exit 1; }
done

n=$(ls "$OUT/gcda"/*.gcda 2>/dev/null | wc -l)
[ "$n" -gt 100 ] || { echo "only $n .gcda files -- training did not record"; exit 1; }
if [ "${DRY_RUN:-0}" = 1 ]; then echo "==> DRY_RUN: $n profile files left in $OUT/gcda"; exit 0; fi
rsync -a --delete "$OUT/gcda/" "$REPO/ModernGekko/pgo/runtime-linux/"
echo "==> $n profile files -> ModernGekko/pgo/runtime-linux (rebuild with build-deck.sh and measure)"
