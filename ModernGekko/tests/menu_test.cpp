// The in-game menu, driven the way a player drives it: keystrokes in, settings
// out. It had no automated coverage at all.
//
// Not because it is hard to test -- RecompMenu exposes Toggle/OnKey/OnEscape and
// the actions happen there, not in Draw() -- but because the obvious way in is
// through the window, and scripted input does not reach it: the determinism
// harness injects PAD input, while the menu is keyboard-driven, and xdotool
// cannot reach an XWayland surface KWin owns. So every route a test might take
// looked blocked, and the programmatic one went unnoticed.
//
// No ImGui context, no window, no core. Toggle() hands the pause to a worker
// that no-ops unless Core::IsRunning, and the row lists for the static tabs are
// compile-time constants, so navigation works with nothing booted.

#include "VideoCommon/RecompMenu.h"

#include "Common/Config/Config.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "UICommon/UICommon.h"
#include "VideoCommon/VideoConfig.h"

#include <filesystem>

namespace fs = std::filesystem;
using Key = RecompMenu::Key;

int main()
{
  const fs::path root = fs::temp_directory_path() / "moderngekko-menu-test";
  fs::remove_all(root);
  fs::create_directories(root);
  UICommon::SetUserDirectory(root.string());
  UICommon::Init();

  // Tabs are System, Video, Audio, Controls, Cheats, and Toggle() opens on
  // System with the tab selector (row 0) focused. Declaration order is the
  // on-screen order, so these counts are the same ones a player presses.
  constexpr int kSystemToVideo = 1;
  constexpr int kSystemToAudio = 2;

  if (RecompMenu::IsOpen())
    return 1;

  // Keys while CLOSED must do nothing. OnKey returns early on !open, and a
  // regression there would let a stray keypress reconfigure the game while
  // somebody is playing it.
  const bool widescreen_at_rest = Config::Get(Config::GFX_WIDESCREEN_HACK);
  RecompMenu::OnKey(Key::Down);
  RecompMenu::OnKey(Key::Right);
  RecompMenu::OnKey(Key::Activate);
  if (Config::Get(Config::GFX_WIDESCREEN_HACK) != widescreen_at_rest)
    return 2;

  // Open, close with Escape, open again.
  RecompMenu::Toggle();
  if (!RecompMenu::IsOpen())
    return 3;
  RecompMenu::OnEscape();
  if (RecompMenu::IsOpen())
    return 4;
  RecompMenu::Toggle();
  if (!RecompMenu::IsOpen())
    return 5;

  // VIDEO tab, first row: Widescreen.
  for (int i = 0; i < kSystemToVideo; ++i)
    RecompMenu::OnKey(Key::Right);
  RecompMenu::OnKey(Key::Down);
  RecompMenu::OnKey(Key::Right);

  if (Config::Get(Config::GFX_WIDESCREEN_HACK) == widescreen_at_rest)
    return 6;
  // The hack and the aspect mode are ALWAYS flipped together. The hack alone
  // widens the projection while the output stays 4:3; the aspect mode alone
  // stretches the image instead of widening the field of view. Either half on
  // its own is a visible bug, so the pairing is asserted, not assumed.
  if (Config::Get(Config::GFX_ASPECT_RATIO) != AspectMode::ForceWide)
    return 7;

  RecompMenu::OnKey(Key::Right);
  if (Config::Get(Config::GFX_WIDESCREEN_HACK) != widescreen_at_rest)
    return 8;
  if (Config::Get(Config::GFX_ASPECT_RATIO) != AspectMode::Auto)
    return 9;

  // AUDIO tab, second row: Muted. A different tab and a different settings
  // system, so this catches a navigation change that happens to leave the
  // video path working.
  RecompMenu::OnEscape();
  RecompMenu::Toggle();
  for (int i = 0; i < kSystemToAudio; ++i)
    RecompMenu::OnKey(Key::Right);
  const bool muted_before = Config::Get(Config::MAIN_AUDIO_MUTED);
  RecompMenu::OnKey(Key::Down);   // row 1: Volume
  RecompMenu::OnKey(Key::Down);   // row 2: Muted
  RecompMenu::OnKey(Key::Right);
  if (Config::Get(Config::MAIN_AUDIO_MUTED) == muted_before)
    return 10;

  // Escape backs out of the menu, and keys are inert again afterwards.
  RecompMenu::OnEscape();
  if (RecompMenu::IsOpen())
    return 11;
  const bool muted_after = Config::Get(Config::MAIN_AUDIO_MUTED);
  RecompMenu::OnKey(Key::Right);
  if (Config::Get(Config::MAIN_AUDIO_MUTED) != muted_after)
    return 12;

  UICommon::Shutdown();
  fs::remove_all(root);
  return 0;
}
