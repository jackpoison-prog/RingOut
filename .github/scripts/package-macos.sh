#!/bin/bash
# package-macos.sh -- assemble RingOut-<VERSION>-macos-arm64.zip. Runs on the
# macOS CI runner (macos.yml), after the runtime, dolrecomp and MoltenVK are
# built and the pinned toolchain is unpacked. Nobody here has a Mac, so this is
# never run by hand; every input is passed in:
#
#   BUILD      the CMake build dir (moderngekko-run, dolrecomp, MoltenVK)
#   LLVM_DIR   the unpacked LLVM release (the clang the US profile is trained with)
#   CMAKE_DIR  CMake.app/Contents of the unpacked CMake release
#   NINJA      the ninja binary (NINJA_LICENSE: its COPYING, optional)
#   OUT        where the zip goes (default: dist/)
#
# Allowlist-built like package-deck.sh: only what is named below goes in, and
# the stage is privacy-scanned before it is zipped. US disc only for now, so
# only the US profile, hot list and regions ship.
set -euo pipefail
# A CI run costs ~10x on macOS: name the line that stopped it, never exit mute.
trap 'echo "  FAIL: line $LINENO: $BASH_COMMAND" >&2' ERR

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SRC="$REPO/dist/RingOut-1.0-dist"
VERSION="$(cat "$REPO/VERSION")"
BUILD="${BUILD:?the CMake build directory}"
LLVM_DIR="${LLVM_DIR:?the unpacked LLVM release}"
CMAKE_DIR="${CMAKE_DIR:?CMake.app/Contents}"
NINJA="${NINJA:?the ninja binary}"
OUT="${OUT:-$REPO/dist}"
CLANG_PIN="clang version 22.1.8"   # the plain <ID>.profdata set; see package-windows.ps1

WORK="$OUT/_macos-stage"
STAGE="$WORK/RingOut-$VERSION-macos"
ZIP="$OUT/RingOut-$VERSION-macos-arm64.zip"
APP="$STAGE/RingOut.app/Contents"

one() { # the one file named $1 under $BUILD
    local f; f="$(find "$BUILD" -type f -path "*$1" | head -1)"
    [ -n "$f" ] || { echo "  FAIL: no $1 under $BUILD" >&2; exit 1; }
    echo "$f"
}

rm -rf "$WORK"
mkdir -p "$APP/MacOS" "$APP/Frameworks" "$APP/Resources" \
         "$STAGE/tools" "$STAGE/bin" "$STAGE/toolchain/bin" "$STAGE/toolchain/lib" \
         "$STAGE/userdata/GameSettings"

echo "==> RingOut.app"
RUNTIME="$(one /moderngekko-run)"
install -m 755 "$RUNTIME" "$APP/MacOS/moderngekko-run"
install -m 755 "$SRC/macos/RingOut" "$APP/MacOS/RingOut"
sed "s/@VERSION@/$VERSION/g" "$SRC/macos/Info.plist" > "$APP/Info.plist"
cp -R "$REPO/ModernGekko/vendor/dolphin/Data/Sys" "$APP/Resources/Sys"
install -m 755 "$(one /Package/Release/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib)" \
    "$APP/Frameworks/libMoltenVK.dylib"

embedded="$(grep -a -o -m1 'RingOut Ver [0-9.]*' "$APP/MacOS/moderngekko-run" | head -1 || true)"
if [ "$embedded" != "RingOut Ver $VERSION" ]; then
    echo "  FAIL: the runtime says '$embedded' but this package is $VERSION" >&2; exit 1
fi
for bin in "$APP/MacOS/moderngekko-run" "$APP/Frameworks/libMoltenVK.dylib"; do
    if otool -L "$bin" | tail -n +2 | grep -E '/opt/homebrew|/usr/local'; then
        echo "  FAIL: $(basename "$bin") links a library a player will not have" >&2; exit 1
    fi
done
# Apple Silicon runs nothing unsigned. The linker signed each binary ad hoc;
# the bundle as a whole is signed the same way (no Developer ID -- see README).
codesign --force --deep --sign - "$STAGE/RingOut.app"
codesign --verify --deep --strict "$STAGE/RingOut.app"
echo "    $VERSION, MoltenVK bundled, ad-hoc signed"

echo "==> recompiler"
install -m 755 "$(one /dolrecomp)" "$STAGE/tools/dolrecomp"

echo "==> toolchain"
# clang itself, its own headers and runtime libraries (lib/clang/NN: the
# profile runtime --pgo needs is there), libLTO for the module's ThinLTO link
# through Apple's linker, and llvm-profdata. Not the other ~1 GB of LLVM.
real_clang="$(cd "$LLVM_DIR/bin" && readlink clang || echo clang)"
install -m 755 "$LLVM_DIR/bin/$real_clang" "$STAGE/toolchain/bin/clang"
install -m 755 "$LLVM_DIR/bin/llvm-profdata" "$STAGE/toolchain/bin/llvm-profdata"
cp -R "$LLVM_DIR/lib/clang" "$STAGE/toolchain/lib/clang"
install -m 644 "$LLVM_DIR/lib/libLTO.dylib" "$STAGE/toolchain/lib/libLTO.dylib"
install -m 755 "$CMAKE_DIR/bin/cmake" "$STAGE/toolchain/bin/cmake"
cp -R "$CMAKE_DIR/share" "$STAGE/toolchain/share"
install -m 755 "$NINJA" "$STAGE/toolchain/bin/ninja"
# Each tool must run from here alone: nothing it links may be outside macOS
# itself (an @rpath libLLVM.dylib, say, that was not copied).
for t in clang llvm-profdata cmake ninja; do
    # -arch arm64: cmake and ninja are universal binaries, and for those otool
    # prints a "<file> (architecture x86_64):" header per slice.
    bad="$(otool -arch arm64 -L "$STAGE/toolchain/bin/$t" | grep -v ':$' | awk '{print $1}' |
           grep -vE '^(/usr/lib/|/System/)' || true)"
    [ -z "$bad" ] || { echo "  FAIL: toolchain/bin/$t needs $bad" >&2; exit 1; }
