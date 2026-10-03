#!/bin/bash
# Builds the Linux runtime against an OLD glibc, inside a container, so the
# result runs on SteamOS / Steam Deck.
#
# Binaries run FORWARD across glibc versions but never backward. This machine
# is glibc 2.44 and SteamOS is ~2.37, so anything built natively simply will
# not start on the Deck. Debian 12 (glibc 2.36) is below the Deck and below
# essentially every current distro, so one build covers all of them -- and
# replaces the libc-fallback bundling hack, which was never verified on real
# SteamOS and whose predicted failure mode is NSS.
#
# Steam Runtime sniper would be the more obvious base (glibc 2.31) but ships
# gcc 10 / clang 11, far too old for this codebase.
#
# Run from the repo root:  .github/scripts/build-deck.sh
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
IMAGE="ringout-deck-build"

echo "==> building the toolchain image"
# The package list lives in deck-deps.txt because CI installs the same set into
# a debian:12 job container; see the header there.
DEPS="$(grep -v '^#' "$REPO/.github/scripts/deck-deps.txt" | grep -v '^[[:space:]]*$' | tr '\n' ' ')"
podman build -t "$IMAGE" -f - "$REPO" <<DOCKERFILE
FROM debian:12
RUN apt-get update && apt-get install -y --no-install-recommends \\
        $DEPS \\
    && rm -rf /var/lib/apt/lists/*
DOCKERFILE

echo
echo "==> configuring"
# ENABLE_LTO (Dolphin's option, off by default): the runtime is about a quarter
# of the CPU time in gameplay and had never been built with it. Measured on the
# Steam Deck with the same module, 16000 frames of the arcade route, n=8 each:
# -2.70% of ALL cycles (the runtime roughly 10% cheaper), frame hash unchanged,
# binary 28.1 -> 19.3 MB, glibc floor still 2.36. The deck.yml job matches.
podman run --rm --userns=keep-id -v "$REPO:/src:Z" -w /src "$IMAGE" bash -c '
  set -e
  # Guarded: an image built before ccache joined deck-deps.txt still works,
  # it just rebuilds everything.
  LAUNCH=""
  command -v ccache >/dev/null && \
    LAUNCH="-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache"
  # Runtime PGO on top of LTO when the committed profile is present (see
  # runtime-pgo-flags.sh): a further -4.51% of all cycles on the Steam Deck.
  PGO="$(/src/.github/scripts/runtime-pgo-flags.sh /src/build-deck)"
  cmake -S ModernGekko -B build-deck -GNinja \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_LTO=ON \
    -DCMAKE_C_FLAGS="$PGO" -DCMAKE_CXX_FLAGS="$PGO" -DCMAKE_EXE_LINKER_FLAGS="$PGO" \
    -DENABLE_QT=OFF \
    -DENABLE_TESTS=OFF \
    -DENABLE_ANALYTICS=OFF \
    -DENABLE_AUTOUPDATE=OFF $LAUNCH
'

echo
echo "==> building moderngekko-run"
podman run --rm --userns=keep-id -v "$REPO:/src:Z" -w /src "$IMAGE" \
  cmake --build build-deck --target moderngekko-run

echo
echo "==> result"
podman run --rm --userns=keep-id -v "$REPO:/src:Z" -w /src "$IMAGE" bash -c '
  echo "glibc required:  $(objdump -T build-deck/moderngekko-run | grep -o "GLIBC_[0-9.]*" | sort -uV | tail -1)"
  echo "GLIBCXX required: $(objdump -T build-deck/moderngekko-run | grep -o "GLIBCXX_[0-9.]*" | sort -uV | tail -1)"
  ls -la build-deck/moderngekko-run
'
