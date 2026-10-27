#!/bin/bash
# Fail a release stage that carries anything personal.
#
#   privacy-scan.sh <staged-package-dir>
#
# package-deck.sh already asserts that the DISC, the SAVE CARD and the MODULE
# are absent -- that is the copyright axis. This is the other axis: a package
# can be perfectly free of disc data and still ship the builder's username,
# home directory layout, LAN addressing or a credential. Those come from
# different places than the game files do, so they need their own check:
#
#   * absolute build paths baked into binaries by the compiler or by cmake
#     (-ffile-prefix-map neutralises the compiler's, but generated files,
#     cmake_install.cmake, DartConfiguration.tcl and .cmake caches are NOT
#     covered and have carried /home/<user>/... in this project before)
#   * source shipped for GPL compliance -- a `git format-patch` carries the
#     author's name and email, and a hand-written patch can carry a hardcoded
#     developer path. A REAL instance: ModernGekko-dolphin-local-changes.patch
#     shipped `/home/<user>/Desktop/soulcalibur/work/fmv` as a default.
#   * a working userdata/ copied in wholesale -- Dolphin's config holds a
#     netplay nickname, RetroAchievements credentials, ISO paths and logs.
#
# Exit status is the point: non-zero means DO NOT PUBLISH.
#
# Env:
#   PRIVACY_EXTRA_ALLOW=<regex>  additional egrep pattern for known-benign hits
set -uo pipefail

DIR="${1:?usage: privacy-scan.sh <staged-package-dir>}"
[ -d "$DIR" ] || { echo "no such directory: $DIR" >&2; exit 2; }

# Known-benign, and each one needs a reason to be here.
#
#  - Dolphin vendors Triforce/ALL.Net defaults that are literal 192.168.x
#    strings in the binary. Upstream data, not the builder's network.
#  - The project's own PUBLIC repo URL is deliberate. Anchored to /RingOut
#    rather than the whole org: the private development repo is RingOutRecomp,
#    and an org-wide allow would wave that through as readily as the public one.
#    \b stops it matching RingOutRecomp while still allowing /RingOut/... .
#  - /home/deck and /home/user are generic; they name no individual and appear
#    in SteamOS-facing docs and defaults.
ALLOW='ALL\.Net|CyCraft|Key of Avalon|MarioKart|namcam|github\.com/jackpoison-prog/RingOut\b|/home/deck|/home/user\b'
[ -n "${PRIVACY_EXTRA_ALLOW:-}" ] && ALLOW="$ALLOW|$PRIVACY_EXTRA_ALLOW"

# Where this repository is checked out, regex-escaped. BUILD_ROOT= overrides it
# for a stage assembled elsewhere.
BUILD_ROOT="${BUILD_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." 2>/dev/null && pwd)}"
BUILD_ROOT_RE=""
if [ -n "$BUILD_ROOT" ] && [ "$BUILD_ROOT" != "/" ]; then
  # '/' is not a regex metacharacter; escaping it makes grep warn
  # "stray \ before /" once per file scanned.
  BUILD_ROOT_RE="$(printf '%s' "$BUILD_ROOT" | sed 's/[][\.*^$(){}?+|]/\\&/g')"
fi