done
# Their licences travel with them (Apache-2.0 and BSD terms ask for that).
mkdir -p "$STAGE/toolchain/LICENSES"
for lic in "$LLVM_DIR/LICENSE.TXT:LLVM.txt" "${NINJA_LICENSE:-}:ninja.txt" \
           "$(find "$BUILD" -maxdepth 9 -path "*MoltenVK*" -name LICENSE | head -1):MoltenVK.txt"; do
    from="${lic%%:*}"; to="${lic##*:}"
    if [ -f "$from" ]; then cp "$from" "$STAGE/toolchain/LICENSES/$to"
    else echo "  WARN: no licence file for $to ($from)" >&2; fi
done
cmake_copy="$(find "$STAGE/toolchain/share" -maxdepth 3 -name Copyright.txt | head -1)"
[ -n "$cmake_copy" ] && cp "$cmake_copy" "$STAGE/toolchain/LICENSES/CMake.txt"
got="$("$STAGE/toolchain/bin/clang" --version | head -1)"
case "$got" in
    *"$CLANG_PIN"*) echo "    $got (matches the profiles)" ;;
    *) echo "  FAIL: bundled clang is '$got', the profiles need $CLANG_PIN" >&2; exit 1 ;;
esac

echo "==> module sources"
cp -R "$SRC/module-src" "$STAGE/module-src"
find "$STAGE/module-src/profiles" -type f ! -name 'GRSEAF.*' -delete
ls "$STAGE/module-src/profiles" | sed 's/^/    /'

echo "==> scaffolding"
install -m 755 "$SRC/setup.sh" "$STAGE/setup.sh"
install -m 755 "$SRC/macos/Setup.command" "$STAGE/Setup.command"
sed "1s/^Ring Out - Ver [0-9.]*/Ring Out - Ver $VERSION/" "$SRC/macos/README-macos.txt" > "$STAGE/README.txt"
# The Deck's model, not the Linux package's: these binaries are prebuilt and
# source/ does not ship, so the offer names the public repository.
sed "1s/^Ring Out - Ver .*/Ring Out - Ver $VERSION/" "$SRC/macos/CREDITS-macos.txt" > "$STAGE/CREDITS.txt"
cat >> "$STAGE/CREDITS.txt" <<'CREDITS'

macOS package
  RingOut.app bundles MoltenVK (Apache-2.0, The Khronos Group) to run Vulkan
  on Metal. toolchain/ is the compiler setup uses: LLVM clang and
  llvm-profdata (Apache-2.0 WITH LLVM-exception), CMake (BSD-3-Clause) and
  ninja (Apache-2.0), unmodified releases. Their licences are in
  toolchain/LICENSES/.
CREDITS
install -m 644 "$SRC/bin/README-modules.txt" "$STAGE/bin/README-modules.txt"
"$REPO/dist/shared/stage-gamesettings.sh" "$REPO" "$STAGE/userdata/GameSettings"
find "$STAGE/userdata/GameSettings" -type f ! -name 'GRSEAF.ini' -delete

echo "==> checks"
for forbidden in game work art userdata/GC userdata/Config; do
    [ -e "$STAGE/$forbidden" ] && { echo "  FAIL: $forbidden is in the stage" >&2; exit 1; }
done
if find "$STAGE" \( -name '*_recomp.dylib' -o -name '*_recomp.so' -o -name '*.gci' \
                    -o -name '*.iso' -o -name '*.rvz' \) | grep -q .; then
    echo "  FAIL: disc-derived files in the stage" >&2; exit 1
fi
offer_url="$(grep -oE 'https://github\.com/[A-Za-z0-9._/-]+' "$STAGE/CREDITS.txt" | grep -iE '/RingOut' | head -1)"
[ -n "$offer_url" ] || { echo "  FAIL: CREDITS.txt names no project source URL" >&2; exit 1; }
code="$(curl -s -o /dev/null -w '%{http_code}' -L "$offer_url")"
[ "$code" = 200 ] || { echo "  FAIL: GPL source offer $offer_url -> HTTP $code" >&2; exit 1; }
echo "    source offer $offer_url resolves"
# privacy-scan.sh is bash 4; macOS's /bin/bash is 3.2, so it runs under Homebrew's.
"${BASH4:-bash}" "$REPO/.github/scripts/privacy-scan.sh" "$STAGE"

echo "==> zipping"
rm -f "$ZIP"
( cd "$WORK" && TZ=UTC zip -X -qry "$ZIP" "$(basename "$STAGE")" )
mb=$(( $(stat -f %z "$ZIP") / 1048576 ))
echo "    $ZIP  ${mb} MB"
[ "$mb" -lt 400 ] || { echo "  FAIL: ${mb} MB is over the 400 MB budget" >&2; exit 1; }
