#!/bin/bash
# Ring Out : first-time setup
#
# Builds your personal copy from a GameCube disc image you already have. Nothing
# game-derived ships with this package -- this script extracts the disc and
# recompiles its executable here, on your machine.
#
# Usage:  ./setup.sh [--deck] [--pgo] [-y] /path/to/your/disc.iso
#
#   --deck   build a module that will also run on a Steam Deck, which you then
#            copy into the Deck package. See "THE DECK BUILD" below.
set -euo pipefail

HERE="$(dirname "$(readlink -f "$0")")"
DEPS="$HERE/module-src/deps"

# THE DECK BUILD. The Deck package ships no module and its README sends players
# here to make one -- so this script produces the artifact that has to run on
# SOMEONE ELSE'S CPU, and two properties of the build host decide whether it
# will. Both fail silently at the end of a long recompile, which is the worst
# possible time to find out:
#
#   -march   "native" targets THIS machine. The Deck is Zen 2, which tops out at
#            x86-64-v3; a module built on a Zen 4/5 or a recent Intel part can
#            carry AVX-512 or AVX-VNNI, and the Deck takes SIGILL -- an instant
#            crash with no message, possibly mid-match.
#   glibc    the module links whatever the host has. SteamOS is ~2.37 and
#            binaries run forward across glibc versions but never backward, so a
#            module built on a current distro dies at dlopen.
#
# --deck fixes the first and checks the second. Without it the build is tuned
# for this machine, which is faster and right for playing here.
DECK=0
ISO=""
PGO=0
ASSUME_YES=0
for arg in "$@"; do
    case "$arg" in
        --deck) DECK=1 ;;
        --pgo)  PGO=1 ;;
        -y|--yes) ASSUME_YES=1 ;;
        *)      [ -z "$ISO" ] && ISO="$arg" ;;
    esac
done
# --pgo adds a fourth stage. Counted from the flag rather than written into each
# label, so the run that trains a profile does not announce "3/3" and then go on
# to do a fourth thing -- which is what it did the first time this was run.
if [ "$PGO" = 1 ]; then NSTAGES=4; else NSTAGES=3; fi

# Filled in as the build goes, and reported in one block at the very end. A
# profile that was silently not used is the single most likely thing to go
# unnoticed here: it costs 10-14% and looks exactly like a normal build.
PGO_STATE="none"          # none | shipped | self-trained
PGO_WHY=""                # why not, in the player's words

if [ -z "$ISO" ] || [ ! -f "$ISO" ]; then
    echo "Usage: ./setup.sh [--deck] [--pgo] [-y] /path/to/your/disc.iso"
    echo
    echo "Supply a GameCube disc image you already have."
    echo "  --deck   build a module that also runs on a Steam Deck"
    echo "  --pgo    train a profile on YOUR clang and rebuild with it"
    echo "           (one-time; ~11 min extra on a desktop; worth 10-14%)"
    echo "  -y       skip the preflight confirmation and just build"
    exit 1
fi

if [ "$DECK" = 1 ]; then
    MARCH="x86-64-v3"
else
    MARCH="native"
fi

missing=""
for tool in cmake ninja python3; do
    command -v "$tool" >/dev/null || missing="$missing $tool"
done
# Name the command for THIS host rather than making the reader work out which
# of two examples applies. Reported from Bazzite (RingOut#6), where the answer
# is neither: it is Fedora-based AND image-based, so packages go in with
# rpm-ostree and need a reboot before they exist -- a detail that costs a
# confused half hour when the message only offers pacman and apt.
toolchain_hint() {
    id=""; like=""
    if [ -r /etc/os-release ]; then
        id="$(. /etc/os-release 2>/dev/null && printf '%s' "${ID:-}")"
        like="$(. /etc/os-release 2>/dev/null && printf '%s' "${ID_LIKE:-}")"
    fi
    case " $id $like " in
        *" fedora "*|*" rhel "*)
            if command -v rpm-ostree >/dev/null 2>&1; then
                echo "  sudo rpm-ostree install cmake ninja-build clang python3"
                echo "  sudo systemctl reboot     # rpm-ostree layers apply on boot"
            else
                echo "  sudo dnf install cmake ninja-build clang python3"
            fi
            ;;
        *" debian "*|*" ubuntu "*)
            echo "  sudo apt install cmake ninja-build clang python3" ;;
        *" arch "*)
            echo "  sudo pacman -S cmake ninja clang python" ;;
        *)
            echo "  Arch:          sudo pacman -S cmake ninja clang python"
            echo "  Debian/Ubuntu: sudo apt install cmake ninja-build clang python3"
            echo "  Fedora:        sudo dnf install cmake ninja-build clang python3"
            echo "  Fedora atomic: sudo rpm-ostree install cmake ninja-build clang python3"
            echo "                 sudo systemctl reboot" ;;
    esac
}

if [ -n "$missing" ]; then
    echo "Missing required build tools:$missing"
    echo
    echo "Install them and re-run:"
    toolchain_hint
    exit 1
fi

# A compiler being PRESENT is not the same as a compiler that WORKS. SteamOS
# ships clang but not the C library headers, so the old "command -v clang"
# check passed and the build then died on <string.h> after several minutes of
# extracting and recompiling. Actually compile something first, and pick the
# first toolchain that can.
CC=""
_probe="$(mktemp -d)"
cat > "$_probe/probe.c" <<'EOF'
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
int main(void) { return (int)strlen(""); }
EOF
for candidate in clang gcc cc; do
    command -v "$candidate" >/dev/null || continue
    if "$candidate" "$_probe/probe.c" -o "$_probe/probe" >/dev/null 2>&1; then
        CC="$(command -v "$candidate")"
        break
    fi
done
rm -rf "$_probe"

if [ -z "$CC" ]; then
    echo
    echo "A C compiler is installed but cannot compile a trivial program --"
    echo "the C standard library headers (string.h, stdio.h) are missing."
    echo
    echo "This is normal on SteamOS / Steam Deck: the system image ships the"
    echo "compilers but strips the development headers, and /usr is read-only."
    echo
    echo "Options on SteamOS:"
    echo "  1. Build inside a container (recommended -- survives OS updates):"
    echo "       distrobox create -n ringout -i archlinux:latest"
    echo "       distrobox enter ringout"
    echo "       sudo pacman -S --needed base-devel cmake ninja clang python"
    echo "       cd \"$HERE\" && ./setup.sh <your-disc-image>"
    echo
    echo "  2. Or unlock the system image (undone by every SteamOS update):"
    echo "       sudo steamos-readonly disable"
    echo "       sudo pacman-key --init && sudo pacman-key --populate archlinux"
    echo "       sudo pacman -S --overwrite '*' glibc linux-api-headers"
    echo
    echo "On other distros, install the libc development package:"
    echo "  Arch: sudo pacman -S glibc   Debian/Ubuntu: sudo apt install libc6-dev"
    exit 1
fi
echo "Using C compiler: $CC"

# ---------------------------------------------------------------------------
# PREFLIGHT. What is here, what is not, and what each absence costs -- BEFORE
# spending 20 minutes finding out. Everything below was a mid-build surprise at
# some point: a clang that could not read the profile, an llvm-profdata that
# was not installed until after the counts were collected, a training run with
# no save data to get past the memory-card screen.
# ---------------------------------------------------------------------------
row() { printf "    %-16s %-26s %s\n" "$1" "$2" "$3"; }

