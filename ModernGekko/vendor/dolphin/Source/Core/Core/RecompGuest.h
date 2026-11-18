// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Core
{
class System;
}

// Guest characters on the US disc (see RecompGuestCore.h for what the archive is and why it lives in
// <game>/guests/). This unit decides, once per boot, whether guests are on, and while they are, serves
// their archive wherever the disc reads root.olk. Off -- the default whenever the data is missing or
// stale -- nothing here touches the disc or guest RAM.
namespace RecompGuestTables
{
struct Installed;
}

namespace RecompGuest
{
// Reads <game_root>/guests/guests.json and checks it against `root_sha1` (RecompGameData's hash
// of the installed root.olk). Logs why when the data cannot be used. Call before any boot.
void Initialize(const std::string& game_root, const std::string& root_sha1);

bool IsAvailable();
// The manifest's guests_sha1, or empty when unavailable: what two netplay peers compare.
const std::string& GuestsHash();
// Which guests the archive holds; none when unavailable.
const RecompGuestTables::Installed& InstalledGuests();
// The reason guests are unavailable, for the menu and the log.
const std::string& Unavailable();

// The installed guests' own opening movies (each disc's cut of the intro, copied by setup beside the
// archive), Spawn's first: host paths of those present. Empty when guests are unavailable. The movie
// player shows them in turn with the game's own; nothing else reads them.
std::vector<std::string> IntroMovies();

// Decided once, before boot. Ignored (stays off) when guests are not available.
void SetSession(bool on);

// The guests' code sites are gate groups 1 (both guests) and 2 (Heihachi's six), compiled into the
// module only by setup.sh --spawn or --heihachi (a module without them costs every other US player
// nothing). The module says which groups it has in its staticrecomp_gate_groups export. Guests stay
// off without group 1, and without group 2 when Heihachi is installed (a module built for Spawn alone,
// before Heihachi): their tables in RAM with no site ever firing hang or mis-select them.
// Set before SetSession; nothing until set.
inline constexpr std::uint32_t kSitesGateGroups = 1u << 1;
inline constexpr std::uint32_t kHeihachiGateGroups = 1u << 2;
void SetModuleGateGroups(std::uint32_t groups);
// Whether a module with these gate groups has every code site the installed guests need.
bool ModuleRunsGuests(std::uint32_t groups);
bool ModuleHasSites();
bool IsOn();

// CPU thread, once per frame: writes the table rows once the DOL is in RAM (RecompGuestTables.h).
// Nothing while off. RINGOUT_SPAWN_FORCE_ID=<id> (testing) also holds P1's default character.
void OnFrame(Core::System& system);

// True while guests are on AND their table rows are in RAM: only then does a decision at a code
// site (RecompGuestTables.h kUsSites) need the hook. The run loop arms gate group 1 from this.
bool SitesArmed();
}  // namespace RecompGuest
