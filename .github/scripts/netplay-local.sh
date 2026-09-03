#!/bin/bash
# Run two local netplay instances against each other and report whether they
# stayed in sync.
#
# $1 = work dir   $2 = seconds to stay in-game (default 60)   $3 = port
#
# Both peers get:
#   * the memory card, or the game parks on "No previous SOULCALIBUR II data
#     found ..." forever and the session measures a stalled emulator;
#   * BackgroundInput=True, since Dolphin drops all input -- pipe included --
#     when the render window lacks focus, and neither of these has focus;
#   * a Pipe pad device with a writer held open for the session, because
#     Dolphin opens the FIFO once and never recovers from EOF.
# See the "driving the game" notes: each of those alone is enough to make a
# session look broken for reasons unrelated to netplay.
#
# Env:
#   HASH=1        diff per-frame guest-RAM hashes afterwards (see below)
#   WINDOWED=1    give both peers a real window instead of running headless.
#                 The lobby still auto-starts on --netplay-players 2, so this
#                 needs no clicking -- it is how the in-game overlay gets
#                 inspected inside a live session. Raise TIMEOUT with it: the
#                 90 s default expires while you are still looking at the
#                 lobby, and both peers then log "lobby closed".
#   OVERCLOCK=f   preseed a CPU overclock factor on both peers before the
#                 lobby runs. RunNetplayLobby is supposed to force stock clock
#                 on every peer, so this is the test of it: set 3.0 here and
#                 the session should still come out at 1.0.
#   TIMEOUT=s     lobby timeout per peer (default 90)
#   PKG=dir       package the peers run from (default dist/RingOut-1.0-deck)
#   RUNTIME=path  emulator to run (default ./bin/moderngekko-run inside PKG)
#   GAME=path     extracted disc root (default ./game inside PKG)
#   MODULE=path   recompiled module (default ./bin/gGRSEAF_recomp.so in PKG)
#   SAVE=dir      memory-card dir copied into each peer (default PKG/userdata/GC)
#
# The last four exist so this can be pointed at a disc that is not the US one.
# Both peers cd into PKG first, so a RELATIVE value resolves there -- pass
# absolute paths for anything outside it. GAME and MODULE must agree: the
# runtime refuses a module whose disc ID is not the disc's. SAVE matters more
# than it looks -- each disc reads its OWN card file (the JP disc wants
# AF-GRSJ, PAL AF-GRSP, the Plus mod PS-GRSE), and a disc that finds no save of
# its own parks on the memory-card dialog forever. Two peers agree perfectly on
# a dialog, so that is a green hash over a session that played nothing.
set -u
P="${P:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
W="${1:-/tmp/netplay-local}"
# Absolute, because each peer runs `cd "$PKG"` before exec: a relative work dir
# then resolves under the PACKAGE, the determinism log fopen()s a directory that
# does not exist, and the hash comparison below is skipped rather than failed --
# the run still prints "no desync reported" and looks like a pass. That happened.
case "$W" in /*) ;; *) W="$PWD/$W" ;; esac
PLAY="${2:-60}"
PORT="${3:-2626}"
PKG="${PKG:-$P/dist/RingOut-1.0-deck}"
RUNTIME="${RUNTIME:-./bin/moderngekko-run}"
GAME="${GAME:-./game}"
MODULE="${MODULE:-./bin/gGRSEAF_recomp.so}"
SAVE="${SAVE:-$PKG/userdata/GC}"
TIMEOUT="${TIMEOUT:-90}"

pre="$(pgrep -x moderngekko-run 2>/dev/null | wc -l)"
if [ "$pre" != "0" ]; then
  echo "ABORT: $pre emulator(s) already running (pkill -9 -x moderngekko-run)"
  exit 1
fi

rm -rf "$W"; mkdir -p "$W"

setup_peer() {
  local name="$1"
  local d="$W/$name"
  mkdir -p "$d/user/Config" "$d/user/Pipes"
  cp -r "$SAVE" "$d/user/" 2>/dev/null || true
  mkfifo "$d/user/Pipes/ctrl"
  cat > "$d/user/Config/GCPadNew.ini" <<'EOF'
[GCPad1]
Device = Pipe/0/ctrl
Buttons/A = `Button A`
Buttons/B = `Button B`
Buttons/X = `Button X`
Buttons/Y = `Button Y`
Buttons/Z = `Button Z`
Buttons/Start = `Button START`
D-Pad/Up = `Button D_UP`
D-Pad/Down = `Button D_DOWN`
D-Pad/Left = `Button D_LEFT`
D-Pad/Right = `Button D_RIGHT`
Triggers/L = `Button L`
Triggers/R = `Button R`
Main Stick/Up = `Axis MAIN Y -`
Main Stick/Down = `Axis MAIN Y +`
Main Stick/Left = `Axis MAIN X -`
Main Stick/Right = `Axis MAIN X +`
EOF
  # OVERCLOCK preseeds a factor the lobby is expected to override, so that
  # "netplay forces stock clock" can be tested rather than assumed.
  if [ -n "${OVERCLOCK:-}" ]; then
    printf '[Core]\nOverclock = %s\nOverclockEnable = True\n' "$OVERCLOCK" \
      > "$d/user/Config/Dolphin.ini"
    printf '[Input]\nBackgroundInput = True\n' >> "$d/user/Config/Dolphin.ini"
  else
    printf '[Input]\nBackgroundInput = True\n' > "$d/user/Config/Dolphin.ini"
  fi
  setsid bash -c 'exec 9<>"'"$d"'/user/Pipes/ctrl"; exec sleep 7200' \
    >/dev/null 2>&1 &
  echo $! > "$d/holder.pid"
}

setup_peer host
setup_peer guest

# HASH=1 additionally runs the determinism harness on both peers and diffs the
# per-frame guest-RAM hashes afterwards. Dolphin's own desync detection compares
# only the emulated timebase, once every 60 frames; this compares all of RAM
# every frame, so it is the difference between "the clocks agree" and "the two
# machines are in the same state". It costs a 24 MB hash per frame, hence opt-in.
launch() {
  local name="$1"; shift
  local d="$W/$name"
  local -a hash_env=()
  if [ "${HASH:-0}" = "1" ]; then
    hash_env=(RINGOUT_DETERMINISM_LOG="$d/hash.log")
  fi
  local -a mode=(--headless)
  [ "${WINDOWED:-0}" = "1" ] && mode=()
  ( cd "$PKG" && exec env "${hash_env[@]}" "$RUNTIME" "${mode[@]}" \
      --user-dir "$d/user" --game "$GAME" --module "$MODULE" \
      --controller "Standard Controller" "$@" ) > "$d/log.txt" 2>&1 &
  echo $! > "$d/pid"
  echo "$name pid=$(cat "$d/pid")"
}

launch host  --netplay-host --netplay-port "$PORT" --netplay-players 2 \
             --nickname Host  --buffer 5 --netplay-timeout "$TIMEOUT"
sleep 4
launch guest --netplay-join 127.0.0.1 --netplay-port "$PORT" \
             --nickname Guest --netplay-timeout "$TIMEOUT"

# Wait for both to report booting, then let them run.
# "netplay armed" is the load-bearing string, not "booting". NetPlay_Enable
# happens inside NetPlayClient::StartGame; without it SI reads local pads and
# desync detection never arms, so two independent single-player sessions run to
# completion and report no desync -- a clean-looking pass that checked nothing.
waited=0
while [ $waited -lt 100 ]; do
  if grep -qa "netplay armed" "$W/host/log.txt" 2>/dev/null && \
     grep -qa "netplay armed" "$W/guest/log.txt" 2>/dev/null; then
    echo "both peers booted after ${waited}s"
    break
  fi
  # Bail out early on a lobby failure rather than burning the whole timeout.
  if grep -qa "no start signal\|timed out\|refusing to start\|could not connect\|did not arm\|refused to start" \
       "$W/host/log.txt" "$W/guest/log.txt" 2>/dev/null; then
    echo "lobby failed:"
    grep -ha "netplay:" "$W/host/log.txt" "$W/guest/log.txt" | tail -12
    break
  fi
  sleep 2; waited=$((waited + 2))
done

if grep -qa "netplay armed" "$W/host/log.txt" 2>/dev/null; then
  echo "running for ${PLAY}s ..."
  slept=0
  while [ $slept -lt "$PLAY" ]; do
    sleep 5; slept=$((slept + 5))
    if grep -qa "DESYNC" "$W/host/log.txt" "$W/guest/log.txt" 2>/dev/null; then
      echo "desync detected after ${slept}s in-game"
      break
    fi
  done
fi

for name in host guest; do
  d="$W/$name"
  [ -f "$d/pid" ] && kill "$(cat "$d/pid")" 2>/dev/null
done
sleep 2
for name in host guest; do
  d="$W/$name"
  [ -f "$d/pid" ] && kill -9 "$(cat "$d/pid")" 2>/dev/null
  [ -f "$d/holder.pid" ] && kill -9 "$(cat "$d/holder.pid")" 2>/dev/null
done
sleep 1
left="$(pgrep -x moderngekko-run 2>/dev/null | wc -l)"
[ "$left" != "0" ] && echo "WARNING: $left emulator(s) still alive"

echo
echo "================ HOST ================"
grep -a "netplay:\|fmv-hle\|staticrecomp. shutdown" "$W/host/log.txt" 2>/dev/null
echo "================ GUEST ==============="
grep -a "netplay:\|fmv-hle\|staticrecomp. shutdown" "$W/guest/log.txt" 2>/dev/null
echo "======================================"
hash_verdict=""
if [ "${HASH:-0}" = "1" ] && { [ ! -s "$W/host/hash.log" ] || [ ! -s "$W/guest/hash.log" ]; }; then
  # HASH=1 asked for the strong check. Not getting it is a failed run, not a
  # quiet downgrade to the DESYNC grep -- which only catches what Dolphin itself
  # noticed, and is exactly the weaker claim this flag exists to avoid.
  echo "HASH=1 but no per-frame hashes were written:"
  [ -s "$W/host/hash.log" ]  || echo "  host:  $W/host/hash.log missing or empty"
  [ -s "$W/guest/hash.log" ] || echo "  guest: $W/guest/hash.log missing or empty"
  hash_verdict="NOT RUN"
elif [ "${HASH:-0}" = "1" ]; then
  # Compare only the frames both peers reached; one is always killed a moment
  # before the other, and a length difference is not a state difference.
  n=$(( $(wc -l < "$W/host/hash.log") < $(wc -l < "$W/guest/hash.log") \
        ? $(wc -l < "$W/host/hash.log") : $(wc -l < "$W/guest/hash.log") ))
  head -n "$n" "$W/host/hash.log"  > "$W/host.trim"
  head -n "$n" "$W/guest/hash.log" > "$W/guest.trim"
  echo "guest-RAM hash comparison over $n frames:"
  if diff -q "$W/host.trim" "$W/guest.trim" >/dev/null; then
    echo "  IDENTICAL on every frame"
    hash_verdict="IDENTICAL"
  else
    echo "  FIRST DIVERGENCE:"
    diff "$W/host.trim" "$W/guest.trim" | head -4
    hash_verdict="DIVERGED"
  fi
fi

# Exit code as well as a line of text, so a caller that is not reading the
# output cannot mistake any of this for success.
if grep -qa "DESYNC" "$W/host/log.txt" "$W/guest/log.txt" 2>/dev/null; then
  echo "RESULT: DESYNCED"
  exit 1
elif [ "$hash_verdict" = "DIVERGED" ]; then
  echo "RESULT: DESYNCED (per-frame hashes diverged; Dolphin did not notice)"
  exit 1
elif ! grep -qa "netplay armed" "$W/host/log.txt" 2>/dev/null || \
     ! grep -qa "netplay armed" "$W/guest/log.txt" 2>/dev/null; then
  echo "RESULT: session did not start"
  exit 1
elif [ "$hash_verdict" = "NOT RUN" ]; then
  echo "RESULT: INCONCLUSIVE -- peers ran netplay-armed, but the hash check asked"
  echo "        for with HASH=1 never ran, so this is not the check you wanted"
  exit 1
elif [ "$hash_verdict" = "IDENTICAL" ]; then
  echo "RESULT: both peers ran netplay-armed, hashes identical on every frame"
else
  echo "RESULT: both peers ran netplay-armed with no desync reported"
fi