# The disc ID decides which profile applies, so read it now if the format lets
# us. A plain .iso/.gcm starts with it; .rvz/.gcz are compressed, so those wait
# for the extract step and say so rather than guessing.
PRE_ID="$(head -c 6 "$ISO" 2>/dev/null | tr -dc 'A-Z0-9')"
case "$PRE_ID" in
    G?????) ;;
    *) PRE_ID="" ;;
esac

echo
echo "==> Preflight"
row "compiler" "$("$CC" --version 2>&1 | head -1 | cut -c1-26)" "OK"
row "cmake" "$(cmake --version 2>/dev/null | head -1 | cut -d' ' -f3)" "OK"
row "ninja" "$(ninja --version 2>/dev/null)" "OK"
FREE_GB="$(df -BG --output=avail "$HERE" 2>/dev/null | tail -1 | tr -dc '0-9')"
if [ -n "$FREE_GB" ] && [ "$FREE_GB" -lt 5 ]; then
    row "free space" "${FREE_GB}G" "TIGHT -- needs about 5G"
else
    row "free space" "${FREE_GB:-?}G" "OK"
fi
row "disc image" "${PRE_ID:-read at extract time}" "OK"

# Optional things. None of these stop the build; each one costs something
# specific, and the point of naming them here is that you can fix them now
# instead of after the build.
PRE_PROFDATA="$(command -v llvm-profdata 2>/dev/null || true)"
if [ -z "$PRE_PROFDATA" ]; then
    for c in "$(dirname "$CC")"/llvm-profdata*; do
        [ -x "$c" ] && PRE_PROFDATA="$c" && break
    done
fi
# `|| true` again, and for the same reason as the draws check below: a released
# package ships NO userdata/GC, so find exits non-zero, pipefail propagates it
# and set -e kills setup before it builds anything. Caught by running the actual
# zip -- the staged tree used during development had a save card in it and
# sailed through.
PRE_SAVE="$( { find "$HERE/userdata/GC" \( -name '*.gci' -o -name '*.raw' \) 2>/dev/null || true; } | head -1)"
PRE_IS_CLANG=0
"$CC" --version 2>&1 | grep -qi clang && PRE_IS_CLANG=1

# Does the shipped profile exist for this disc, and can this clang read it?
# Probed, not assumed -- the answer is no on every clang older than 22, which
# is most of them, and it is the single largest thing this check exists to say.
PRE_PROF=""
[ -n "$PRE_ID" ] && [ -f "$HERE/module-src/profiles/$PRE_ID.profdata" ] &&
    PRE_PROF="$HERE/module-src/profiles/$PRE_ID.profdata"
# The build below gives GRSEPS the US profile -- it hooks the US executable in
# place, so its chunks ARE the US disc's chunks. Say the same thing here: this
# check used to report "none ships for GRSEPS" and send a Plus owner off to
# retrain a profile they were about to be given anyway.
[ -z "$PRE_PROF" ] && [ "$PRE_ID" = "GRSEPS" ] &&
    [ -f "$HERE/module-src/profiles/GRSEAF.profdata" ] &&
    PRE_PROF="$HERE/module-src/profiles/GRSEAF.profdata"
if [ "$PRE_IS_CLANG" = 1 ] && [ -n "$PRE_PROF" ]; then
    if echo 'int main(void){return 0;}' |
       "$CC" -x c - -fprofile-use="$PRE_PROF" -o /dev/null >/dev/null 2>&1; then
        row "shipped profile" "readable by your clang" "OK -- 10-14% faster"
    else
        row "shipped profile" "TOO NEW for your clang" "costs 10-14%; --pgo fixes it"
    fi
elif [ "$PRE_IS_CLANG" != 1 ]; then
    row "shipped profile" "needs clang" "costs 10-14%"
elif [ -n "$PRE_ID" ]; then
    row "shipped profile" "none ships for $PRE_ID" "costs 10-14%; --pgo fixes it"
else
    row "shipped profile" "checked after extract" "-"
fi

if [ "$PGO" = 1 ]; then
    if [ -n "$PRE_PROFDATA" ]; then
        row "llvm-profdata" "$(basename "$PRE_PROFDATA")" "OK -- needed by --pgo"
    else
        row "llvm-profdata" "NOT INSTALLED" "--pgo cannot merge counts"
    fi
    if [ -n "$PRE_SAVE" ]; then
        row "save data" "found" "OK -- training needs it"
    else
        row "save data" "NONE in userdata/GC" "training will not reach a match"
    fi
fi
command -v lld >/dev/null 2>&1 ||
    row "lld" "not installed" "no effect -- measured at 0.1%"

# Anything above that would waste the run, said again as a warning rather than
# left for the reader to spot in a table.
PRE_WARN=""
if [ "$PGO" = 1 ] && [ -z "$PRE_PROFDATA" ]; then
    PRE_WARN="$PRE_WARN\n    --pgo needs llvm-profdata. Arch: sudo pacman -S llvm"
fi
if [ "$PGO" = 1 ] && [ -z "$PRE_SAVE" ]; then
    PRE_WARN="$PRE_WARN\n    --pgo has no save data to train with: the game parks on the"
    PRE_WARN="$PRE_WARN\n    memory-card screen and the profile is refused (measured: 2.4"
    PRE_WARN="$PRE_WARN\n    draws/frame there against 84 in a match). Play once, then re-run."
fi
if [ -n "$PRE_WARN" ]; then
    echo
    echo "  Heads up before you spend the time:"
    printf '%b\n' "$PRE_WARN"
fi

# The prompt only exists where someone can answer it. The launcher runs this in
# a terminal, but it is also run from scripts and piped, and a setup that blocks
# forever on a question nobody sees is worse than one that just builds.
if [ "$ASSUME_YES" != 1 ] && [ -t 0 ]; then
    echo
    printf "  Start the build? [Y/n] "
    read -r _answer || _answer=""
    case "$_answer" in
        [Nn]*) echo "  Stopped. Nothing was changed."; exit 0 ;;
    esac
fi

echo "==> 1/$NSTAGES  Extracting disc"
rm -rf "$HERE/game"
# Two extractors, and the second one reads more than the first.
#
# tools/dolrecomp handles .iso, .gcm and .wbfs. The launcher's file picker has
# always ALSO offered .rvz and .gcz, and for those the old path stopped dead at
# "unsupported format: only .iso and .wbfs are supported" -- after the player
# had already chosen their disc, with no suggestion of what to do next.
#
# bin/moderngekko-run reads all of them, because it carries Dolphin's DiscIO
# and always did; nothing here was reachable from this step until now. Its
# output is byte-identical to the recompiler's on a disc both can read (checked
# file by file, system files and all three multi-hundred-MB archives).
#
# The recompiler stays first so the common .iso path is unchanged, and the
# fallback covers the rest rather than replacing anything.
if ! "$HERE/tools/dolrecomp" extract "$ISO" "$HERE/game" 2>/dev/null; then
    echo "    (that format needs the runtime's extractor -- using it)"
    if ! "$HERE/bin/moderngekko-run" --extract "$ISO" "$HERE/game"; then
        echo
        echo "Could not read $ISO as a GameCube disc image."
        echo "Supported: .iso .gcm .wbfs .rvz .gcz .wia and NKit variants of those."
        exit 1
    fi
fi

DISC_ID="$(head -c 6 "$HERE/game/sys/boot.bin" 2>/dev/null || true)"
if [ -z "$DISC_ID" ]; then
    echo "Could not read a disc ID -- is that a GameCube disc image?"
    exit 1
fi
echo "    disc id: $DISC_ID"

