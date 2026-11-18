// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/RecompGuest.h"

#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <utility>

#include "Common/FileUtil.h"
#include "Core/HW/Memmap.h"
#include "Core/RecompGuestCore.h"
#include "Core/RecompGuestTables.h"
#include "Core/System.h"
#include "DiscIO/DirectoryBlob.h"

namespace RecompGuest
{
namespace
{
std::mutex s_mutex;
std::string s_game_root;
std::string s_hash;
std::string s_unavailable = "not initialised";
std::atomic<bool> s_on{false};
std::atomic<std::uint32_t> s_module_groups{0};        // the module's staticrecomp_gate_groups
std::vector<RecompGuestTables::SpawnPatch> s_patches;  // built from the manifest; read on the CPU thread
RecompGuestTables::BootPatcher s_patcher;             // CPU thread only
std::atomic<bool> s_sites_armed{false};               // s_on && the tables are in RAM
RecompGuestTables::Installed s_installed;             // from the manifest
int s_force_id = 0;                                   // RINGOUT_SPAWN_FORCE_ID, for testing before the grid

// P1's character when the select screen is skipped (the arcade route's default, 800AE8CC writes 22).
constexpr std::uint32_t kP1DefaultCharacter = 0x8034D550;

std::string ReadAll(const std::string& path)
{
  std::ifstream f(path, std::ios::binary);
  if (!f)
    return {};
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

std::string ResolveHostFile(const std::string& host_path)
{
  if (!s_on.load())
    return host_path;
  std::lock_guard lock(s_mutex);
  return RecompGuestCore::Resolve(host_path, true, s_game_root);
}
}  // namespace

void Initialize(const std::string& game_root, const std::string& root_sha1)
{
  std::lock_guard lock(s_mutex);
  s_game_root = game_root;
  s_hash.clear();
  s_patches.clear();
  s_installed = {};
  s_on = false;
  const std::string archive = RecompGuestCore::ArchivePath(game_root);
  const std::uint64_t size = File::Exists(archive) ? File::GetSize(archive) : RecompGuestCore::kNoFile;
  RecompGuestCore::Manifest m;
  const std::string text = ReadAll(RecompGuestCore::ManifestPath(game_root));
  if (text.empty() && size == RecompGuestCore::kNoFile)
  {
    if (File::Exists(RecompGuestCore::OldManifestPath(game_root)))
    {
      s_unavailable = RecompGuestCore::Describe(RecompGuestCore::Check::OldFormat);
      std::fprintf(stderr, "[guests] unavailable: %s\n", s_unavailable.c_str());
    }
    else
    {
      s_unavailable = "no guests installed";  // the normal case: say nothing
    }
  }
  else if (const auto c = RecompGuestCore::Validate(text, root_sha1, size, &m); c != RecompGuestCore::Check::Ok)
  {
    s_unavailable = RecompGuestCore::Describe(c);
    std::fprintf(stderr, "[guests] unavailable: %s\n", s_unavailable.c_str());
  }
  else
  {
    s_hash = m.guests_sha1;
    s_patches = RecompGuestTables::MakeUsPatches(m);
    s_installed = RecompGuestTables::InstalledFrom(m);
    s_unavailable.clear();
    std::fprintf(stderr, "[guests] available (%.12s):%s%s\n", s_hash.c_str(), s_installed.spawn ? " Spawn" : "",
                 s_installed.heihachi ? " Heihachi" : "");
  }
  DiscIO::SetHostFileResolver(&ResolveHostFile);
}

bool IsAvailable()
{
  std::lock_guard lock(s_mutex);
  return !s_hash.empty();
}

const std::string& GuestsHash()
{
  return s_hash;
}

const RecompGuestTables::Installed& InstalledGuests()
{
  return s_installed;
}

const std::string& Unavailable()
{
  return s_unavailable;
}

bool ModuleRunsGuests(std::uint32_t groups)
{
  std::lock_guard lock(s_mutex);
  if (!(groups & kSitesGateGroups))
    return false;
  return !s_installed.heihachi || (groups & kHeihachiGateGroups);
}

std::vector<std::string> IntroMovies()
{
  std::lock_guard lock(s_mutex);
  std::vector<std::string> out;
  if (!s_unavailable.empty())
    return out;
  for (const auto& [installed, name] : {std::pair{s_installed.spawn, "intro-spawn.sfd"},
                                        std::pair{s_installed.heihachi, "intro-heihachi.sfd"}})
  {
    const std::string path = s_game_root + "/guests/" + name;
    if (installed && File::Exists(path))
      out.push_back(path);
  }
  return out;
}

void SetModuleGateGroups(std::uint32_t groups)
{
  s_module_groups = groups;
}

bool ModuleHasSites()
{
  return ModuleRunsGuests(s_module_groups.load());
}

void SetSession(bool on)
{
  const bool sites = ModuleHasSites();  // after Initialize: which guests are installed decides it
  if (on && IsAvailable() && !sites)
    std::fprintf(stderr, "[guests] installed, but the game module was built without all of their code sites: "
#ifdef _WIN32
                         "run setup.ps1 again with your GameCube disc and -Spawn or -Heihachi\n");
#else
                         "run setup.sh again with your GameCube disc and --spawn or --heihachi\n");
#endif
  s_on = on && IsAvailable() && sites;
  s_patcher.Reset();
  s_sites_armed = false;
  const char* const force = std::getenv("RINGOUT_SPAWN_FORCE_ID");
  s_force_id = force ? std::atoi(force) : 0;
  std::fprintf(stderr, "[guests] session: %s\n", s_on ? "on" : "off");
}

bool IsOn()
{
  return s_on.load();
}

bool SitesArmed()
{
  return s_sites_armed.load(std::memory_order_relaxed);
}

void OnFrame(Core::System& system)
{
  if (!s_on.load())
    return;
  auto& memory = system.GetMemory();
  if (memory.GetRAM() == nullptr)
    return;
  const RecompGuestTables::ReadFn read = [&](std::uint32_t a) { return memory.Read_U8(a); };
  const RecompGuestTables::WriteFn write = [&](std::uint32_t a, std::uint8_t v) { memory.Write_U8(v, a); };
  if (const auto* r = s_patcher.OnFrame(true, s_patches, read, write); r && s_patcher.Done())
  {
    using RecompGuestTables::PatchResult;
    if (r->result == PatchResult::Mismatch)
      std::fprintf(stderr, "[guests] tables NOT patched: %s holds unexpected bytes (another build?)\n", r->what);
    else
      std::fprintf(stderr, "[guests] tables %s\n",
                   r->result == PatchResult::Applied ? "patched" : "already patched");
  }
  s_sites_armed = s_patcher.Applied();
  // A save from before Spawn's stats hooks carries their damage in player 2's controller settings
  // (RecompGuestTables.h RepairP2Pad). Checked every frame: the save loads after the tables.
  if (s_patcher.Applied() && RecompGuestTables::RepairP2Pad(read, write))
    std::fprintf(stderr, "[guests] repaired player 2's controller settings (damage from an older build)\n");
  if (s_force_id > 0 && s_patcher.Applied())
  {
    const u16 id = memory.Read_U16(kP1DefaultCharacter);
    if (id != s_force_id && id > 0 && id < 34)
      memory.Write_U16(static_cast<u16>(s_force_id), kP1DefaultCharacter);
  }
}
}  // namespace RecompGuest
