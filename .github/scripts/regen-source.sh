#!/bin/bash
# Regenerates the GPL source shipment in dist/RingOut-1.0-dist/source/.
#
#   ./.github/scripts/regen-source.sh
#
# Env:
#   DEST=dir   where to write (default dist/RingOut-1.0-dist/source)
#
# WHY A SCRIPT. This shipment was hand-made once, in July, and then went three
# weeks stale while the binaries moved on -- which both breaks the GPL offer
# (the source must match the binaries shipped) and is how a developer's home
# path from an old working tree survived into a published patch. Making it
# reproducible is the fix; package-dist.sh refuses to package when source/ is
# older than the runtime, and this is the thing that clears that.
#
# HOW THE SHIPMENT IS SHAPED. Each component is a git checkout pinned at its
# upstream commit, with this project's modifications left UNCOMMITTED on top.
# So for each one:
#
#   <name>-<short>.tar.gz          the upstream tree, straight from git archive
#   <name>-local-changes.patch     git diff -- every modification made here
#   <name>-new-files.tar.gz        files this project added (untracked upstream)
#
# and applying the patch plus the new files to the archive reproduces exactly
# what was built. Submodules are deliberately absent from git archive output,
# which is why vendor/dolphin ships as its own component rather than nested.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DEST="${DEST:-$REPO/dist/RingOut-1.0-dist/source}"

# name:path -- the name is what appears in CREDITS.txt.
COMPONENTS=(
  "ModernGekko:$REPO/ModernGekko"
  "ModernGekko-dolphin:$REPO/ModernGekko/vendor/dolphin"
  "DolRecomp:$REPO/DolRecomp"
)

mkdir -p "$DEST"
# Clear only what this script owns, so a stale artifact from an older pin cannot
# survive alongside a new one and be mistaken for current.
rm -f "$DEST"/*.tar.gz "$DEST"/*.patch

echo "==> regenerating source shipment"
declare -a CREDIT_LINES=()

for entry in "${COMPONENTS[@]}"; do
  name="${entry%%:*}"
  path="${entry#*:}"
  [ -d "$path/.git" ] || [ -f "$path/.git" ] || { echo "  FAIL: $path is not a git checkout" >&2; exit 1; }

  # Pinned length, NOT bare --short. Git auto-sizes the abbreviation from the
  # object count, so a plain fetch into the vendored Dolphin clone silently
  # moved this from 7 characters to 10 -- renaming the GPL archive, and leaving
  # CREDITS.txt naming a file that no longer exists. The name a licence offer
  # points at cannot depend on how many objects the builder happens to have.
  short="$(git -C "$path" rev-parse --short=12 HEAD)"
  full="$(git -C "$path" rev-parse HEAD)"
  echo "  $name @ $full"

  # Upstream tree at the pinned commit. Deterministic: same commit, same bytes.
  git -C "$path" archive --format=tar.gz --prefix="$name-$short/" HEAD \
      > "$DEST/$name-$short.tar.gz"

  # Modifications to tracked files.
  if git -C "$path" diff --quiet; then
    echo "    no local modifications"
  else
    git -C "$path" diff > "$DEST/$name-local-changes.patch"
    echo "    $(git -C "$path" diff --shortstat | sed 's/^ *//')"
  fi

  # Files this project added. --exclude-standard honours .gitignore, which is
  # what keeps build trees and the disc image out of the shipment.
  mapfile -t newfiles < <(git -C "$path" ls-files --others --exclude-standard)
  if [ "${#newfiles[@]}" -gt 0 ]; then
    tar -czf "$DEST/$name-new-files.tar.gz" -C "$path" "${newfiles[@]}"
    echo "    ${#newfiles[@]} new files"
  fi

  CREDIT_LINES+=("  $name-$short.tar.gz")
done

echo
echo "==> privacy check"
# The known-real failure this guards: a hardcoded developer path inside a patch.
"$REPO/.github/scripts/privacy-scan.sh" "$DEST"

echo
echo "==> CREDITS.txt must name these archives:"
printf '%s\n' "${CREDIT_LINES[@]}"
echo
echo "current CREDITS.txt names:"
grep -oE '[A-Za-z-]+-[0-9a-f]{7}\.tar\.gz' "$REPO/dist/RingOut-1.0-dist/CREDITS.txt" |
  sed 's/^/  /' | sort -u
