#!/bin/bash
# Ring Out : first-time setup
#
# Builds your personal copy from a GameCube disc image you already have. Nothing
# game-derived ships with this package -- this script extracts the disc and
# recompiles its executable here, on your machine.
#
# Usage:  ./setup.sh [--deck] /path/to/your/disc.iso
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
for arg in "$@"; do
    case "$arg" in
        --deck) DECK=1 ;;
        *)      [ -z "$ISO" ] && ISO="$arg" ;;
    esac
done

if [ -z "$ISO" ] || [ ! -f "$ISO" ]; then
    echo "Usage: ./setup.sh [--deck] /path/to/your/disc.iso"
    echo
    echo "Supply a GameCube disc image you already have."
    echo "  --deck   build a module that also runs on a Steam Deck"
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
if [ -n "$missing" ]; then
    echo "Missing required build tools:$missing"
    echo "Install them and re-run. On Arch:  sudo pacman -S cmake ninja clang python"
    echo "On Debian/Ubuntu:  sudo apt install cmake ninja-build clang python3"
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

echo "==> 1/3  Extracting disc"
rm -rf "$HERE/game"
"$HERE/tools/dolrecomp" extract "$ISO" "$HERE/game"

DISC_ID="$(head -c 6 "$HERE/game/sys/boot.bin" 2>/dev/null || true)"
if [ -z "$DISC_ID" ]; then
    echo "Could not read a disc ID -- is that a GameCube disc image?"
    exit 1
fi
echo "    disc id: $DISC_ID"

echo "==> 2/3  Recompiling the game executable (several minutes)"
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
"$HERE/tools/dolrecomp" --gamecube "$HERE/game/sys/main.dol" --idle-pc auto \
    -j"$(nproc)" "$HERE/work/out"
# gen_module_tables.py reads main.dol from alongside the generated sources.
cp "$HERE/game/sys/main.dol" "$HERE/work/out/generated/main.dol"

echo "==> 3/3  Building the module"
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
PGO_ARGS=()
if [ -f "$HERE/module-src/module.profdata" ] && "$CC" --version 2>&1 | grep -qi clang; then
    PGO_ARGS=(-DMODULE_PGO_PROFILE="$HERE/module-src/module.profdata")
    echo "    profile-guided build (this takes longer, and is worth it)"
fi
cmake -S "$HERE/module-src" -B "$HERE/work/build" -GNinja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER="$CC" \
      -DCMAKE_C_FLAGS="-march=$MARCH" \
      "${PGO_ARGS[@]}" \
      -DGAME_ID="$DISC_ID" \
      -DGENERATED_DIR="$HERE/work/out/generated" \
      -DDOLRECOMP_SRC="$DEPS/dolrecomp-src" \
      -DGXRUNTIME_INC="$DEPS/gxruntime-include" \
      -DCHASSIS_ABI_DIR="$DEPS/chassis-abi" \
      -DMODULE_TEMPLATE="$DEPS/module-template"
cmake --build "$HERE/work/build"

cp "$HERE/work/build/g${DISC_ID}_recomp.so" "$HERE/bin/"

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
    echo "    Copy game/ and bin/g${DISC_ID}_recomp.so into the Deck package."
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

echo
echo "Setup complete. Run ./RingOut to play."