# Record what came off the disc, so the game can later tell whether root.olk is
# still that. Character skins (the GameBanana ones) patch root.olk in place with
# olkviewer and leave main.dol alone, which is exactly the case the netplay
# fingerprint could not see before: two peers matched on every field and then
# desynced in play. This baseline is what makes the MODS tab able to say
# "modified" rather than guess, and it names the image so the original can be
# put back without keeping a second 590 MB copy of it.
#
# SHA-1, not SHA-256: this answers "did these bytes change", and it is the
# digest the runtime already has a hardware-accelerated path for.
OLK="$HERE/game/files/root.olk"
if [ -f "$OLK" ] && command -v sha1sum >/dev/null 2>&1; then
    mkdir -p "$HERE/userdata"
    OLK_HASH="$(sha1sum "$OLK" | cut -d" " -f1)"
    # An absolute path, because the request is acted on from the launcher, whose
    # working directory is not this one.
    ISO_ABS="$(cd "$(dirname "$ISO")" && pwd)/$(basename "$ISO")"
    {
        echo "# Written by setup.sh. Deleting this only costs you the ability"
        echo "# to detect and undo a game-data mod; the game runs either way."
        echo "[Origin]"
        echo "GameDataHash = $OLK_HASH"
        echo "SourceImage = $ISO_ABS"
    } > "$HERE/userdata/disc-origin.txt"
    # The cached hash belongs to the OLD root.olk if one was there before.
    rm -f "$HERE/userdata/game-data-hash.txt" "$HERE/userdata/restore-game-data.request"
    echo "    game data: baseline recorded"
else
    echo "    game data: no baseline (sha1sum missing) -- mod detection stays off"
fi

echo "==> 2/$NSTAGES  Recompiling the game executable (several minutes)"
rm -rf "$HERE/work"
mkdir -p "$HERE/work"
# --idle-pc is not optional. Loop back-edges are compiled as native gotos, which
# is where most of the recompiler's speed comes from -- but the OS idle spin
# loop must stay a dispatcher return, or the host never sees the guest idling
# and idle-skip silently stops working, at a cost of about half the frame rate.
#
# "auto" finds that loop in YOUR disc's DOL instead of assuming one disc's
# address: it is at 0x80185DEC in the US release and 0x8017F35C in the Japanese
# one, so the constant this used to pass was right for exactly one of them. The
# recompiler prints the address it found. If it finds none -- or more than one
# candidate, which it will not guess between -- it says so and carries on; the
# build is still correct, just slower.
#
# --leader-cases keeps chunk-entry switch cases only where control can arrive
# (block leaders, FP-guard resume sites, every direct branch and jump-table
# target in the program) instead of one per instruction. Frame hashes identical
# to the full switch; with its retrained profile, -8.96% CPU cycles on a real
# match against 1.6.1.
#
# EVERY DISC THIS SCRIPT KNOWS. The flag changes every chunk's control flow, so
# a profile trained without it stops matching -- clang silently drops the counts,
# which costs more than the flag saves, so a disc gets the flag only once its
# own profile has been retrained with it. All four have been, and each was gated
# on frame hashes identical to the full switch over 16000 frames of a real match
# before its profile was trained:
#     JP   (GRSJAF)  -20.00% CPU cycles against what that player gets today
#     PAL  (GRSPAF)  -21.32%
#     Plus (GRSEPS)  -20.73%
# An unknown disc ID still gets none of this, and no profile.
LEADER_CASES=()
case "$DISC_ID" in GRSEAF|GRSJAF|GRSPAF|GRSEPS) LEADER_CASES=(--leader-cases);; esac
# --direct-calls (same disc, same reason): a call into another chunk calls it
# natively instead of returning to the dispatcher -- half the dispatches, -22.6%
# CPU cycles with its profile against 1.6.1. It changes guest timing slightly,
# so replays carry a timing marker and warn when played on a different build.
# Every address the run loop hooks must still be reached through the
# dispatcher, or the movie player's hooks would never fire: those are passed as
# --dispatch-pc. They are US-disc addresses, which is part of why this is gated.
#
# Mapped for the other discs by .github/scripts/map-fmv-hooks.py (vote over
# mid-function slices; a single shared delta across all six is the validator):
#   Plus (GRSEPS): identical to the US addresses, delta 0, 23-24/24 votes.
#   PAL  (GRSPAF): the whole library sits +0x7750 -> 0x80213938 0x80210888
#                  0x80214B08 0x8020F5E0 0x8020F638 0x8020F994.
#   JP   (GRSJAF): NOT mapped. Only 2 of 6 agree and their deltas do not
#                  cluster, so its library is rearranged, not just relocated.
#                  map-fmv-hooks.py --masked is the pass for that case; it has
#                  not been run against the JP disc yet.
# The runtime hooks each disc's own six from a per-disc table keyed on the
# module's game id (StaticRecompFmvHooks.h), which must agree with the lists
# below. US, Plus and PAL are playtested with the native movie player; JP has
# no row. A row added before its playtest stays off unless
# STATICRECOMP_FMV_UNPLAYTESTED=1.
#
# US AND PLUS. Plus is a hack of the US text -- its movie library is
# byte-identical at all six addresses -- and it was playtested on this build,
# which is the same bar the US disc had to clear.
case "$DISC_ID" in
GRSEAF|GRSEPS) LEADER_CASES+=(--direct-calls
    --dispatch-pc 0x8020C1E8 --dispatch-pc 0x80209138 --dispatch-pc 0x8020D3B8
    --dispatch-pc 0x80207E90 --dispatch-pc 0x80207EE8 --dispatch-pc 0x80208244);;
# JP passes NO --dispatch-pc, and that is not an oversight. Its movie library is
# not mapped, so the run loop hooks nothing on this disc (no row in the per-disc
# table), and the US addresses are not entry points here AT ALL -- 0 entry-switch
# cases and absent from generated_entries.txt. Passing them would only make the
# wrong code reachable by the dispatcher. The idle PC protects itself (the recompiler adds
# it, emitter.c emit_set_idle_pc), and it is in the entry list here with no
# flag passed.
GRSJAF) LEADER_CASES+=(--direct-calls);;
# PAL passes its OWN six, mapped by .github/scripts/map-fmv-hooks.py: the whole
# movie library sits +0x7750 from the US one. The runtime's per-disc table hooks
# these same six, and they must stay dispatch points for that to work under
# --direct-calls. It is also what the
# playtested build was built with. Six extra dispatch points out of 185066.
GRSPAF) LEADER_CASES+=(--direct-calls
    --dispatch-pc 0x80213938 --dispatch-pc 0x80210888 --dispatch-pc 0x80214B08
    --dispatch-pc 0x8020F5E0 --dispatch-pc 0x8020F638 --dispatch-pc 0x8020F994);;