# Two tiers, because the same string means different things in different files.
#
# ALWAYS: a build path, an author email or a credential is the builder's data
# wherever it turns up, including compiled into a binary -- that is precisely
# how this project's known leak travelled.
#
# THE BUILDER'S OWN CHECKOUT PATH is in here too, derived rather than listed.
# The home-path patterns below only catch a leak that happens to live under
# /home or /Users; this project's checkout is at /mnt/... and sailed through
# every scan for three releases. It is still shipping inside the three PGO
# profiles, because LLVM keys a static function as "<source path>;<symbol>" and
# that is inherent to the format -- three strings per profile, naming a
# directory layout and a mount, no username. Deriving the pattern from wherever
# this script lives means the check works on any machine that cuts a release,
# including one whose checkout is somewhere neither of us predicted.
PATTERNS_ALWAYS=(
  '/home/[a-z0-9_-]+/'            # linux home paths
  '/Users/[A-Za-z0-9_-]+/'        # macOS home paths
  'C:\\\\Users\\\\[A-Za-z0-9_-]+'  # windows home paths
  'ghp_[A-Za-z0-9]{20,}'          # github tokens
  'github_pat_[A-Za-z0-9_]{20,}'
  'AKIA[0-9A-Z]{16}'              # aws
  'xox[baprs]-[A-Za-z0-9-]{10,}'  # slack
  # The PRIVATE development repo. Not a personal identifier, but it must never
  # reach a published package, and it did: every Deck zip up to and including
  # 1.5.2 gave this as the GPL written offer in CREDITS.txt, pointing the one
  # URL a Deck owner needs to exercise their source rights at a 404. Every one
  # of those releases passed this scan, because nothing here was looking for it.
  #
  # In PATTERNS_ALWAYS, not PATTERNS_TEXT: this is a fixed literal that cannot
  # collide with upstream binary data, so it is worth checking the binaries for
  # too. (This said PATTERNS_TEXT was dead code, which it was when written --
  # declared and never read. It is wired in now, below.)
  #
  # The BARE name, not "<org>/<name>": every Windows exe from 1.6 to the first
  # 1.6.10 build carried 462 copies of the CI checkout path D:\a\<name>\<name>\
  # (MSVC-style __FILE__), with no org in front, so the org-qualified pattern
  # passed all of them. One more copy is UTF-16, hence the -el pass below.
  'RingOutRecomp'
)
# TEXT ONLY: in a config file a private address is the user's own network and a
# PEM header is a real key. In the runtime binary both are upstream Dolphin
# data -- it vendors Triforce/ALL.Net LAN defaults (192.168.x, 10.0.1.x) and
# PEM templates for Wii certificate handling. Failing on those would train
# everyone to ignore the scan.
PATTERNS_TEXT=(
  '[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}' # email addresses
  '192\.168\.[0-9]+\.[0-9]+'      # private LAN
  '10\.[0-9]+\.[0-9]+\.[0-9]+'
  '172\.(1[6-9]|2[0-9]|3[01])\.[0-9]+\.[0-9]+'
  'ssh-(rsa|ed25519|dss)'
  'BEGIN [A-Z ]*PRIVATE KEY'
)

# Files that must never appear at all, whatever their contents.
FORBIDDEN_NAMES=(
  '.git' '.gitconfig' '.ssh' 'id_rsa' 'id_ed25519' '.netrc' '.npmrc'
  '.env' '.aws' 'RetroAchievements.ini' 'dolphin.log' 'TimePlayed.ini'
  '*.gci' '*.iso' '*.sav' '*.raw'
  # The extracted disc. Not privacy, but it is the other thing that must never
  # ship, and rejecting the directory here also keeps 989 MB of game data out
  # of the identifier scan -- where it produces chance matches such as an
  # AWS-key-shaped run of characters inside game/files/root.olk.
  'game'
)

fail=0

echo "==> forbidden files"
for n in "${FORBIDDEN_NAMES[@]}"; do
  found="$(find "$DIR" -name "$n" 2>/dev/null | head -5)"
  if [ -n "$found" ]; then
    echo "  FAIL: $n present:"; echo "$found" | sed 's/^/    /'
    fail=1
  fi
done
[ "$fail" = 0 ] && echo "  none"

