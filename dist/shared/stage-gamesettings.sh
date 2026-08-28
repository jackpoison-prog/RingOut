#!/bin/bash
# Stages the cheat/game-settings inis into a release, with nothing enabled.
#
#   stage-gamesettings.sh <repo root> <destination dir>
#
# ONE copy, called by both packagers, for the reason gc-art.py lives here too:
# a second copy drifts, and this one carries a rule that is easy to get subtly
# wrong (ship the list, ship nothing switched on).
#
# Sources, and why they differ:
#   GRSEAF.ini  authored by this project. Dolphin has never shipped a USA ini
#               for this game, so the CHEATS tab was empty on a US disc until
#               these were written.
#   GRSPAF.ini  Dolphin's own, 177 codes. Dolphin loads game inis from its Sys
#               directory as well as the user directory, but no release ships a
#               Sys tree -- so those codes existed and reached nobody.
#   GRSJAF.ini  authored by this project from a genuinely Japanese code list,
#               2026-08-26. Every code was decrypted and checked to write into
#               the JAPANESE executable's own memory before shipping -- see the
#               file's header. A list circulating as "Japanese" turned out to be
#               the USA list relabelled, so provenance here is checked, not
#               assumed.
set -u
REPO="${1:?repo root}"
DEST="${2:?destination directory}"

SRC_PKG="$REPO/dist/RingOut-1.0-dist"
mkdir -p "$DEST"

usa=""
for cand in "$SRC_PKG/userdata/GameSettings/GRSEAF.ini" \
            "$REPO/work/mg_userdir/GameSettings/GRSEAF.ini"; do
  [ -s "$cand" ] && { usa="$cand"; break; }
done
if [ -n "$usa" ]; then
  install -m 644 "$usa" "$DEST/GRSEAF.ini"
else
  echo "  WARN: no GRSEAF.ini found; a US disc will have an empty CHEATS tab" >&2
fi

pal="$REPO/ModernGekko/vendor/dolphin/Data/Sys/GameSettings/GRSPAF.ini"
if [ -s "$pal" ]; then
  install -m 644 "$pal" "$DEST/GRSPAF.ini"
else
  echo "  WARN: no GRSPAF.ini found; a PAL disc will have an empty CHEATS tab" >&2
fi

jpn=""
for cand in "$SRC_PKG/userdata/GameSettings/GRSJAF.ini" \
            "$REPO/work/mg_userdir/GameSettings/GRSJAF.ini"; do
  [ -s "$cand" ] && { jpn="$cand"; break; }
done
if [ -n "$jpn" ]; then
  install -m 644 "$jpn" "$DEST/GRSJAF.ini"
else
  echo "  WARN: no GRSJAF.ini found; a Japanese disc will have an empty CHEATS tab" >&2
fi

# SHIP THE LIST WITH NOTHING ENABLED, whatever the developer's working copy has
# on. v1.2 shipped five codes enabled -- Infinite Time, both Infinite Healths
# and P2 Play As Inferno -- because they were on locally. Latent rather than
# broken, since EnableCheats defaults off, but the first player to turn cheats
# on would have got unendable matches against a forced Inferno.
python3 - "$DEST"/*.ini <<'STRIP'
import re, sys
for path in sys.argv[1:]:
    text = open(path).read()
    # Empty every enabled section, leaving the section headers in place so the
    # CHEATS tab still has somewhere to write the player's own choices.
    cleaned = re.sub(r"(\[(?:ActionReplay|Gecko)_Enabled\]\n)(?:\$[^\n]*\n)*", r"\1", text)
    open(path, "w").write(cleaned)
    listed = len([l for l in cleaned.splitlines() if l.startswith("$")])
    enabled = 0
    for section in re.findall(r"\[(?:ActionReplay|Gecko)_Enabled\]\n((?:\$[^\n]*\n)*)", cleaned):
        enabled += len([l for l in section.splitlines() if l.startswith("$")])
    print(f"    {path.split('/')[-1]}: {listed} codes listed, {enabled} enabled")
    if enabled:
        raise SystemExit(f"  FAIL: {path} still has {enabled} enabled codes")
STRIP