esac
# --self-calls (same discs): a call to a function in the SAME chunk also calls
# natively, so neither the call nor its return goes through the dispatcher.
# -5.8% CPU cycles on top of the above for US; the two together are -16.34% for
# Plus, taking it to -34.0% against what a Plus player gets today.
case "$DISC_ID" in GRSEAF|GRSEPS|GRSJAF|GRSPAF) LEADER_CASES+=(--self-calls);; esac
# --chunk-overhang (US, Plus, PAL): chunks are fixed 16 KB windows, and on these
# three discs one boundary falls inside a hot loop (0x8000D8D0-0x8000D96C), so
# every iteration made two dispatcher round trips -- ~13% of all chunk entries.
# The overhang lets a chunk run on past its window to the first exit, so the
# loop closes inside one chunk. -1.84% CPU cycles for US on the Steam Deck.
# NOT for JP: its layout does not straddle, and fewer dispatches shift event
# timing -- on JP that changed the game's path for nothing in return.
case "$DISC_ID" in GRSEAF|GRSEPS|GRSPAF) LEADER_CASES+=(--chunk-overhang 512);; esac
# --ram-bases 1,2,13 (all four discs): r1, r2 and r13 are the stack pointer and
# the two small-data bases, and only ever address main RAM. A STORE based on one
# skips the range test that decides between RAM and MMIO (loads keep it: on Zen 2
# a load straight after a store to the same place stalls either way, and taking
# it for loads too measured +1.13%). A census over arcade and VS on every disc
# found no marked access outside RAM. Steam Deck, cycles, both arms retrained:
# US -0.61%, JP -0.39%, Plus -1.07%, PAL -0.89%; the game's behaviour is unchanged.
case "$DISC_ID" in GRSEAF|GRSJAF|GRSPAF|GRSEPS) LEADER_CASES+=(--ram-bases 1,2,13);; esac
# --fp-check-once (all four discs): every FP instruction tested MSR.FP, 1.43% of
# a gameplay profile. MSR changes only at mtmsr or at a block boundary, so one
# test per straight-line run of FP instructions is enough; every entry point
# still tests. Steam Deck, cycles, both arms retrained, game behaviour unchanged:
# US -2.61%, JP -2.24%, Plus -0.99%, PAL -2.23%.
case "$DISC_ID" in GRSEAF|GRSJAF|GRSPAF|GRSEPS) LEADER_CASES+=(--fp-check-once);; esac
# The inline paired-single fast path (module-src CMakeLists MODULE_PSQ_FAST) has
# the same constraint and the same gate: it changes chunk control flow, so a
# disc needs a profile trained with it. US, JP and PAL all have one. Every
# module build below passes this.
PSQ_FAST=()
case "$DISC_ID" in GRSEAF|GRSJAF|GRSPAF|GRSEPS) PSQ_FAST=(-DMODULE_PSQ_FAST=ON);; esac
# MODULE_MEM_FAST (same gate, same reason): leaner inlined RAM accesses with no
# debug-journal, reservation or NULL tests. -6.3% CPU cycles on top of the
# above with its retrained profile; the game's behaviour is unchanged.
case "$DISC_ID" in GRSEAF|GRSJAF|GRSPAF|GRSEPS) PSQ_FAST+=(-DMODULE_MEM_FAST=ON);; esac
# Twin chunks: every chunk is emitted twice -- a fast copy entered only at the
# entry PCs a training run actually used, with rarely-written guest registers
# held in locals, and the ordinary chunk as a cold fallback for any other entry,
# so any entry stays correct. The hot-entry list ships beside the profile.
# About -6 to -8% CPU cycles per disc on the Steam Deck, game behaviour
# unchanged. A disc without a list builds exactly as before.
TWIN=()
if [ -f "$HERE/module-src/profiles/$DISC_ID.hot" ]; then
    # --twin-cr: the fast copy also keeps the CR fields in write-through locals,
    # so a branch reads its compare's result from a register instead of from
    # ctx, which every guest store forces clang to reload. Steam Deck, cycles,
    # both arms retrained, game behaviour unchanged: US -0.69%, JP -0.25%,
    # Plus -1.20%, PAL -1.75%.
    TWIN=(--twin-hot "$HERE/module-src/profiles/$DISC_ID.hot" --twin-regs ratio --twin-cr)
fi
"$HERE/tools/dolrecomp" --gamecube "$HERE/game/sys/main.dol" --idle-pc auto \
    "${LEADER_CASES[@]}" "${TWIN[@]}" -j"$(nproc)" "$HERE/work/out"
# gen_module_tables.py reads main.dol from alongside the generated sources.
cp "$HERE/game/sys/main.dol" "$HERE/work/out/generated/main.dol"

echo "==> 3/$NSTAGES  Building the module"
# Profile-guided optimisation, when a profile ships beside the sources and the
# compiler we found is clang. Worth -11.9% CPU cycles and 54.5 -> 59.6 fps on a
# real match (measured 2026-08-12; see module-src/CMakeLists.txt for the full
# numbers), at the cost of roughly three times the build below.
#
# The profile is trained on SOULCALIBUR II gameplay, so it only matches a module
# generated from that game's DOL. A different disc still builds -- clang reports
# the functions as unprofiled and optimises them normally -- and a profile this
# clang cannot read is skipped rather than fatal. Both cases are handled in the
# CMakeLists, which probes before committing to the flag.
# PROFILE-GUIDED BUILD, PICKED BY DISC ID.
#
# A chunk function is named for its guest address, so a profile only fits the
# disc it was trained on. Measured 2026-08-26 on a Japanese module, 3 arms x 3
# reps over 24000 frames of a real match, all arms hashing identically:
#
#     no profile      806.50 Gcyc              IPC 2.512
#     US profile      795.68 Gcyc   -1.34%     IPC 2.450
#     matching one    697.58 Gcyc  -13.50%     IPC 2.640
#
# A MISMATCHED profile is worse than no profile for IPC -- clang has no usable
# counts and inlines blind, producing a bigger, slower module. The 1.34% it
# still buys comes from the runtime helpers, whose names do not move between
# regions. So: use the profile for THIS disc, and if there is not one, use none.
#
# AND PICKED BY CLANG VERSION. A profile records each function's shape as the
# compiler saw it, and a newer clang's optimiser reshapes the code before it
# compares: clang 23 matched only ~1/3 of the chunks of a clang-22 profile and ran
# the rest unprofiled, silently (2026-10-02, the day Arch moved to 23). So a
# profile trained on the player's clang major, <ID>.clangNN.profdata, wins; the
# plain <ID>.profdata is the clang-22 set (also what the Windows package's pinned
# clang 22 uses). The check after the build reports any mismatch that is left.
PGO_ARGS=()
PROFILE=""
CLANG_MAJOR="$("$CC" -dumpversion 2>/dev/null | cut -d. -f1)"
if "$CC" --version 2>&1 | grep -qi clang; then
    if [ -n "$CLANG_MAJOR" ] && [ -f "$HERE/module-src/profiles/$DISC_ID.clang$CLANG_MAJOR.profdata" ]; then
        PROFILE="$HERE/module-src/profiles/$DISC_ID.clang$CLANG_MAJOR.profdata"
    elif [ -f "$HERE/module-src/profiles/$DISC_ID.profdata" ]; then
        PROFILE="$HERE/module-src/profiles/$DISC_ID.profdata"
    elif [ "$DISC_ID" = "GRSEPS" ] && [ -f "$HERE/module-src/profiles/GRSEAF.profdata" ]; then
        # SC2 Plus, the community mod. It appends its own code at 0x80476000 and
        # hooks the base text IN PLACE rather than relocating it, so its chunks
        # are the US disc's chunks and the US profile really does apply -- that
        # was confirmed directly, a Plus chunk compiled against the US profile
        # carries !prof with a function_entry_count. Plus now ships a profile
        # trained on Plus itself (-20.73% against the US-profile build), so this
        # branch is only reached by an older or hand-assembled tree where that
        # file is missing; the US profile is still the right fallback there.
        PROFILE="$HERE/module-src/profiles/GRSEAF.profdata"
    elif [ -f "$HERE/module-src/module.profdata" ] && [ "$DISC_ID" = "GRSEAF" ]; then
        # The original single-profile layout, kept so an older package or a
        # hand-assembled tree still builds the way it always did.
        PROFILE="$HERE/module-src/module.profdata"
    fi
fi
if [ -n "$PROFILE" ]; then
    PGO_ARGS=(-DMODULE_PGO_PROFILE="$PROFILE")
    echo "    profile-guided build for $DISC_ID (this takes longer, and is worth it)"
