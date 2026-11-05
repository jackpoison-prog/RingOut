#!/bin/bash
# macos/launcher.c, the Mach-O entry point of RingOut.app (Contents/MacOS/RingOut).
# The logic is plain POSIX, so it is built and run here against a fake Ring Out
# folder whose "runtime" is a stub that records how it was started.
#
#   bash dist/RingOut-1.0-dist/tests/test_macos_launcher.sh
set -u
DIST="$(cd "$(dirname "$0")/.." && pwd)"
fail=0
check() { # name expected actual
    if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: want '$2', got '$3'"; fail=1; fi
}
# Resolved: the launcher resolves its own path (as the script's `pwd -P` did),
# and on a Mac the temp dir is under /var, a link to /private/var.
T="$(cd "$(mktemp -d)" && pwd -P)"; trap 'rm -rf "$T"' EXIT
ROOT="$T/Ring Out"
MACOS="$ROOT/RingOut.app/Contents/MacOS"
mkdir -p "$MACOS"
# The alert is CoreFoundation's on a Mac; elsewhere it compiles out.
[ "$(uname)" = Darwin ] && FRAMEWORKS=(-framework CoreFoundation) || FRAMEWORKS=()
${CC:-cc} -O2 -Wall -Wextra -Werror -o "$MACOS/RingOut" "$DIST/macos/launcher.c" "${FRAMEWORKS[@]}" \
    || { echo "FAIL compile"; exit 1; }
cat > "$MACOS/moderngekko-run" <<'SH'
#!/bin/sh
printf '%s\n' "$@" > "$STUB_ARGS"
echo "runtime stdout"; echo "runtime stderr" >&2
exit 7
SH
chmod +x "$MACOS/moderngekko-run"
export STUB_ARGS="$T/args" RINGOUT_LAUNCHER_NO_ALERT=1

# ---- not set up: no module, no game/ -> refuses, runtime never started
(cd / && "$MACOS/RingOut" 2>/dev/null); rc=$?
check "not set up exits 1"         "1" "$rc"
check "not set up starts nothing"  "no" "$([ -e "$STUB_ARGS" ] && echo yes || echo no)"

# ---- set up
mkdir -p "$ROOT/game" "$ROOT/bin"; : > "$ROOT/bin/gGRSEAF_recomp.dylib"
(cd / && "$MACOS/RingOut" --fullscreen -psn_0_12345); rc=$?
check "runtime's exit code"        "7" "$rc"
want="--user-dir
$ROOT/userdata
--game
$ROOT/game
--module
$ROOT/bin/gGRSEAF_recomp.dylib
--fullscreen"
check "arguments (paths with a space, -psn_ dropped)" "$want" "$(cat "$STUB_ARGS")"
check "log gets stdout and stderr" "runtime stdout
runtime stderr" "$(cat "$ROOT/userdata/launch.log")"

# ---- RINGOUT_LOG_NAME, and started through a symlink
ln -s "$MACOS/RingOut" "$T/link"
(cd / && RINGOUT_LOG_NAME=other.log "$T/link"); rc=$?
check "through a symlink"          "7" "$rc"
check "RINGOUT_LOG_NAME"           "yes" "$([ -s "$ROOT/userdata/other.log" ] && echo yes || echo no)"

exit $fail
