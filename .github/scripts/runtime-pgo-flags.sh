#!/bin/sh
# runtime-pgo-flags.sh <absolute build dir> -- prints the compiler/linker flags
# that apply the runtime's GCC profile, or nothing when there is no profile
# (or RINGOUT_RUNTIME_PGO=0), in which case the build is plain LTO as before.
#
# The profile is ModernGekko/pgo/runtime-linux: GCC .gcda files from a training
# run (.github/scripts/train-runtime-pgo.sh), named relative to the build dir.
# -fprofile-prefix-path makes GCC look them up relative to THIS build dir, so the
# same files work in build-deck, in CI's checkout, or anywhere else. Verified: a
# build in a different directory is byte-identical to the one measured.
#
# Why each flag:
#   -fprofile-partial-training  training runs windowed on Vulkan, so the other
#       renderers and the Null backend are untrained; without this, GCC would
#       treat them as cold.
#   -Wno-missing-profile  REQUIRED, not cosmetic: CMake's flag checks compile
#       test files that have no profile; the warning makes them fail, and the
#       build silently loses -fno-strict-aliasing, -msse2, -fno-exceptions...
#   -Wno-error=coverage-mismatch  after a runtime source change, GCC ignores the
#       stale profile for the changed functions instead of failing the build.
#       Retrain (train-runtime-pgo.sh) to get the full gain back.
#   -fprofile-correction  counters from a multithreaded training run.
# GCC-only, and tied to the compiler that trained it (Debian 12's gcc 12.2, the
# container build-deck.sh and deck.yml both use).
set -eu
BUILD="${1:?usage: runtime-pgo-flags.sh <absolute build dir>}"
HERE="$(cd "$(dirname "$0")/../.." && pwd)"
PROF="$HERE/ModernGekko/pgo/runtime-linux"
[ "${RINGOUT_RUNTIME_PGO:-1}" = 0 ] && exit 0
ls "$PROF"/*.gcda >/dev/null 2>&1 || exit 0
printf '%s' "-fprofile-use=$PROF -fprofile-prefix-path=$BUILD -fprofile-partial-training -fprofile-correction -Wno-missing-profile -Wno-error=coverage-mismatch"