else
    # Deliberately NOT falling back to another region's profile: measured above,
    # that is worse than building without one.
    echo "    no profile ships for $DISC_ID, so this builds without one."
    echo "    Your module is correct; a profiled build is 10-14% faster."
    [ "$PGO" = 1 ] || echo "    ./setup.sh --pgo would train one here (~11 min extra, once)."
    if "$CC" --version 2>&1 | grep -qi clang; then
        PGO_WHY="no profile ships for $DISC_ID"
    else
        PGO_WHY="$(basename "$CC") is not clang, and PGO here is clang-only"
    fi
fi

# Teed, not just printed: the summary at the end reports whether the profile was
# actually USED, and the only thing that knows that is CMake's own probe output.
# Inferring it from "we passed -DMODULE_PGO_PROFILE" would report success on
# every machine whose clang silently refused the file -- which is most of them.
cmake -S "$HERE/module-src" -B "$HERE/work/build" -GNinja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER="$CC" \
      -DCMAKE_C_FLAGS="-march=$MARCH" \
      "${PGO_ARGS[@]}" \
      "${PSQ_FAST[@]}" \
      -DGAME_ID="$DISC_ID" \
      -DGENERATED_DIR="$HERE/work/out/generated" \
      -DDOLRECOMP_SRC="$DEPS/dolrecomp-src" \
      -DGXRUNTIME_INC="$DEPS/gxruntime-include" \
      -DCHASSIS_ABI_DIR="$DEPS/chassis-abi" \
      -DMODULE_TEMPLATE="$DEPS/module-template" 2>&1 | tee "$HERE/work/configure.log"
cmake --build "$HERE/work/build"

cp "$HERE/work/build/g${DISC_ID}_recomp.so" "$HERE/bin/"

# What did CMake actually do with the profile? Its own probe is the authority:
# a profile can be present, passed, and still refused for being too new to read,
# and that refusal looks exactly like an ordinary build from the outside.
if grep -q "module: PGO enabled" "$HERE/work/configure.log" 2>/dev/null; then
    PGO_STATE="shipped"
elif grep -q "cannot read the shipped PGO profile" "$HERE/work/configure.log" 2>/dev/null; then
    PGO_WHY="your clang is older than the LLVM 22 that wrote the shipped profile"
elif grep -q "total count of ZERO" "$HERE/work/configure.log" 2>/dev/null; then
    PGO_WHY="the shipped profile reads as all-cold, which is worse than none"
elif [ -n "$PROFILE" ] && [ -z "$PGO_WHY" ]; then
    PGO_WHY="this compiler could not use the shipped profile"
fi

# DOES THE PROFILE STILL FIT? "PGO enabled" only means clang read the file. Whether
# it matched the code is a warning the build above silences (-Wno-backend-plugin),
# and a mismatched chunk -- each chunk is about one function -- builds unprofiled
# without a word. Recompile the three chunks holding the hottest entry points
# (from the twin hot list) with that warning on, and count. A few seconds.
PGO_MISMATCH=0
HOT="$HERE/module-src/profiles/$DISC_ID.hot"
if [ "$PGO_STATE" = "shipped" ] && [ -f "$HOT" ] && command -v ninja >/dev/null 2>&1; then
    hot_chunks=()
    while read -r pc _; do
        # the chunk holding pc: the last chunk file whose start address is <= pc
        c="$(ls "$HERE/work/out/generated/chunks" 2>/dev/null |
             awk -F_ -v pc="$(echo "$pc" | tr A-F a-f)" '{a=tolower($4); sub(/\.c$/,"",a); if (a <= pc) c=$0} END {print c}' || true)"
        [ -n "$c" ] && hot_chunks+=("${c%.c}")
    done < <(tr -d '\r' < "$HOT" | sort -k2 -n -r | head -3)
    for c in $(printf '%s\n' "${hot_chunks[@]}" | sort -u); do
        cmd="$(cd "$HERE/work/build" && ninja -t commands 2>/dev/null | grep -F "/$c.c.o -c" | head -1 |
               sed -E 's/ -o \S+/ -o \/dev\/null/; s/ -MD -MT \S+ -MF \S+//' || true)"
        [ -n "$cmd" ] || continue
        # grep -c exits 1 on a count of 0, which set -e + pipefail would turn into
        # an abort of setup itself -- exactly on the GOOD outcome (2026-10-02).
        n="$(cd "$HERE/work/build" && bash -c "$cmd -Wbackend-plugin" 2>&1 | grep -c 'hash mismatch' || true)"
        [ "${n:-0}" -gt 0 ] && PGO_MISMATCH=$((PGO_MISMATCH + 1))
    done
    if [ "$PGO_MISMATCH" -gt 0 ]; then
        PGO_STATE="mismatch"
        PGO_WHY="clang $CLANG_MAJOR reshapes the code before matching it, so $PGO_MISMATCH of the 3 hottest chunks ignore the shipped profile (it was trained on another clang version)"
    fi
fi

