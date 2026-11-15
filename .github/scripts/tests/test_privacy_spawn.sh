#!/bin/bash
# The privacy scan refuses Spawn's converted data wherever it lands in a stage, and passes the converter
# that ships (tools/spawn and tools/guests, *.py and dspenc.c, without tests).
#
#   bash .github/scripts/tests/test_privacy_spawn.sh
set -u
REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
SCAN="$REPO/.github/scripts/privacy-scan.sh"
fail=0
check() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: want '$2', got '$3'"; fail=1; fi; }
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
stage() { rm -rf "$T/s"; mkdir -p "$T/s/tools/spawn" "$T/s/tools/guests" "$T/s/tools/heihachi"; cp "$REPO"/tools/spawn/*.py "$T/s/tools/spawn/"; cp "$REPO"/tools/heihachi/*.py "$T/s/tools/heihachi/"; cp "$REPO"/tools/guests/*.py "$REPO"/tools/guests/dspenc.c "$T/s/tools/guests/"; }

stage; bash "$SCAN" "$T/s" >/dev/null 2>&1; check "the shipped converter passes" 0 $?
stage; : > "$T/s/root-spawn.olk"; bash "$SCAN" "$T/s" >/dev/null 2>&1; check "root-spawn.olk refused" 1 $?
stage; : > "$T/s/tools/root-spawn.json"; bash "$SCAN" "$T/s" >/dev/null 2>&1; check "root-spawn.json refused" 1 $?
stage; mkdir -p "$T/s/x/spawn.new"; bash "$SCAN" "$T/s" >/dev/null 2>&1; check "a half-built spawn.new refused" 1 $?
stage; : > "$T/s/root-guests.olk"; bash "$SCAN" "$T/s" >/dev/null 2>&1; check "root-guests.olk refused" 1 $?
stage; : > "$T/s/tools/guests.json"; bash "$SCAN" "$T/s" >/dev/null 2>&1; check "guests.json refused" 1 $?
stage; mkdir -p "$T/s/x/guests.new"; bash "$SCAN" "$T/s" >/dev/null 2>&1; check "a half-built guests.new refused" 1 $?
exit $fail
