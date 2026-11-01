#!/bin/bash
# Checks setup.sh's per-architecture CPU flag: x86 keeps -march (native, or
# x86-64-v3 for --deck), Apple Silicon / ARM Linux get -mcpu=native, and --deck
# is refused on ARM because a Steam Deck is x86. Also the macOS branch: module
# suffix, the US-only disc rule, where the runtime lives, and the CPU count.
#
#   bash dist/RingOut-1.0-dist/tests/test_setup_arch.sh
set -u
HERE="$(cd "$(dirname "$0")/.." && pwd)"
RINGOUT_SETUP_LIB=1 source "$HERE/setup.sh"
set +eu +o pipefail   # setup.sh turns these on; this script checks exit codes itself
fail=0
check() { # name expected actual
    if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: want '$2', got '$3'"; fail=1; fi
}
check "x86_64 desktop" "-march=native"    "$(pick_cpu_flags x86_64 0)"
check "x86_64 deck"    "-march=x86-64-v3" "$(pick_cpu_flags x86_64 1)"
check "arm64 (macOS)"  "-mcpu=native"     "$(pick_cpu_flags arm64 0)"
check "aarch64 linux"  "-mcpu=native"     "$(pick_cpu_flags aarch64 0)"
msg="$(pick_cpu_flags arm64 1 2>&1)"; rc=$?
check "arm64 --deck exit code" "2" "$rc"
check "arm64 --deck message" "--deck is for the Steam Deck (x86) only" "$msg"
check "module suffix linux"  ".so"    "$(module_suffix Linux)"
check "module suffix macOS"  ".dylib" "$(module_suffix Darwin)"
mac_disc_ok Linux GRSJAF;  check "JP disc on linux" "0" "$?"
mac_disc_ok Darwin GRSEAF; check "US disc on macOS" "0" "$?"
msg="$(mac_disc_ok Darwin GRSJAF 2>&1)"; rc=$?
check "JP disc on macOS refused" "1" "$rc"
check "JP disc on macOS message" "This Mac build supports the US disc (GRSEAF) for now; this disc is GRSJAF." "$msg"
check "runtime path linux" "/p/bin/moderngekko-run" "$(runtime_path Linux /p)"
check "runtime path macOS" "/p/RingOut.app/Contents/MacOS/moderngekko-run" "$(runtime_path Darwin /p)"
n="$(cpu_count)"; case "$n" in ''|*[!0-9]*) n=bad ;; esac
check "cpu count is a number" "yes" "$([ "$n" != bad ] && [ "$n" -ge 1 ] && echo yes)"
exit $fail