# --pgo: TRAIN A PROFILE ON THIS MACHINE'S CLANG, then rebuild with it.
#
# The profiles that ship are written by LLVM 22, and an indexed profile can only
# be read by an LLVM at least as new as the one that wrote it -- so clang 20
# (SteamOS 3.8, Fedora), 18 (Ubuntu 24.04) and 14 (Debian 12) all refuse them
# with "unsupported instrumentation profile format version". CONFIRMED on a
# stock Steam Deck: MODULE_PGO_USABLE came back empty and the module built 23 MB
# instead of 39 MB. Readability is also not the whole story: a profile keys its
# counts to a CFG hash the INSTRUMENTING compiler computed, so where two clang
# versions build a different CFG for the same function the counts for it are
# dropped -- silently, since the warning that would say so is suppressed below.
# That part is reasoning about how -fprofile-use works, not something measured
# here; the format refusal above IS measured. Either way no single shipped file
# serves every clang, and the way to get the win on an arbitrary machine is to
# make the profile on it, which is what this does.
#
# Worth 10-14% of CPU time -- the range is the machine, not the uncertainty.
# Over 6000 fixed frames of an arcade match, 6 alternating reps per arm:
#   desktop (Zen 3)    176.95 -> 158.66 Gcyc  -10.3%  (shipped profile)
#                      176.95 -> 156.72 Gcyc  -11.4%  (trained here by --pgo)
#   Steam Deck (Zen 2) 233.99 -> 201.07 Gcyc  -14.1%  (trained on the Deck,
#                      8 runs an arm, ranges not overlapping, IPC 2.005 ->
#                      2.136, wall clock 75.5s -> 68.3s)
# A profile trained on the spot beat the shipped one by 1.22% on the desktop,
# and the two arms' ranges did not overlap over 6 reps -- so --pgo is not a
# degraded fallback, it is the better profile, which is what you would expect of
# one whose CFG hashes match the compiler doing the build exactly.
if [ "$PGO" = 1 ]; then
  if ! "$CC" --version 2>&1 | grep -qi clang; then
    echo
    echo "==> --pgo needs clang; $CC is not clang, so skipping the training pass."
    echo "    (GCC cannot read a clang profile and its own PGO was not measured here.)"
  else
    echo
    echo "==> 4/$NSTAGES  Training a profile with YOUR clang (one time only)"
    echo "    Measured: ~11 minutes on a desktop, on top of a normal build."
    echo "    A Steam Deck is a good deal slower -- leave it running."
    PROFDIR="$HERE/work/pgo"
    # With twin chunks the training build also counts entries the fast copies
    # hand to their cold copies (DOLRECOMP_TWIN_MISS_LOG, cpu.h): the shipped hot
    # list can miss hot entry points, silently -- 7-8% of entries on three discs
    # once a codegen change moved them -- and a profile trained that way learns
    # the slow path.
    MISS_DEF=""
    [ "${#TWIN[@]}" -gt 0 ] && MISS_DEF=" -DDOLRECOMP_TWIN_MISS_LOG"

    # One instrumented build + one training run. A function because a stale hot
    # list means doing it twice (see below).
    pgo_train() {
    rm -rf "$PROFDIR" "$HERE/work/build-gen" "$HERE/work/pgo-twin-miss.txt"; mkdir -p "$PROFDIR"

    # MODULE_LTO=OFF is REQUIRED here, not a speed choice: instrumented objects
    # are LTO IR, and mixing them with a non-LTO link silently produces a 21 KB
    # stub that loads and does nothing -- a training run against it would look
    # like it worked and collect nothing.
    cmake -S "$HERE/module-src" -B "$HERE/work/build-gen" -GNinja \
             -DCMAKE_BUILD_TYPE=Release \
             -DCMAKE_C_COMPILER="$CC" \
             -DCMAKE_C_FLAGS="-march=$MARCH -fprofile-generate=$PROFDIR$MISS_DEF" \
             -DCMAKE_SHARED_LINKER_FLAGS="-fprofile-generate=$PROFDIR" \
             -DMODULE_LTO=OFF \
             "${PSQ_FAST[@]}" \
             -DGAME_ID="$DISC_ID" \
             -DGENERATED_DIR="$HERE/work/out/generated" \
             -DDOLRECOMP_SRC="$DEPS/dolrecomp-src" \
             -DGXRUNTIME_INC="$DEPS/gxruntime-include" \
             -DCHASSIS_ABI_DIR="$DEPS/chassis-abi" \
             -DMODULE_TEMPLATE="$DEPS/module-template" >"$HERE/work/pgo-gen.log" 2>&1 \
       && cmake --build "$HERE/work/build-gen" >>"$HERE/work/pgo-gen.log" 2>&1 \
       || return 1
        # 6000 frames of ONE arcade match. Broader training sets were tried
        # twice and both LOST (+2.18% and +1.26%): weight went to code the
        # player does not run. Menus are not a substitute either -- they carry
        # ~0.1% of a session's paired-single traffic.
        echo "    playing 6000 frames to collect counts ..."
        rm -rf "$HERE/work/pgo-user"; mkdir -p "$HERE/work/pgo-user/Config"
        cp -r "$HERE/userdata/GC" "$HERE/work/pgo-user/" 2>/dev/null || true
        # RINGOUT_DETERMINISM_LOG is what ARMS the harness: the frame-keyed
        # input and the frame limit are both inert without it, so setting only
        # FRAMES and INPUT gives a run that gets no input and never stops. That
        # is not a hypothetical -- it is what the first version of this did, and
        # it sat on the title screen past 10000 frames pushing 3 draws each.
        # NOHASH keeps the input and the frame counter but drops the per-frame
        # guest-RAM hash, which is wanted twice over here: it is ~29% of cycles,
        # and profiling it would spend the profile's weight on the harness's
        # crc32 instead of the game.
        DOLRECOMP_TWIN_MISS_FILE="$HERE/work/pgo-twin-miss.txt" \
        LLVM_PROFILE_FILE="$PROFDIR/%p.profraw" \
        RINGOUT_GX_STATS=1000 \
        RINGOUT_DETERMINISM_LOG="$HERE/work/pgo-frames.log" \
        RINGOUT_DETERMINISM_NOHASH=1 \
        RINGOUT_DETERMINISM_FRAMES=6000 \
        RINGOUT_DETERMINISM_INPUT="$HERE/tools/train-route.txt" \
          "$HERE/bin/moderngekko-run" --headless \
            --user-dir "$HERE/work/pgo-user" --game "$HERE/game" \
            --module "$HERE/work/build-gen/g${DISC_ID}_recomp.so" \
            >"$HERE/work/pgo-train.log" 2>&1 || true

    return 0
    }

    if pgo_train
    then
        # A STALE HOT LIST: entry PCs the cold copies took at least 1000 times in
        # 6000 frames. Past 500k such entries (a stale list measured ~15M per 6000
        # frames; a current one a few thousand), add them to a local copy of the
        # list, regenerate, and train again, so the profile learns the fast copies.
        MISS_FILE="$HERE/work/pgo-twin-miss.txt"
        if [ -n "$MISS_DEF" ] && [ -s "$MISS_FILE" ]; then
            read -r MISS_PCS MISS_HITS < <(awk '$2 >= 1000 { n++; s += $2 } END { print n + 0, s + 0 }' "$MISS_FILE")
            if [ "$MISS_HITS" -ge 500000 ]; then
                echo "    the shipped hot list misses $MISS_PCS entry points ($MISS_HITS entries);"
                echo "    adding them to a local list and training again ..."
                { cat "$HERE/module-src/profiles/$DISC_ID.hot"; awk '$2 >= 1000' "$MISS_FILE"; } |
                    sort -u -k1,1 > "$HERE/work/local.hot"
                TWIN=(--twin-hot "$HERE/work/local.hot" --twin-regs ratio)
                if ! { "$HERE/tools/dolrecomp" --gamecube "$HERE/game/sys/main.dol" --idle-pc auto \
                         "${LEADER_CASES[@]}" "${TWIN[@]}" -j"$(nproc)" "$HERE/work/out" \
                         >"$HERE/work/pgo-regen.log" 2>&1 \
                       && cp "$HERE/game/sys/main.dol" "$HERE/work/out/generated/main.dol" \
                       && pgo_train; }; then
                    # The first run's counts were cleared when the second began, so
                    # there is nothing to merge: the checks below report no counts
                    # and keep the module already built, which is correct.
                    echo "    retraining with the refreshed list failed (see work/pgo-regen.log"
                    echo "    and work/pgo-gen.log); keeping the module you already have."
                    rm -rf "$PROFDIR"; mkdir -p "$PROFDIR"
                fi
            fi
        fi

        # DID THE TRAINING RUN ACTUALLY REACH A FIGHT? A run that parked on a
        # dialog still exits cleanly and still writes a structurally valid
        # profile -- one that says every block is cold, which builds a module
        # SLOWER than no profile at all. Only a count can tell the two apart:
        # gameplay pushes hundreds of draw calls a frame, a menu pushes about
        # one. Measured on a PAL disc: 1.3 draws/frame over the logos, 259 once
        # the match starts.
        # `|| true` is load-bearing: this script runs under `set -o pipefail`,
        # so a grep that matches nothing would fail the whole pipeline and abort
        # setup -- in exactly the case the message below exists to explain.
        DRAWS="$( { grep '^\[gx\]' "$HERE/work/pgo-train.log" 2>/dev/null || true; } | tail -1 |
                 sed -n 's/.*draws=\([0-9]*\).*/\1/p')"
        PROFDATA_TOOL="$(command -v llvm-profdata 2>/dev/null || true)"
        if [ -z "$PROFDATA_TOOL" ]; then
            for c in "$(dirname "$(command -v "$CC")")"/llvm-profdata*; do
                [ -x "$c" ] && PROFDATA_TOOL="$c" && break
            done
        fi

        TRAINED="$(wc -l < "$HERE/work/pgo-frames.log" 2>/dev/null || echo 0)"
        if ! ls "$PROFDIR"/*.profraw >/dev/null 2>&1; then
            PGO_WHY="the training run collected no counts -- see work/pgo-train.log"
            echo "    the training run collected no counts (see work/pgo-train.log)."
            echo "    Keeping the module you already have: it is correct, just slower."
        elif [ -z "$DRAWS" ] || [ "$DRAWS" -lt 10 ] 2>/dev/null; then
            echo "    the training run never reached a match (draws/frame=${DRAWS:-none},"
            PGO_WHY="the training run never reached a match (no save data)"
            echo "    frames=${TRAINED:-0} of 6000). Almost certainly NO SAVE DATA:"
            echo "    with none, the game parks on the memory-card screen and the"
            echo "    training route never gets into a fight. Measured on this route:"
            echo "    84 draw calls a frame with a save, 2.4 without."
            echo
            echo "    PLAY THE GAME ONCE, far enough that it writes save data, then"
            echo "    run --pgo again. Your save lives in userdata/GC/ and is picked"
            echo "    up automatically."
            echo
            echo "    Keeping the module you already have -- a profile trained on a"
            echo "    menu builds something SLOWER than no profile at all."
        elif [ -z "$PROFDATA_TOOL" ]; then
            PGO_WHY="llvm-profdata is not installed, so the counts could not be merged"
            echo "    counts were collected but llvm-profdata is not installed, so they"
            echo "    cannot be merged. Install it (SteamOS: pacman -S llvm) and re-run."
        else
            "$PROFDATA_TOOL" merge -output="$HERE/work/local.profdata" "$PROFDIR"/*.profraw
            echo "    rebuilding with your own profile ..."
            rm -rf "$HERE/work/build"
            if cmake -S "$HERE/module-src" -B "$HERE/work/build" -GNinja \
                     -DCMAKE_BUILD_TYPE=Release \
                     -DCMAKE_C_COMPILER="$CC" \
                     -DCMAKE_C_FLAGS="-march=$MARCH" \
                     -DMODULE_PGO_PROFILE="$HERE/work/local.profdata" \
                     "${PSQ_FAST[@]}" \
                     -DGAME_ID="$DISC_ID" \
                     -DGENERATED_DIR="$HERE/work/out/generated" \
                     -DDOLRECOMP_SRC="$DEPS/dolrecomp-src" \
                     -DGXRUNTIME_INC="$DEPS/gxruntime-include" \
                     -DCHASSIS_ABI_DIR="$DEPS/chassis-abi" \
                     -DMODULE_TEMPLATE="$DEPS/module-template" >"$HERE/work/pgo-use.log" 2>&1 \
               && cmake --build "$HERE/work/build" >>"$HERE/work/pgo-use.log" 2>&1
            then
                cp "$HERE/work/build/g${DISC_ID}_recomp.so" "$HERE/bin/"
                PGO_STATE="self-trained"; PGO_WHY=""
                echo "    done -- built with a profile trained on your own machine."
            else
                PGO_WHY="the profiled rebuild failed -- see work/pgo-use.log"
                echo "    the profiled rebuild failed (see work/pgo-use.log)."
                echo "    Keeping the module you already have."
            fi
        fi
    else
        PGO_WHY="the instrumented build failed -- see work/pgo-gen.log"
        echo "    the instrumented build failed (see work/pgo-gen.log)."
        echo "    Keeping the module you already have."
    fi
    rm -rf "$HERE/work/build-gen" "$PROFDIR" "$HERE/work/pgo-user"
  fi
fi

# The module you just built is also the one you copy to a Steam Deck -- the Deck
# package ships no module and its README sends you here to make one. SteamOS is
# around glibc 2.37, and a binary runs FORWARD across glibc versions but never
# backward, so a module whose floor is higher fails at dlopen on the Deck with
# "version GLIBC_x.y not found" -- after the whole extract-and-recompile above.
#
# Nothing else can catch this. The packaging scripts check artifacts that live in
# the repo; this module is built here, on your machine, so no release gate ever
# sees it. That is how a single fmod call quietly raised the floor to 2.38 on
# every current distro until 2026-08-12.
#
# Checked here or nowhere: the module is built on the PLAYER's machine, so no
# release gate ever sees it. That is how a single fmod call quietly raised the
# floor to 2.38 on every current distro until 2026-08-12.
MODULE="$HERE/bin/g${DISC_ID}_recomp.so"
deck_ok=1

floor=""
if command -v objdump >/dev/null 2>&1; then
    floor="$(objdump -T "$MODULE" 2>/dev/null |
             grep -o 'GLIBC_[0-9][0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -1)"
fi
# sort -V puts the newer version last, so the floor is too high exactly when it
# sorts after 2.37 and is not 2.37 itself.
if [ -n "$floor" ] && [ "$floor" != "2.37" ] &&
   [ "$(printf '%s\n2.37\n' "$floor" | sort -V | tail -1)" = "$floor" ]; then
    deck_ok=0
    glibc_problem="needs glibc $floor; SteamOS has about 2.37"
    glibc_fix="build on an older distro (Debian 12 or Ubuntu 22.04)"
fi

# The instruction-set half. -march=native targets THIS CPU; anything above
# x86-64-v3 is not executable on the Deck's Zen 2 and shows up as an instant
# crash rather than a load error, so it is worth naming before it happens.
if [ "$MARCH" = "native" ] &&
   grep -qE '^flags.*\b(avx512[a-z0-9]*|avx_vnni|avxvnni|amx_tile)\b' /proc/cpuinfo 2>/dev/null; then
    deck_ok=0
    isa_problem="this CPU has instructions the Deck's Zen 2 does not (AVX-512 or AVX-VNNI)"
    isa_fix="re-run with  ./setup.sh --deck \"$ISO\""
fi

if [ "$DECK" = 1 ] && [ "$deck_ok" = 0 ]; then
    echo
    echo "    ============================================================"
    echo "    THIS MODULE WILL NOT RUN ON A STEAM DECK."
    echo "    You asked for --deck, so this is a failure, not a note:"
    [ -n "${glibc_problem:-}" ] && echo "      - $glibc_problem"
    [ -n "${glibc_problem:-}" ] && echo "        fix: $glibc_fix"
    [ -n "${isa_problem:-}" ] && echo "      - $isa_problem"
    [ -n "${isa_problem:-}" ] && echo "        fix: $isa_fix"
    echo "    It plays fine on THIS machine -- setup has otherwise succeeded."
    echo "    ============================================================"
elif [ "$deck_ok" = 0 ]; then
    echo
    problem="${glibc_problem:-}"
    if [ -n "${isa_problem:-}" ]; then
        [ -n "$problem" ] && problem="$problem; "
        problem="$problem$isa_problem"
    fi
    echo "    NOTE: this module is built for this machine and will not run on a"
    echo "          Steam Deck ($problem)."
    echo "          If you want one for the Deck: ./setup.sh --deck \"$ISO\""
    [ -n "${glibc_problem:-}" ] && echo "          ...and $glibc_fix, which --deck cannot do for you."
elif [ "$DECK" = 1 ]; then
    echo
    echo "    Deck-compatible: -march=x86-64-v3, glibc floor ${floor:-unknown}."
    # Do the copy rather than describing it. The two-command dance was pure
    # error surface: forget one and the package is a module without a disc, or
    # a disc without a module, and the launcher can only say "incomplete".
    #
    # A Deck package is recognised by shape -- a runtime and a README, and NO
    # setup.sh, since that is what makes it the prebuilt one. Exactly one
    # sibling qualifying: install into it. None, or several: say what to do and
    # let the player choose, because guessing which one they meant is worse
    # than asking.
    deck_pkgs=()
    for cand in "$HERE"/../*/; do
        [ -d "$cand" ] || continue
        [ -x "$cand/bin/moderngekko-run" ] || continue
        [ -f "$cand/README.txt" ] || continue
        [ -f "$cand/setup.sh" ] && continue
        deck_pkgs+=("$cand")
    done
    if [ "${#deck_pkgs[@]}" = 1 ]; then
        target="$(cd "${deck_pkgs[0]}" && pwd)"
        echo "    Installing into $(basename "$target") ..."
        rm -rf "$target/game"
        cp -r "$HERE/game" "$target/game"
        cp "$HERE/bin/g${DISC_ID}_recomp.so" "$target/bin/"
        echo "    done: game/ and g${DISC_ID}_recomp.so are in $(basename "$target")."
        echo "    Play it with:  cd $target && ./RingOut"
    else
        echo "    Copy game/ and bin/g${DISC_ID}_recomp.so into the Deck package:"
        echo "      cp -r $HERE/game <deck-package>/"
        echo "      cp $HERE/bin/g${DISC_ID}_recomp.so <deck-package>/bin/"
    fi
