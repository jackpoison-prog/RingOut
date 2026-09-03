#!/bin/bash
# Drive two netplay peers all the way into a VS Battle with scripted input, then
# verify they stayed in sync through it.
#
# This is the test netplay-local.sh cannot be: that one only proves boot, the
# intro and menus with no input at all. Character select is the first screen
# where BOTH peers must contribute input, so it is the first real exercise of
# input propagation in each direction.
#
# $1 = work dir   $2 = seconds to stay in the match (default 60)   $3 = port
#
# The menu route was found by screenshotting a live session:
#   START xN            skip the intro movie -> MODE SELECT
#   A                   enter the Original mode list (Arcade highlighted)
#   D_DOWN, A           VS Battle -> CHARACTER SELECT
#   P1 A, P2 A          lock both characters
#   P1 A, P2 A          confirm health
#   P2 A                stage select is 2P's choice
# VS Battle is greyed out unless a second controller is present, so reaching it
# at all depends on netplay's pad 2 being live.
set -u
P="${P:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
W="${1:-/tmp/netplay-match}"
PLAY="${2:-60}"
PORT="${3:-2640}"
# PKG/RUNTIME/GAME/MODULE/SAVE exist so this can be pointed at a disc that is
# not the US one -- the peers cd into PKG, so relative values resolve there and
# anything outside it needs an absolute path. SAVE is not optional detail: each
# disc reads its OWN card file (JP wants AF-GRSJ, PAL AF-GRSP, the Plus mod
# PS-GRSE), and a disc that finds none parks on the memory-card dialog, where
# two peers agree perfectly and prove nothing.
PKG="${PKG:-$P/dist/RingOut-1.0-deck}"
RUNTIME="${RUNTIME:-./bin/moderngekko-run}"
GAME="${GAME:-./game}"
MODULE="${MODULE:-./bin/gGRSEAF_recomp.so}"
SAVE="${SAVE:-$PKG/userdata/GC}"

pre="$(pgrep -x moderngekko-run 2>/dev/null | wc -l)"
if [ "$pre" != "0" ]; then
  echo "ABORT: $pre emulator(s) already running (pkill -9 -x moderngekko-run)"
  exit 1
fi

rm -rf "$W"; mkdir -p "$W"

setup_peer() {
  local d="$W/$1"
  mkdir -p "$d/user/Config" "$d/user/Pipes"
  # Mandatory: without save data the game parks forever on the memory-card
  # dialog and nothing below happens.
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
  # A writer must stay open for the whole session: Dolphin opens the FIFO once
  # and never recovers from EOF.
  setsid bash -c 'exec 9<>"'"$d"'/user/Pipes/ctrl"; exec sleep 7200' \
    >/dev/null 2>&1 &
  echo $! > "$d/holder.pid"
}

setup_peer host
setup_peer guest

# WINDOWED=1 shows both peers, so the run can be confirmed by looking at it.
# Headless proves the two peers agree with each other, which is NOT the same as
# proving they reached a match -- two peers agreeing on a menu screen would look
# identical in the hash comparison.
launch() {
  local name="$1"; shift
  local d="$W/$name"
  local -a mode=(--headless)
  [ "${WINDOWED:-0}" = "1" ] && mode=()
  # GX_STATS is the objective witness that the peers reached GAMEPLAY. Without
  # it this script can only claim the two agreed, and its own header says that
  # is not the same claim -- a pair stuck at character select is byte-identical
  # too. Draw traffic separates the two by two orders of magnitude.
  ( cd "$PKG" && exec env RINGOUT_DETERMINISM_LOG="$d/hash.log" \
      RINGOUT_GX_STATS=100 \
      "$RUNTIME" "${mode[@]}" --user-dir "$d/user" --game "$GAME" \
      --module "$MODULE" --controller "Standard Controller" \
      "$@" ) > "$d/log.txt" 2>&1 &
  echo $! > "$d/pid"
}

launch host  --netplay-host --netplay-port "$PORT" --netplay-players 2 \
             --nickname Host --buffer 5 --netplay-timeout 120
sleep 4
launch guest --netplay-join 127.0.0.1 --netplay-port "$PORT" \
             --nickname Guest --netplay-timeout 120

waited=0
while [ $waited -lt 120 ]; do
  grep -qa "netplay armed" "$W/host/log.txt" 2>/dev/null && \
  grep -qa "netplay armed" "$W/guest/log.txt" 2>/dev/null && break
  sleep 2; waited=$((waited + 2))
