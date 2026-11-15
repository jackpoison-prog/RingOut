#!/bin/bash
# setup.sh --spawn and --heihachi: argument parsing, guest-only mode, and the continue-without-him question. The driver
# (tools/guests/setup_guests.py, tested on its own) is replaced by a stub that exits with $STUB_RC.
#
#   bash dist/RingOut-1.0-dist/tests/test_setup_spawn.sh
set -u
SETUP_DIR="$(cd "$(dirname "$0")/.." && pwd)"
RINGOUT_SETUP_LIB=1 source "$SETUP_DIR/setup.sh"
set +eu +o pipefail
fail=0
check() { # name expected actual
    if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: want '$2', got '$3'"; fail=1; fi
}
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
cat > "$T/stub.py" <<'PY'
import os, sys
open(os.environ['STUB_LOG'], 'a').write(' '.join(sys.argv[1:]) + '\n')
open(os.environ['STUB_LOG'] + '.cc', 'w').write(os.environ.get('CC', '<unset>'))
print('stub says hi'); sys.exit(int(os.environ.get('STUB_RC', '0')))
PY
export RINGOUT_GUESTS_DRIVER="$T/stub.py" STUB_LOG="$T/calls"

# ---- parsing
parse_args --deck --spawn "/x/My Xbox Disc.iso" /g/sc2.iso
check "--spawn PATH"          "/x/My Xbox Disc.iso" "$SPAWN_IMAGE"
check "--spawn keeps the disc" "/g/sc2.iso" "$ISO"
check "--spawn keeps --deck"   "1" "$DECK"
parse_args "--spawn=/x/a b.iso"
check "--spawn=PATH"           "/x/a b.iso" "$SPAWN_IMAGE"
check "no disc"                "" "$ISO"
parse_args /g/sc2.iso
check "no --spawn"             "" "$SPAWN_IMAGE"
parse_args --heihachi "/x/My PS2 Disc.iso" /g/sc2.iso
check "--heihachi PATH"        "/x/My PS2 Disc.iso" "$HEIHACHI_IMAGE"
check "--heihachi keeps the disc" "/g/sc2.iso" "$ISO"
check "--heihachi alone leaves --spawn" "" "$SPAWN_IMAGE"
parse_args "--heihachi=/x/a b.iso" --spawn /x/c.iso
check "--heihachi=PATH"        "/x/a b.iso" "$HEIHACHI_IMAGE"
check "both flags"             "/x/c.iso" "$SPAWN_IMAGE"
msg="$(parse_args --heihachi 2>&1)"; rc=$?
check "--heihachi without a path exit" "1" "$rc"
check "--heihachi without a path msg"  "--heihachi needs the path of your PS2 disc image" "$msg"
msg="$(parse_args --spawn 2>&1)"; rc=$?
check "--spawn without a path exit" "1" "$rc"
check "--spawn without a path msg"  "--spawn needs the path of your Xbox disc image" "$msg"

# ---- the step
mkdir -p "$T/inst/game/files"
STUB_RC=0 guests_step "$T/inst" "/x/My Xbox Disc.iso" "" 0 </dev/null >/dev/null; rc=$?
check "built"                  "0" "$rc"
check "driver arguments" "--spawn /x/My Xbox Disc.iso --game $T/inst/game --log $T/inst/work/guests.log" "$(tail -1 "$T/calls")"
STUB_RC=0 guests_step "$T/inst" "" "/x/My PS2 Disc.iso" 0 </dev/null >/dev/null
check "Heihachi only" "--heihachi /x/My PS2 Disc.iso --game $T/inst/game --log $T/inst/work/guests.log" "$(tail -1 "$T/calls")"
STUB_RC=0 guests_step "$T/inst" /x/i.iso /x/p.iso 0 </dev/null >/dev/null
check "both images" "--spawn /x/i.iso --heihachi /x/p.iso --game $T/inst/game --log $T/inst/work/guests.log" "$(tail -1 "$T/calls")"
STUB_RC=0 guests_step "$T/inst" /x/i.iso "" 0 --check-image </dev/null >/dev/null
check "--check-image passed on" "--spawn /x/i.iso --game $T/inst/game --log $T/inst/work/guests.log --check-image" "$(tail -1 "$T/calls")"
out="$(STUB_RC=3 guests_step "$T/inst" /x/i.iso "" 1 </dev/null)"; rc=$?
check "refused + -y continues"   "0" "$rc"
check "refused + -y says so"     "stub says hi
    continuing without the guest fighters" "$out"
STUB_RC=1 guests_step "$T/inst" /x/i.iso "" 0 </dev/null >/dev/null; rc=$?
check "failed, no terminal: continues" "0" "$rc"
# a real terminal for the question (script(1) gives the step a pty)
ask() { # rc answer -> step status
    printf '%s\n' "$2" | STUB_RC="$1" script -qec "bash -c 'RINGOUT_SETUP_LIB=1 source \"$SETUP_DIR/setup.sh\"; set +e; guests_step \"$T/inst\" /x/i.iso \"\" 0; echo STATUS=\$?'" /dev/null \
        | tr -d '\r' | sed -n 's/.*STATUS=\([0-9]*\).*/\1/p'
}
check "refused, answer n stops"      "1" "$(ask 3 n)"
check "refused, answer empty goes on" "0" "$(ask 3 '')"
check "failed, answer Y goes on"     "0" "$(ask 1 Y)"
q="$(printf 'n\n' | STUB_RC=3 script -qec "bash -c 'RINGOUT_SETUP_LIB=1 source \"$SETUP_DIR/setup.sh\"; set +e; guests_step \"$T/inst\" /x/i.iso \"\" 0'" /dev/null | tr -d '\r')"
case "$q" in *"The guest fighters could not be built: continue without them? [Y/n]"*) r=yes ;; *) r=no ;; esac
check "the question is asked" "yes" "$r"