fi

# SHADER WARM-UP. The first launch compiles Dolphin's ubershaders before the
# game starts (the runtime forces WaitForShadersBeforeStarting on), and with an
# empty cache that is a black window for ~2 minutes on a 2-core laptop:
# 142-154 s to frame 2000 cold against 21 s warm (A4-9125, RADV, n=6 vs n=3,
# 2026-10-02). Booting the game for 10 frames here builds that cache -- Dolphin's
# in userdata/Cache and the driver's in ~/.cache -- so the wait happens during
# setup, where waiting is expected, and the first real launch reached frame 2000
# in 22.5-22.8 s (n=3). On a desktop it costs a few seconds. Fight speed is the
# same either way; starting without the wait was tried too and cost the fight
# 16% on that laptop, because the compile then runs during play.
#
# The cache belongs to this machine's GPU driver, so a --deck build skips it:
# the Deck has to build its own. It goes through ./RingOut rather than the
# runtime directly, because the launcher is what keeps the bundled libraries
# from shadowing the host's Mesa -- run bare, a window may get no Vulkan driver.
# RINGOUT_DETERMINISM_LOG arms the frame limit (see the --pgo stage above);
# without it the game would just keep running.
if [ "$DECK" != 1 ] && [ -z "${RINGOUT_NO_SHADER_WARMUP:-}" ]; then
    echo "==> shaders"
    if ls "$HERE"/userdata/Cache/Shaders/*-uber-ps-*.cache >/dev/null 2>&1; then
        echo "    already built - skipped."
    elif [ -z "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; then
        echo "    no display here - the first launch will build them instead."
    else
        echo "    starting the game once to build its shader cache (a few seconds on a"
        echo "    desktop, ~2 minutes on a slow laptop). A window opens and closes."
        mkdir -p "$HERE/work"
        rm -f "$HERE/work/shader-warmup-frames.log"
        warm_start=$SECONDS
        RINGOUT_LOG_NAME=shader-warmup.log \
        RINGOUT_DETERMINISM_LOG="$HERE/work/shader-warmup-frames.log" \
        RINGOUT_DETERMINISM_NOHASH=1 \
        RINGOUT_DETERMINISM_FRAMES=10 \
          timeout 900 "$HERE/RingOut" >/dev/null 2>&1 || true
        if ls "$HERE"/userdata/Cache/Shaders/*-uber-ps-*.cache >/dev/null 2>&1; then
            echo "    done in $((SECONDS - warm_start)) s."
        else
            # A running copy of the game refuses the launch before any log opens.
            echo "    did not finish - not a problem, the first launch will build them."
            echo "    (is Ring Out already running? details, if any: userdata/shader-warmup.log)"
        fi
    fi
fi

# The game's own artwork, taken from the disc you supplied. None of it ships in
# this package -- it belongs to the publisher, so it is extracted here on your
# machine, exactly as the module above is. art/banner.png is the disc banner;
# art/icon.png is the memory-card icon, which only exists once you have saved,
# so this is worth re-running after you have played.
echo "==> artwork"
python3 "$HERE/tools/gc-art.py" "$HERE" || true

# A desktop entry, so the game appears in menus with its own icon rather than
# as a shell script. Written with absolute paths because the package is run
# from wherever it was unpacked, not installed to a prefix.
ICON="$HERE/art/icon.png"
[ -f "$ICON" ] || ICON="$HERE/art/banner.png"
if [ -f "$ICON" ]; then
    cat > "$HERE/RingOut.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=Ring Out
Comment=SOULCALIBUR II, statically recompiled
Exec=$HERE/RingOut
Icon=$ICON
Terminal=false
Categories=Game;
DESKTOP
    chmod +x "$HERE/RingOut.desktop"
    echo "    RingOut.desktop -> $(basename "$ICON")"
fi

# THE PROFILE VERDICT, said once, at the end, where it cannot be scrolled past.
# Everything above about the profile is a STATUS line in the middle of a 20-
# minute build, and nobody scrolls back through a thousand lines of ninja output
# to find out whether a flag took effect. The failure this guards against is
# silent by nature: an unprofiled module is CORRECT, boots, plays, and is simply
# 10-14% slower forever.
echo
echo "-----------------------------------------------------------------------"
case "$PGO_STATE" in
  self-trained)
    echo "  Profile:  YES -- trained on this machine, with your own clang."
    echo "            The best case: measured 1.2% AHEAD of the profile that"
    echo "            ships, because it matches your compiler exactly."
    ;;
  shipped)
    echo "  Profile:  YES -- using the profile that ships for $DISC_ID."
    echo "            Worth 10-14% of CPU time against no profile at all."
    ;;
  mismatch)
    echo "  Profile:  PARTLY -- the shipped profile does not fit your compiler."
    echo
    echo "  Why:      $PGO_WHY."
    echo
    echo "  Costs:    the unmatched code builds unprofiled. A profile is worth"
    echo "            10-14% of CPU time; with clang 23 against a clang-22"
    echo "            profile about two thirds of it was lost. The module is"
    echo "            still CORRECT and plays identically."
    if [ "$PGO" != 1 ]; then
    echo
    echo "  Fix:      ./setup.sh --pgo $(basename "$ISO")"
    echo "            Trains a profile with YOUR clang (6000 frames of play) and"
    echo "            rebuilds with it. ~11 min extra on a desktop, once."
    echo "            Needs llvm-profdata (Arch: pacman -S llvm)."
    fi
    ;;
  *)
    echo "  Profile:  NO -- this module is UNPROFILED."
    echo
    echo "  Why:      ${PGO_WHY:-no profile was available for this build}."
    echo
    echo "  Costs:    10-14% of CPU time, forever. On a Steam Deck that is"
    echo "            the difference between holding 60 fps and not, in the"
    echo "            heavier stages. Nothing else is affected: the module is"
    echo "            CORRECT, plays identically, and saves are unaffected."
    if [ "$PGO" != 1 ]; then
      if "$CC" --version 2>&1 | grep -qi clang; then
    echo
    echo "  Fix:      ./setup.sh --pgo $(basename "$ISO")"
    echo "            Builds it again, trains a profile by playing 6000 frames,"
    echo "            and rebuilds with it. ~11 min extra on a desktop, once."
    echo "            Needs llvm-profdata (Arch: pacman -S llvm)."
      else
    echo
    echo "  Fix:      install clang, then re-run. PGO here is clang-only."
      fi
    fi
    ;;
esac
echo "-----------------------------------------------------------------------"

echo
echo "Setup complete. Run ./RingOut to play."
