#!/bin/bash
# Frame-keyed routes that pick Spawn (ID 29) from his own select cell, the free end of Link's row
# (tools/spawn/MAP.md, "Select grid"). Built on vs-stage-route.py's VS route (stage 1), which puts the
# cursors on Talim (1P) and Raphael (2P) and confirms at 1P frame 4000 and 2P frame 4400.
#
#     spawn-route.sh select [p1|p2|both]   VS route; the named side(s) move onto his cell and confirm him
#     spawn-route.sh arcade                arcade-final.txt with the stick held neutral from the first fight
#                                          (frame 4500): his moves under the cycle's steps ring him out. Run
#                                          it as Spawn (RINGOUT_SPAWN_FORCE_ID=29) with the route's two cheat
#                                          codes plus P2's health held at 0 (AR "0436FACC 00000000", the
#                                          float at P2 object 0x8036E560 + 0x156C; MAP.md) so every first hit
#                                          is a KO: the run then reaches the ending
#     spawn-route.sh nav                   both cursors tour his cell and stop on the select screen:
#                                          1P Talim -> Mitsurugi -> Spawn -> Taki -> Spawn -> Mitsurugi -> Spawn,
#                                          2P Raphael -> Taki -> Spawn -> Mitsurugi -> Spawn -> Taki -> Spawn,
#                                          then DOWN and UP (no cell above or below his: stays on him)
#
# Moves: from Talim, UP reaches Mitsurugi and LEFT wraps onto his cell; from Raphael, UP reaches Taki and
# RIGHT reaches his cell. The route is printed to stdout. Needs the per-port input runtime ("P2" lines).
set -eu
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
mode="${1:-}"; side="${2:-both}"
case "$side" in p1|p2|both) ;; *) echo "spawn-route.sh: side is p1, p2 or both" >&2; exit 2;; esac

vs() { python3 "$HERE/vs-stage-route.py" 1 | grep -v '^#'; }
press() { echo "$1 $2 $3"; echo "$1 $(( $2 + 6 )) -"; }   # port frame button: press for 6 frames

case "$mode" in
  select)
    echo "# spawn-route.sh select $side: VS stage 1, Spawn picked from his cell by: $side"
    {
      vs
      case "$side" in p1|both) press P1 3500 UP; press P1 3620 LEFT;; esac
      case "$side" in p2|both) press P2 3500 UP; press P2 3620 RIGHT;; esac
    } | sort -s -k2,2n
    ;;
  arcade)
    echo "# spawn-route.sh arcade: arcade-final.txt, stick neutral from frame 4500 (needs AR 0436FACC 00000000)"
    grep -v '^#' "$HERE/../input-scripts/arcade-final.txt" | awk 'NF >= 4 && $1 >= 4500 { $3 = 128; $4 = 128 } { print }'
    ;;
  nav)
    echo "# spawn-route.sh nav: both cursors tour Spawn's cell (frames 3500-3980), then stay on the select screen"
    {
      vs | awk '$2 < 4000'
      f=3500
      for k in 0 1 2 3 4 5 6 7; do
        p1=(UP LEFT LEFT RIGHT RIGHT LEFT DOWN UP); p2=(UP RIGHT RIGHT LEFT LEFT RIGHT DOWN UP)
        press P1 $f ${p1[$k]}; press P2 $f ${p2[$k]}; f=$(( f + 60 ))
      done
    } | sort -s -k2,2n
    ;;
  *) sed -n '2,19p' "$0" >&2; exit 2;;
esac