# setup's own probed compiler reaches the sound encoder (SteamOS ships a clang that cannot compile)
export -n CC 2>/dev/null; CC=/opt/works/clang    # a plain variable, as setup sets it
STUB_RC=0 guests_step "$T/inst" /x/i.iso "" 0 </dev/null >/dev/null; unset CC
check "setup's CC reaches the driver" "/opt/works/clang" "$(cat "$T/calls.cc")"

# --deck copies game/ into the Deck package: Spawn must be in game/ before that copy
spawn_line="$(grep -n '==> Guest fighters (a few minutes)' "$SETUP_DIR/setup.sh" | cut -d: -f1)"
copy_line="$(grep -n 'cp -r "$HERE/game" "$target/game"' "$SETUP_DIR/setup.sh" | cut -d: -f1)"
check "the guests are built before the Deck copy" "yes" "$([ -n "$spawn_line" ] && [ -n "$copy_line" ] && [ "$spawn_line" -lt "$copy_line" ] && echo yes || echo no)"

# the guests' gated PCs only with --spawn/--heihachi: without them the US module is main's (+0.82% Deck cycles otherwise)
gate_line="$(grep -n 'gated-pc 0x800834DC@1' "$SETUP_DIR/setup.sh" | cut -d: -f1)"
gate_if="$(grep -n 'if \[ -n "\$SPAWN_IMAGE\$HEIHACHI_IMAGE" \]; then' "$SETUP_DIR/setup.sh" | cut -d: -f1 |
           awk -v g="${gate_line:-0}" '$1 < g' | tail -1)"
check "the guests' gates sit under --spawn/--heihachi" "yes" "$([ -n "$gate_if" ] && [ -n "$gate_line" ] && [ $((gate_line - gate_if)) -ge 1 ] && [ $((gate_line - gate_if)) -le 3 ] && echo yes || echo no)"
check "Heihachi's gates are in the same list, as group 2" "yes" "$(sed -n "${gate_line:-1},$(( ${gate_line:-1} + 6 ))p" "$SETUP_DIR/setup.sh" | grep -q 'gated-pc 0x80032E44@2' && echo yes || echo no)"

# the pre-build image check goes one image at a time: a refused Xbox disc must not drop a good PS2 disc
check "Spawn's image is checked on its own" "1" "$(grep -c 'guests_run "\$HERE" "\$SPAWN_IMAGE" "" --check-image' "$SETUP_DIR/setup.sh")"
check "Heihachi's image is checked on its own" "1" "$(grep -c 'guests_run "\$HERE" "" "\$HEIHACHI_IMAGE" --check-image' "$SETUP_DIR/setup.sh")"
check "no check clears both images" "0" "$(sed -n "/checking the disc images/,/^fi$/p" "$SETUP_DIR/setup.sh" | grep -c "SPAWN_IMAGE=\"\"; HEIHACHI_IMAGE")"

# ---- guest-only mode
msg="$(guests_only "$T/none" /x/i.iso "" 2>&1)"; rc=$?
check "guest-only without an install exit" "1" "$rc"
check "guest-only without an install msg" "Guest-only setup needs an existing install: give your GameCube disc image too." "$msg"
: > "$T/inst/game/files/root.olk"
# an install whose module has no code sites for them (built without a guest): refused, with the way out
mkdir -p "$T/inst/work/out/generated"
printf '#define DOLRECOMP_GATE_GROUPS 0x1u\n' > "$T/inst/work/out/generated/generated.h"
msg="$(STUB_RC=0 guests_only "$T/inst" /x/i.iso "" 2>&1)"; rc=$?
check "guest-only on a module without their sites exit" "1" "$rc"
check "guest-only on a module without their sites says how" "yes" "$(case "$msg" in *"--spawn <your Xbox disc image>"*) echo yes;; *) echo no;; esac)"
check "no generated.h: no sites" "no" "$(module_has_guest_sites "$T/none" && echo yes || echo no)"
printf '#define DOLRECOMP_GATE_GROUPS 0x3u\n' > "$T/inst/work/out/generated/generated.h"
check "group 1 present: sites" "yes" "$(module_has_guest_sites "$T/inst" && echo yes || echo no)"
check "group 1 only (built for Spawn before Heihachi): not his sites" "no" "$(module_has_guest_sites "$T/inst" h && echo yes || echo no)"
msg="$(STUB_RC=0 guests_only "$T/inst" "" /x/p.iso 2>&1)"; rc=$?
check "adding Heihachi to a Spawn-only module is refused" "1" "$rc"
printf '#define DOLRECOMP_GATE_GROUPS 0x7u\n' > "$T/inst/work/out/generated/generated.h"
check "groups 1 and 2: his sites too" "yes" "$(module_has_guest_sites "$T/inst" h && echo yes || echo no)"
STUB_RC=0 guests_only "$T/inst" "" /x/p.iso >/dev/null; rc=$?
check "adding Heihachi to a module with group 2" "0" "$rc"
STUB_RC=0 guests_only "$T/inst" /x/i.iso "" >/dev/null; rc=$?
check "guest-only on an install" "0" "$rc"
msg="$(STUB_RC=3 guests_only "$T/inst" /x/i.iso "" 2>&1)"; rc=$?
check "guest-only refused: setup says so in its exit status" "3" "$rc"
check "guest-only refused: nothing was changed" "yes" "$(case "$msg" in *"Nothing was changed."*) echo yes;; *) echo no;; esac)"
STUB_RC=1 guests_only "$T/inst" /x/i.iso "" >/dev/null 2>&1 </dev/null; rc=$?
check "guest-only failed: exit 1, no question asked" "1" "$rc"

exit $fail