echo "==> personal identifiers"
# ONLY first-party content is scanned for identifiers, and that is deliberate.
# The bundled support libraries are stock Debian binaries: libcurl carries its
# maintainer's email, libgcrypt has "ssh-rsa" as a protocol identifier, gnutls
# has "BEGIN PRIVATE KEY" as a PEM template, and libunistring happens to
# contain the substring AKIAISLANDISSHARITAL in its Unicode tables. Every one
# of those trips a credential pattern and none of them is the builder's data.
# Scanning them turns the report into noise nobody reads, which is worse than
# not scanning at all. Their integrity is a supply-chain question, not a
# privacy one -- they come from the pinned container image.
#
# So: skip third-party trees, scan everything this project produces.
hits=0
while IFS= read -r f; do
  case "$f" in
    */lib/*|*/toolchain/*|*llvm-mingw*|*/Externals/*) continue ;;
    # Disc- and save-derived blobs are rejected by name above; running text
    # patterns over their binary content only produces gibberish matches.
    *.gci|*.iso|*.raw|*.sav|*.dol|*.uidcache) continue ;;
    */game/*) continue ;;
  esac

  # A .tar.gz is opaque to `strings`, and the GPL source shipment is nothing
  # BUT tarballs -- so the one directory whose contents most need checking was
  # the one directory the scan could not read. It shipped a committed .pyc
  # carrying an upstream author's macOS home path in every desktop package;
  # found 2026-09-11 by scanning the git tree instead, which is not something a
  # release should depend on remembering to do.
  #
  # Streamed through `strings` into grep rather than captured in a variable:
  # these archives are ~20 MB compressed and far larger open, and command
  # substitution would both hold all of that in memory and warn on every null
  # byte in what is mostly binary content.
  case "$f" in
    *.tar.gz|*.tgz|*.tar)
      # PATTERNS_ALWAYS only, NOT the text set. These archives are vendored
      # upstream source, which is the same category as the runtime binary: a
      # private address in Dolphin's own tree is its Triforce LAN default and an
      # email is a copyright line, not the builder's data. Running the text
      # patterns here reported ~40 such hits across two tarballs and buried the
      # one line that mattered.
      #
      # It is still the right net: the fault this was written for was a
      # committed .pyc holding an upstream author's macOS home path, and home
      # paths are in PATTERNS_ALWAYS precisely because they identify a person
      # wherever they appear. (Not quoted here -- writing the literal into this
      # file would publish the very string the scan exists to keep out, and this
      # script is public.)
      for p in "${PATTERNS_ALWAYS[@]}" ${BUILD_ROOT_RE:+"$BUILD_ROOT_RE"}; do
        # Same third-party skips the file loop applies, for the same reason:
        # the vendored Externals are stock upstream sources and carry their
        # authors' emails, Dolphin's Triforce LAN defaults and mbedtls PEM
        # templates. Reporting those buries the one line that matters.
        m="$({ tar xz --exclude='*/Externals/*' --exclude='*/Data/*' -Of "$f" 2>/dev/null ||
               tar x --exclude='*/Externals/*' --exclude='*/Data/*' -Of "$f" 2>/dev/null; } |
             strings -a | grep -aE "$p" | grep -avE "$ALLOW" |
             grep -aEo "$p" | sort -u | head -3)"
        if [ -n "$m" ]; then
          echo "  FAIL: $(realpath --relative-to="$DIR" "$f" 2>/dev/null || echo "$f") (inside the archive)"
          echo "$m" | sed 's/^/    /'
          hits=1
        fi
      done
      continue
      ;;
  esac

  pats=("${PATTERNS_ALWAYS[@]}")
  [ -n "$BUILD_ROOT_RE" ] && pats+=("$BUILD_ROOT_RE")
  # The text-only set, which the comment above PATTERNS_TEXT describes and
  # which nothing added to this list until 2026-09-10. Every release up to and
  # including 1.5.2 was scanned with HALF its patterns: no email address, no
  # private LAN address, no ssh key and no PEM private-key header was looked
  # for, in any file. The array was written, documented and then never read.
  #
  # -I is what makes the distinction the comment asks for: grep treats a binary
  # file as non-matching, so this asks "does the file contain text at all" and
  # is false for the runtime binary, where these same patterns are upstream
  # Dolphin data rather than the builder's.
  if LC_ALL=C grep -qI . "$f" 2>/dev/null; then
    pats+=("${PATTERNS_TEXT[@]}")
  fi

  for p in "${pats[@]}"; do
    m="$(strings -a "$f" 2>/dev/null | grep -aE "$p" | grep -avE "$ALLOW" |
         grep -aEo "$p" | sort -u | head -3)"
    if [ -n "$m" ]; then
      echo "  FAIL: $(realpath --relative-to="$DIR" "$f" 2>/dev/null || echo "$f")"
      echo "$m" | sed 's/^/    /'
      hits=1
    fi
  done

  # UTF-16LE strings, which `strings -a` cannot see: Windows binaries keep wide
  # paths (wide __FILE__ in asserts, resource strings). The ALWAYS set only --
  # these are the patterns that mean the builder's data in a binary.
  case "$f" in
    *.exe|*.dll|*.pdb|*.pgd)
      for p in "${PATTERNS_ALWAYS[@]}" ${BUILD_ROOT_RE:+"$BUILD_ROOT_RE"}; do
        m="$(strings -a -el "$f" 2>/dev/null | grep -aE "$p" | grep -avE "$ALLOW" |
             grep -aEo "$p" | sort -u | head -3)"
        if [ -n "$m" ]; then
          echo "  FAIL: $(realpath --relative-to="$DIR" "$f" 2>/dev/null || echo "$f") (UTF-16)"
          echo "$m" | sed 's/^/    /'
          hits=1
        fi
      done
      ;;
  esac
done < <(find "$DIR" -type f 2>/dev/null)
[ "$hits" = 0 ] && echo "  none" || fail=1

echo
if [ "$fail" != 0 ]; then
  echo "PRIVACY SCAN FAILED -- do not publish this stage." >&2
  exit 1
fi
echo "privacy scan clean: $DIR"
