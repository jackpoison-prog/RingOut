// SPDX-License-Identifier: GPL-2.0-or-later
//
// RingOut: texture mods, kept separate from the HD texture pack.
//
// A "mod" here is one folder of replacement textures under Load/Mods/<name>/.
// It is exactly the same file format as an HD pack -- the separation is about
// PRECEDENCE and about being able to turn one off without moving files: where
// a mod and the HD pack replace the same texture, the mod wins.
//
// Everything in this file is host-side. No mod listed here reaches guest RAM,
// so none of it affects determinism or netplay. Character skins that patch
// root.olk are a different tier entirely and are NOT handled here -- see
// RecompGameData.h.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace RecompMods
{
// What a mod folder turned out to contain. Players download a .rar from
// GameBanana without much idea which tier it is, so the folder is classified on
// sight rather than assumed to be usable.
enum class Kind
{
  // Replacement textures (.png/.dds). Host-side, toggleable, netplay-safe.
  Textures,
  // A .dtp -- a whole root.olk entry, i.e. a character skin. Cannot be applied
  // from here: it is a patch INTO the game archive, only valid when its size
  // matches the slot exactly, and olkviewer is the tool for it.
  GameData,
  // Extracted, but nothing recognisable in it.
  Unknown,
};

struct Mod
{
  std::string name;  // folder name under Load/Mods/, or the archive's basename
  std::string path;  // absolute path to that folder
  bool enabled = false;
  Kind kind = Kind::Unknown;
  // Why this cannot simply be switched on, when it cannot. Shown in the menu.
  std::string note;

  // GameData only. A skin can be dropped straight into root.olk when its size
  // matches exactly one entry; anything else needs the container rebuilt and is
  // left to olkviewer. `installed` means those bytes are already in place.
  bool installable = false;
  bool installed = false;
  std::string payload;            // the .dtp inside the mod folder
  std::uint64_t slot_offset = 0;  // absolute, into root.olk
  std::uint32_t slot_size = 0;
};

// Folders under Load/Mods/, in a stable order (sorted by name), each carrying
// its saved enabled state. Rescans the directory on every call so a folder
// dropped in while the game runs shows up on the next menu open.
// Unpacks any archive sitting in Load/Mods/ first: a mod arrives from
// GameBanana as one .rar (or .zip/.7z), and requiring players to unpack it into
// a correctly named folder by hand is the step most of them would get wrong.
// The mod's name is the archive's basename.
std::vector<Mod> Scan();

// Enabled mod directories, highest precedence first. The texture loader walks
// this before the HD pack, and takes the first copy of any given texture.
std::vector<std::string> EnabledDirectories();

// Persists to Config/Mods.ini. Unknown names are remembered too, so disabling a
// mod, deleting it and putting it back keeps the choice.
// Queues a skin to be written into root.olk. Deliberately queued and not done
// here: root.olk is open and mapped for the whole session, so the launcher does
// it before the next boot -- the same route the game-data restore takes.
bool RequestInstall(const Mod& mod, std::string* message);

void SetEnabled(const std::string& name, bool enabled);
bool IsEnabled(const std::string& name);

// Bumped by SetEnabled. The texture cache reloads when this moves: toggling a
// mod changes which files win without changing any VideoConfig value, so
// without a counter to compare there is nothing for OnConfigChanged to notice
// and the switch would appear to do nothing until the next launch.
std::uint32_t Generation();
}  // namespace RecompMods
