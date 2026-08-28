// The CHEATS tab is fed by GameSettings/<disc id>.ini, and which discs get one
// is a PACKAGING decision -- so this asserts the plumbing that decision rests
// on, for every region the port now supports.
//
// The gap it was written for: Dolphin loads game inis from its Sys directory
// AND the user directory, but the release ships no Sys tree, so Dolphin's own
// GRSPAF.ini (177 PAL codes) never reached a player. A US disc got 23 codes and
// a European one got an empty tab, with nothing anywhere saying why.
//
// Also asserts that a shipped list has nothing ENABLED. v1.2 shipped five codes
// switched on because they happened to be on in the developer's working copy;
// the packaging strip fixes that, and nothing verified it from the runtime side.

#include "Core/Cheats/ActionReplay.h"
#include "Core/Config/ConfigManager.h"
#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "UICommon/UICommon.h"

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
int CountCodes(const std::string& disc_id)
{
  const Common::IniFile global = SConfig::LoadDefaultGameIni(disc_id, std::nullopt);
  const Common::IniFile local = SConfig::LoadLocalGameIni(disc_id, std::nullopt);
  return static_cast<int>(ActionReplay::LoadCodes(global, local).size());
}

int CountEnabled(const std::string& disc_id)
{
  const Common::IniFile global = SConfig::LoadDefaultGameIni(disc_id, std::nullopt);
  const Common::IniFile local = SConfig::LoadLocalGameIni(disc_id, std::nullopt);
  int enabled = 0;
  for (const auto& code : ActionReplay::LoadCodes(global, local))
    enabled += code.enabled ? 1 : 0;
  return enabled;
}
}  // namespace

int main()
{
  const fs::path root = fs::temp_directory_path() / "moderngekko-cheats-test";
  fs::remove_all(root);
  fs::create_directories(root);
  // Through UICommon, not File::SetUserPath directly: Dolphin derives
  // D_GAMESETTINGS_IDX from the user directory, and setting the leaf by hand
  // leaves the rest of the path table uninitialised.
  UICommon::SetUserDirectory(root.string());
  UICommon::Init();
  const std::string game_settings = File::GetUserPath(D_GAMESETTINGS_IDX);
  fs::create_directories(game_settings);

  // Counts are measured as DELTAS against a baseline, never as absolutes.
  // Dolphin also loads its own Sys/GameSettings, which carries 177 codes for
  // GRSPAF -- present in a source tree, absent from a shipped package, so an
  // absolute assertion here would pass in one and fail in the other while the
  // code under test behaved identically. What is being asserted is the
  // contribution of the USER file, which is the half a release controls.
  const int pal_baseline = CountCodes("GRSPAF");
  const int usa_baseline = CountCodes("GRSEAF");
  const int jpn_baseline = CountCodes("GRSJAF");

  const auto write_pal = [&game_settings](bool enable_one) {
    std::ofstream ini(game_settings + "GRSPAF.ini");
    ini << "[ActionReplay]\n"
           "$MG Test Infinite Time\n"
           "03EC2BE3 18000000\n"
           "$MG Test All Levels\n"
           "043A3694 00000063\n"
           "[ActionReplay_Enabled]\n";
    if (enable_one)
      ini << "$MG Test Infinite Time\n";
  };

  // Two codes, one of them switched on -- the state the packaging strip exists
  // to prevent, so a later "nothing enabled" result cannot be mistaken for
  // "nothing loaded".
  write_pal(true);
  if (CountCodes("GRSPAF") != pal_baseline + 2)
    return 2;
  if (CountEnabled("GRSPAF") != 1)
    return 3;

  // The same list as a release ships it: still listed, none enabled.
  write_pal(false);
  if (CountCodes("GRSPAF") != pal_baseline + 2)
    return 4;
  if (CountEnabled("GRSPAF") != 0)
    return 5;

  // Each disc ID reads its OWN file. A PAL list leaking into a US or Japanese
  // session would be worse than an empty tab: the codes would look right and
  // point at unrelated memory.
  if (CountCodes("GRSEAF") != usa_baseline)
    return 6;
  if (CountCodes("GRSJAF") != jpn_baseline)
    return 7;

  // A disc with no user ini is not an error, just an empty contribution.
  fs::remove(game_settings + "GRSPAF.ini");
  if (CountCodes("GRSPAF") != pal_baseline)
    return 8;

  UICommon::Shutdown();
  fs::remove_all(root);
  return 0;
}