done
if ! grep -qa "netplay armed" "$W/guest/log.txt" 2>/dev/null; then
  echo "peers never armed"; grep -ha "netplay:" "$W"/*/log.txt | tail -8; exit 1
fi
echo "both peers armed after ${waited}s"

press() {  # $1 = host|guest  $2 = button  [$3 = settle seconds]
  local p="$W/$1/user/Pipes/ctrl"
  printf 'PRESS %s\n' "$2" > "$p"; sleep 0.25
  printf 'RELEASE %s\n' "$2" > "$p"; sleep "${3:-1.2}"
}

# The intro movie plays automatically and is long; press START until the game
# leaves it. Extra presses on MODE SELECT are harmless (START is not Confirm).
echo "skipping intro ..."
for i in $(seq 1 25); do press host START 1.0; done

echo "MODE SELECT -> VS Battle ..."
press host A 1.5          # enter the Original mode list
press host D_DOWN 1.2     # Arcade -> VS Battle
press host A 3.0          # -> CHARACTER SELECT

# Character select, health and stage are all "A" on one side or the other, and
# each screen has an entry animation that eats input arriving too soon. Settles
# are deliberately generous: the earlier version used 1.5s and the presses were
# swallowed, leaving both peers sitting at character select in perfect sync.
echo "character select -> health -> stage ..."
for round in 1 2 3; do
  press host  A 3.0       # lock 1P / confirm 1P health
  press guest A 3.0       # lock 2P / confirm 2P health / pick stage
done

echo "loading match, then playing for ${PLAY}s ..."
sleep 20
slept=0
while [ $slept -lt "$PLAY" ]; do
  # Trade attacks so both peers actually contribute input during gameplay.
  #
  # NEVER press B here. In this game's menus B is Back, so a loop alternating A
  # and B locks and immediately unlocks the character -- which is exactly how
  # the first version of this script got stuck at character select forever while
  # both peers stayed byte-identical and reported no desync. A and X are safe:
  # attacks in a match, and harmless confirms if a menu is somehow still up.
  press host  A 0.5
  press guest X 0.5
  press host  X 0.5
  press guest A 0.5
  slept=$((slept + 4))
  if grep -qa "DESYNC" "$W"/*/log.txt 2>/dev/null; then
    echo "DESYNC after ${slept}s of play"; break
  fi
done

for n in host guest; do kill "$(cat "$W/$n/pid")" 2>/dev/null; done
sleep 2
for n in host guest; do
  kill -9 "$(cat "$W/$n/pid")" 2>/dev/null
  kill -9 "$(cat "$W/$n/holder.pid")" 2>/dev/null
done
sleep 1
left="$(pgrep -x moderngekko-run 2>/dev/null | wc -l)"
[ "$left" != "0" ] && echo "WARNING: $left emulator(s) still alive"

echo
grep -ha "netplay: pad map\|netplay armed\|DESYNC" "$W"/*/log.txt
if [ -s "$W/host/hash.log" ] && [ -s "$W/guest/hash.log" ]; then
  n=$(( $(wc -l < "$W/host/hash.log") < $(wc -l < "$W/guest/hash.log") \
        ? $(wc -l < "$W/host/hash.log") : $(wc -l < "$W/guest/hash.log") ))
  head -n "$n" "$W/host/hash.log"  > "$W/h.trim"
  head -n "$n" "$W/guest/hash.log" > "$W/g.trim"
  echo "guest-RAM hash over $n frames:"
  if diff -q "$W/h.trim" "$W/g.trim" >/dev/null; then
    echo "  IDENTICAL on every frame"
    hash_ok=1
  else
    echo "  FIRST DIVERGENCE:"; diff "$W/h.trim" "$W/g.trim" | head -4
    hash_ok=0
  fi
fi

# Did they actually PLAY? [gx] prints a cumulative mean every 100 displayed
# frames, so differencing consecutive lines gives the traffic of that window.
# Menus and character select sit near 200 draws; a match is tens of thousands.
peak_draws() {
  grep -a '^\[gx\]' "$1" | sed 's/[a-z_]*=//g' |
    awk '{f=$2; d=$4; s=d*f; w=s-p; p=s; if (w>m) m=w} END{printf "%.0f", m+0}'
}
hd="$(peak_draws "$W/host/log.txt")"; gd="$(peak_draws "$W/guest/log.txt")"
echo "peak draw calls per 100 displayed frames:  host=$hd  guest=$gd"
played=0
[ "${hd:-0}" -gt 5000 ] && [ "${gd:-0}" -gt 5000 ] && played=1

if grep -qa "DESYNC" "$W"/*/log.txt 2>/dev/null || [ "${hash_ok:-0}" != "1" ]; then
  echo "RESULT: DESYNCED"
  exit 1
elif [ "$played" != "1" ]; then
  # The peers agreed, but on what? Sitting in a menu in perfect sync is the
  # false pass this script was written to stop claiming.
  echo "RESULT: IN SYNC BUT NEVER REACHED GAMEPLAY -- draw traffic stayed at"
  echo "        menu levels, so the input route did not land. Not a pass."
  exit 1
else
  echo "RESULT: both peers played a match, hashes identical on every frame"
fi
