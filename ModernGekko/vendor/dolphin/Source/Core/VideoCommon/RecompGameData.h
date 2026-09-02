// SPDX-License-Identifier: GPL-2.0-or-later
//
// RingOut: is the extracted game data still the data that came off the disc?
//
// This is the second tier of mod support and it is a different problem from
// RecompMods. Character skins from GameBanana do not add files anywhere the
// emulator looks -- they rewrite root.olk in place, with a third-party tool
// (olkviewer), on disk, before the game ever starts. There is nothing to
// toggle and no folder to scan; the only honest thing to do is notice.
//
// Noticing matters because of netplay. The existing compatibility fingerprint
// hashes main.dol, and a skin patch does not touch main.dol -- so two peers
// running different skins agreed on every check and then desynced in play,
// with nothing on screen explaining why. root.olk reaches guest RAM, so its
// contents are part of what has to match.

#pragma once

#include <string>

namespace RecompGameData
{
enum class Status
{
  // No baseline recorded -- an install predating disc-origin.txt, or a game
  // directory assembled by hand. Reported as-is rather than guessed at.
  Unknown,
  Pristine,
  Modified,
};

struct State
{
  Status status = Status::Unknown;
  std::string current;   // digest of the root.olk on disk now
  std::string pristine;  // digest recorded when setup.sh extracted the disc
  // Whether the original can actually be put back, and from what. False when
  // the player's disc image has moved or been deleted since setup.sh ran.
  bool restorable = false;
  std::string restore_source;
  std::string detail;  // one line, written for the menu to show verbatim
};

// Hashes root.olk under game_root, caching the result against the file's size
// and mtime -- the file is ~590 MB and rehashing it on every launch would be
// felt. Safe to call before the video backend exists.
void Initialize(const std::string& game_root, const std::string& user_directory);

State Get();

// Absolute path of the game's root.olk, or empty before Initialize. Mods need
// it to work out whether a skin has a slot to go in.
std::string RootOlkPath();

// Mixed into the netplay compatibility fingerprint. "unknown" when no hash
// could be taken, which still differs between a modded and an unmodded peer
// only if both could be read -- so callers must treat Unknown as unsafe rather
// than as a match.
std::string Fingerprint();

// True when netplay must be refused: the game data differs from the disc, so
// this peer will desync against an unmodified one.
bool NetplayBlocked();

// Reversal. The restore itself is deliberately NOT done here: root.olk is open
// and mapped while the game runs, and rewriting 590 MB underneath a live
// session is not something a menu should do. This records the intent and the
// launcher performs it before the next boot.
bool RequestRestore(std::string* message);
bool RestoreRequested();
void CancelRestore();
}  // namespace RecompGameData
