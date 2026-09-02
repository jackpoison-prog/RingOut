// Copyright 2026 ModernGekko Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/RecompMenu.h"

#include "VideoCommon/RecompGameData.h"
#include "VideoCommon/RecompMods.h"

#include <enet/enet.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <imgui.h>

#include <cmath>
#include <fstream>

#include "Common/Config/Config.h"
#include "Core/NetPlay/NetPlayProto.h"
#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Core/Cheats/ActionReplay.h"
#include "Core/Cheats/GeckoCode.h"
#include "Core/Cheats/GeckoCodeConfig.h"
#include "Core/Cheats/PatchEngine.h"
#include "Core/Config/CheatSettings.h"
#include "Core/Config/ConfigManager.h"
#include "Core/Config/FreeLookSettings.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/FreeLookManager.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/SI/SI.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/State.h"
#include "Core/System.h"
#include "InputCommon/ControlReference/ControlReference.h"
#include "InputCommon/ControllerEmu/Control/Control.h"
#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControllerInterface/MappingCommon.h"
#include "InputCommon/InputConfig.h"
#include "VideoCommon/AsyncRequests.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/VideoConfig.h"

namespace RecompMenu
{
namespace
{
enum class Item
{
  // Video
  Widescreen,
  InternalRes,
  AspectRatio,
  VSync,
  AntiAliasing,
  Anisotropy,
  TextureFiltering,
  TexturePacks,
  PrefetchTextures,
  ShowFPS,
  FreeCamera,
  TrainingHud,
  LensFlares,
  Filter,
  Fullscreen,
  // Audio
  Volume,
  Muted,
  AudioLatency,
  FillGaps,
  // System
  Speed,
  Overclock,
  StateSlot,
  SaveState,
  LoadState,
  AutoResume,
  NetplayMode,
  NetplayScan,
  NetplayAddress,
  NetplayPort,
  NetplayStart,
  Reset,
  Quit,
  // Shared
  Apply,
};

// Declaration order IS the on-screen tab order (Draw walks 0..kTabCount and
// Left/Right cycles modulo it), so System leads.
enum class Tab
{
  System,
  Video,
  Audio,
  Controls,
  Cheats,
  Mods,
  Count,
};

constexpr int kTabCount = static_cast<int>(Tab::Count);

// The Controls tab lists two fixed rows above the rebindable inputs -- which
// backend to take input from, and which device on it -- in the same way the
// Cheats tab puts its master switch above the codes. Rebind rows are therefore
// offset by this much when indexing control_rows.
// The CONTROLS tab's non-binding rows, in on-screen order: which port is being
// configured, its backend and device, and whether port 2 is plugged in at all.
constexpr int kPortRow = 0;
constexpr int kBackendRow = 1;
constexpr int kDeviceRow = 2;
constexpr int kPort2Row = 3;
constexpr int kControlsHeaderRows = 4;

const char* TabName(Tab tab)
{
  switch (tab)
  {
  case Tab::Video:
    return "VIDEO";
  case Tab::Audio:
    return "AUDIO";
  case Tab::System:
    return "SYSTEM";
  case Tab::Controls:
    return "CONTROLS";
  case Tab::Cheats:
    return "CHEATS";
  case Tab::Mods:
    return "MODS";
  default:
    return "";
  }
}

// Controls and Cheats build their rows dynamically, so they have no static list.
const std::vector<Item>& TabItems(Tab tab)
{
  static const std::vector<Item> video = {
      Item::Widescreen,   Item::InternalRes, Item::AspectRatio,      Item::VSync,
      Item::AntiAliasing, Item::Anisotropy,  Item::TextureFiltering, Item::TexturePacks,
      Item::PrefetchTextures, Item::ShowFPS, Item::FreeCamera,       Item::TrainingHud,
      Item::LensFlares,       Item::Filter,      Item::Fullscreen,       Item::Apply};
  static const std::vector<Item> audio = {Item::Volume, Item::Muted, Item::AudioLatency,
                                          Item::FillGaps, Item::Apply};
  static const std::vector<Item> system = {Item::Speed,        Item::Overclock,
                                           Item::StateSlot,
                                           Item::SaveState,    Item::LoadState,
                                           Item::AutoResume,   Item::NetplayMode,
                                           Item::NetplayScan,
                                           Item::NetplayAddress,
                                           Item::NetplayPort,  Item::NetplayStart,
                                           Item::Apply,        Item::Reset,
                                           Item::Quit};
  static const std::vector<Item> none = {};

  switch (tab)
  {
  case Tab::Video:
    return video;
  case Tab::Audio:
    return audio;
  case Tab::System:
    return system;
  default:
    return none;
  }
}

// One cheat code flattened for the list: either an Action Replay or a Gecko
// entry, identified by index into the matching vector.
struct CheatRow
{
  std::string label;
  bool gecko = false;
  size_t index = 0;
};

// One remappable input on the emulated GC pad, flattened out of the
// group/control tree so the page is a simple list.
struct ControlRow
{
  std::string label;
  ControllerEmu::Control* control = nullptr;
};

struct State
{
  std::mutex mutex;
  bool open = false;
  int selected = 0;
  int state_slot = 1;
  std::function<void()> fullscreen_callback;
  std::function<void()> quit_callback;

  // Netplay cannot be joined in place: the lobby runs before the core boots and
  // NetPlay_Enable happens inside NetPlayClient::StartGame, so entering a
  // session means starting one from scratch. These rows therefore compose a
  // request, and Start writes it out and quits -- the runner picks it up and
  // brings up the lobby. 0 = off, 1 = host, 2 = join.
  int netplay_mode = 0;
  int netplay_port = 2626;

  // The address to join. The TEXT is authoritative and the octets are only the
  // editing affordance, because the frontend accepts a hostname and this row
  // cannot represent one -- Tailscale's MagicDNS names are the common case.
  // Holding octets as the truth meant an unparseable address silently became
  // 127.0.0.1 the moment a session started from this menu.
  //
  // There is no text entry here and none to be had on a pad: the deck package
  // ships this menu and nothing else. So Space enters an edit mode where
  // Left/Right pick an octet and Up/Down change it -- which is why the octet
  // index lives in state, and why Up/Down must be intercepted before the
  // generic row movement while it is set. -1 means "not editing". Entering the
  // edit is what replaces a hostname, and it takes a deliberate keypress.
  std::string netplay_addr_text = "127.0.0.1";
  std::array<int, 4> netplay_addr = {127, 0, 0, 1};
  int netplay_addr_octet = -1;
  bool netplay_addr_seeded = false;

  // LAN discovery results. A host in the lobby broadcasts once a second; Space
  // on the Scan row listens for a couple of seconds and lists what answered, so
  // the address never has to be typed on the same network. Written by a worker
  // thread, hence everything here is under the same mutex as the rest.
  struct FoundHost
  {
    std::string address;
    std::string nickname;
    int port = 2626;
  };
  // -1 = nothing to write. Set when the resolution row changes, consumed by
  // OnKey after the mutex is released (this file never does file I/O under it).
  int pending_resolution_scale = -1;

  std::vector<FoundHost> netplay_hosts;
  int netplay_host_index = 0;
  bool netplay_scanning = false;
  bool netplay_scanned = false;

  // Consecutive presses in the same direction step further each time. Without
  // it an octet moves by one per press and reaching 192 from 127 is 65 of them,
  // which is not a thing anyone will do twice.
  int netplay_addr_run = 0;
  int netplay_addr_run_dir = 0;

  // Row 0 is always the tab selector; rows 1.. are that tab's entries.
  Tab tab = Tab::System;
  std::vector<ControlRow> control_rows;
  // Port the CONTROLS tab is editing: 0 or 1. Mirrored into s_config_port.
  int config_port = 0;

  // Backend and device selection, shown above the rebind rows. Qualified device
  // strings are "SOURCE/CID/NAME", so the backend is the leading field and no
  // separate list of backends has to be kept in step with this one.
  std::vector<std::string> devices;
  std::string device_current;

  // Input detection is asynchronous: Start() then Update() until IsComplete(),
  // driven from PumpFrame so the overlay keeps redrawing while we wait.
  std::unique_ptr<ciface::Core::InputDetector> detector;
  ControllerEmu::Control* detecting_control = nullptr;

  std::vector<ActionReplay::ARCode> ar_codes;
  std::vector<Gecko::GeckoCode> gecko_codes;
  std::vector<CheatRow> cheat_rows;

  // Texture mods, and whether the game data itself has been patched. Both are
  // read on entering the tab rather than held live: Scan() walks a directory
  // and this file never does I/O under the mutex.
  std::vector<RecompMods::Mod> mod_rows;
  RecompGameData::State game_data;
  // Result of the last restore request, shown in place of the hint line so the
  // player finds out whether it took without leaving the menu.
  std::string mod_message;
};

State s_state;

// Entries below the tab selector for the active tab.
int RowCount(const State& state)
{
  switch (state.tab)
  {
  case Tab::Controls:
    return kControlsHeaderRows + static_cast<int>(state.control_rows.size());
  case Tab::Cheats:
    return 1 + static_cast<int>(state.cheat_rows.size());  // master switch + codes
  case Tab::Mods:
    // HD pack switch + one row per mod + the game-data line, plus the restore
    // action only when there is something to undo.
    return 2 + static_cast<int>(state.mod_rows.size()) +
           (state.game_data.status == RecompGameData::Status::Modified ? 1 : 0);
  default:
    return static_cast<int>(TabItems(state.tab).size());
  }
}

// Flips whatever the MODS row at `index` represents. Runs with the state mutex
// held, so it records what to do rather than doing it: writing Mods.ini and
// rescanning the folder are file I/O, and this file keeps I/O out from under
// the lock. The in-memory row is flipped here anyway so the next redraw shows
// the new value instead of lagging a frame.
void ToggleModRow(int index, std::string* toggle_name, bool* toggle_enabled, bool* reload,
                  bool* save_config, int* install_mod)
{
  if (index == 0)
  {
    const bool on = !Config::Get(Config::GFX_HIRES_TEXTURES);
    Config::SetBase(Config::GFX_HIRES_TEXTURES, on);
    *save_config = true;
    *reload = true;
    return;
  }

  const int mod_index = index - 1;
  if (mod_index < 0 || mod_index >= static_cast<int>(s_state.mod_rows.size()))
    return;  // the Game data and Restore rows are not toggles

  auto& mod = s_state.mod_rows[mod_index];
  if (mod.kind == RecompMods::Kind::GameData)
  {
    // A skin is written into the game itself, so it is not a switch and cannot
    // be undone by pressing again -- removing it means restoring the game data.
    if (mod.installable && !mod.installed)
      *install_mod = mod_index;
    return;
  }
  if (mod.kind != RecompMods::Kind::Textures)
    return;  // nothing to switch; the row is showing why
  mod.enabled = !mod.enabled;
  *toggle_name = mod.name;
  *toggle_enabled = mod.enabled;
  *reload = true;
}

// Pausing must NOT happen on the host thread. CPUManager::SetStepping(true)
// blocks until the CPU thread acknowledges, but the host thread is also the
// Wayland/X11 event loop: CPU thread waits on the video thread, the video
// thread waits in Present for a swapchain image, and that needs compositor
// events which only the host thread can dispatch. Blocking it closes the loop
// and the window wedges -- which is why mashing Escape triggered it.
//
// So state changes are handed to a dedicated worker. It blocks instead, the
// host thread keeps pumping events, and the deadlock cannot form. Rapid toggles
// are latest-wins, which is exactly right for pause/resume.
// Deliberately leaked, and they must stay that way.
//
// CoreStateWorker is detached and parks in s_core_state_cv.wait() forever. If
// these were ordinary statics, process exit would destroy them while the worker
// is still waiting -- and glibc's pthread_cond_destroy blocks when a waiter is
// present. That is precisely how quitting from the in-game menu wedged the
// process: the main thread sat in exit() -> pthread_cond_destroy while the
// worker sat in pthread_cond_wait, which reads from outside as "emulation
// shutdown hangs" and has nothing to do with the CPU thread.
//
// References rather than accessor functions so the nine existing uses read
// unchanged.
std::mutex& s_core_state_mutex = *new std::mutex;
std::condition_variable& s_core_state_cv = *new std::condition_variable;
bool s_core_state_want_paused = false;
bool s_core_state_pending = false;
bool s_core_state_worker_started = false;

// Settings staged while the menu is open. Config::SetBase still writes straight
// away (so a row shows its new value immediately), but the callbacks that make a
// change actually TAKE EFFECT -- VideoConfig::Refresh and friends -- are held
// back by this guard. Applying a backend reconfigure while the core is paused is
// what used to wedge the video thread inside VKTexture::TransitionToLayout, so
// the guard is released by the worker only AFTER the resume has gone through:
// unpause first, then apply. Guarded by s_core_state_mutex.
// unique_ptr, not optional: ConfigChangeCallbackGuard is deliberately neither
// copyable nor movable, so it can only be handed around behind a pointer.
std::unique_ptr<Config::ConfigChangeCallbackGuard> s_settings_guard;

void CoreStateWorker()
{
  for (;;)
  {
    bool want_paused;
    {
      std::unique_lock<std::mutex> lock(s_core_state_mutex);
      s_core_state_cv.wait(lock, [] { return s_core_state_pending; });
      want_paused = s_core_state_want_paused;
      s_core_state_pending = false;
    }

    auto& system = Core::System::GetInstance();
    if (Core::IsRunning(system))
      Core::SetState(system, want_paused ? Core::State::Paused : Core::State::Running);

    if (!want_paused)
    {
      // Released here, off the host thread and after the core is running again.
      // Destroying the guard is what dispatches the staged config callbacks.
      // (Unconditional, so a resume requested while the core is not running
      // still flushes rather than stranding the settings forever.)
      std::unique_ptr<Config::ConfigChangeCallbackGuard> release;
      {
        std::lock_guard<std::mutex> lock(s_core_state_mutex);
        release = std::move(s_settings_guard);
      }
      release.reset();   // fires the staged callbacks, outside the lock
    }
  }
}

void RequestCoreState(bool paused)
{
  std::lock_guard<std::mutex> lock(s_core_state_mutex);
  if (!s_core_state_worker_started)
  {
    s_core_state_worker_started = true;
    std::thread(CoreStateWorker).detach();
  }
  s_core_state_want_paused = paused;
  s_core_state_pending = true;
  s_core_state_cv.notify_one();
}

bool IsSelectable(Item item)
{
  return true;
}

// Opt-in continue-where-you-left-off: the Quit row saves a state to a fixed
// file and the next boot loads it once the core is running.
const Config::Info<bool> RECOMP_AUTO_RESUME{{Config::System::Main, "RecompMenu", "AutoResume"},
                                            false};

std::string AutoResumePath()
{
  return File::GetUserPath(D_STATESAVES_IDX) + "autoresume.sav";
}

// ---- Training HUD ---------------------------------------------------------
// Live match state read straight out of guest MEM1. Anchor addresses come from
// the decrypted retail AR codes ("P1 Unlimited Health" writes f32 240.0 to
// 0x8034EA1C; "Unlimited Time" writes u8 99 to 0x8038F6D7), so health is an
// f32 with 240.0 = full bar.
constexpr u32 kHudP1Health = 0x8034EA1C;
constexpr u32 kHudTimer = 0x8038F6D7;
constexpr float kHudFullHealth = 240.0f;

const Config::Info<bool> RECOMP_TRAINING_HUD{{Config::System::Main, "RecompMenu", "TrainingHud"},
                                             false};
// P2's health has no AR-code anchor. 0x8036FABC was proven by differential
// scan during a real two-sided match: of every f32 in MEM1 at 240.0 at round
// start, only it and P1's slot ever decreased, and each moved only when its
// own fighter was hit. (First guess 0x8034EA28, "the 240.0 next to P1", never
// tracked damage.) Config-overridable; 0 hides the P2 side entirely.
const Config::Info<u32> RECOMP_HUD_P2_ADDR{{Config::System::Main, "RecompMenu", "TrainingHudP2"},
                                           0x8036FABCu};

const u8* HudGuestRam(u32* out_size)
{
  auto& system = Core::System::GetInstance();
  if (!Core::IsRunning(system))
    return nullptr;
  auto& memory = system.GetMemory();
  *out_size = memory.GetRamSizeReal();
  return memory.GetRAM();
}

u32 HudReadU32(const u8* ram, u32 ram_size, u32 addr)
{
  const u32 off = addr & 0x01FFFFFFu;
  if (ram == nullptr || off + 4 > ram_size)
    return 0;
  return (u32(ram[off]) << 24) | (u32(ram[off + 1]) << 16) | (u32(ram[off + 2]) << 8) |
         u32(ram[off + 3]);
}

float HudReadF32(const u8* ram, u32 ram_size, u32 addr)
{
  return std::bit_cast<float>(HudReadU32(ram, ram_size, addr));
}

u8 HudReadU8(const u8* ram, u32 ram_size, u32 addr)
{
  const u32 off = addr & 0x01FFFFFFu;
  if (ram == nullptr || off >= ram_size)
    return 0;
  return ram[off];
}

// Per-player damage tracking. Video-thread-only state: Draw is the sole caller.
struct HudTrack
{
  float prev = -1.0f;
  float last_hit = 0.0f;
  float combo = 0.0f;
  std::chrono::steady_clock::time_point last_hit_at{};

  void Update(float health)
  {
    const auto now = std::chrono::steady_clock::now();
    if (prev >= 0.0f && health < prev)
    {
      const float dmg = prev - health;
      last_hit = dmg;
      // Hits close together read as one combo; a gap starts a new one.
      if (now - last_hit_at <= std::chrono::milliseconds(1200))
        combo += dmg;
      else
        combo = dmg;
      last_hit_at = now;
    }
    else if (prev >= 0.0f && health > prev + 1.0f)
    {
      // Round reset / heal: start over but keep the last numbers on screen.
      combo = 0.0f;
    }
    prev = health;
  }
};

void HudBar(const char* label, float health, bool right_to_left)
{
  const float frac = std::clamp(health / kHudFullHealth, 0.0f, 1.0f);
  char text[48];
  std::snprintf(text, sizeof(text), "%s  %.0f / %.0f", label, std::max(health, 0.0f),
                kHudFullHealth);
  // Health drains toward the middle like the game's own bars: P2's fill is
  // mirrored by right-aligning a spacer before the fill.
  ImVec4 color = frac > 0.5f ? ImVec4(0.20f, 0.75f, 0.25f, 1.0f) :
                 frac > 0.25f ? ImVec4(0.85f, 0.70f, 0.15f, 1.0f) :
                                ImVec4(0.85f, 0.20f, 0.15f, 1.0f);
  ImGui::PushStyleColor(ImGuiCol_PlotHistogram, color);
  ImGui::ProgressBar(frac, ImVec2(230.0f, 16.0f), text);
  ImGui::PopStyleColor();
  (void)right_to_left;
}

void DrawTrainingHud()
{
  static const bool env_force = std::getenv("RECOMP_TRAINING_HUD") != nullptr;
  static const bool env_scan = std::getenv("RECOMP_HUD_SCAN") != nullptr;
  if (!env_force && !Config::Get(RECOMP_TRAINING_HUD))
    return;

  u32 ram_size = 0;
  const u8* ram = HudGuestRam(&ram_size);
  if (ram == nullptr)
    return;

  const float p1 = HudReadF32(ram, ram_size, kHudP1Health);
  const u32 p2_addr = Config::Get(RECOMP_HUD_P2_ADDR);
  const float p2 = p2_addr != 0 ? HudReadF32(ram, ram_size, p2_addr) : -1.0f;
  const u8 timer = HudReadU8(ram, ram_size, kHudTimer);

  if (env_scan)
  {
    // Differential health hunt (the first, exact-240-in-a-window version
    // fingered a slot that never moved — 0x8034EA28 turned out not to track
    // P2 damage). Seed on EVERY f32 in MEM1 holding exactly 240.0, then each
    // pass keep only candidates that stay in health range; the ones that also
    // DECREASE while both fighters take hits are the real health variables.
    struct Cand
    {
      u32 addr;
      float last;
      bool dropped;
    };
    static std::vector<Cand> s_cands;
    static bool s_seeded = false;
    static int s_frame = 0;
    if (++s_frame % 120 == 0)
    {
      if (!s_seeded)
      {
        // Wait for a round start: P1's known slot reads exactly 240.0 there,
        // so every other health variable is guaranteed to be at 240.0 too.
        if (p1 == kHudFullHealth)
        {
          for (u32 addr = 0x80003000u; addr < 0x80000000u + ram_size; addr += 4)
          {
            if (HudReadU32(ram, ram_size, addr) == 0x43700000u)
              s_cands.push_back({addr, kHudFullHealth, false});
          }
          s_seeded = true;
          std::fprintf(stderr, "[hud-scan] seeded %zu candidates at 240.0\n", s_cands.size());
        }
      }
      else
      {
        std::erase_if(s_cands, [&](Cand& c) {
          const float v = HudReadF32(ram, ram_size, c.addr);
          if (!(v >= 0.0f && v <= kHudFullHealth + 0.5f))
            return true;  // left health range: junk or reused memory
          if (v < c.last - 0.5f)
            c.dropped = true;
          c.last = v;
          return false;
        });
        int shown = 0;
        for (const Cand& c : s_cands)
        {
          if (c.dropped && shown++ < 16)
            std::fprintf(stderr, "[hud-scan] HEALTH-LIKE %08X now=%.1f\n", c.addr, c.last);
        }
        std::fprintf(stderr, "[hud-scan] pass: %zu candidates, p1=%.1f timer=%u\n", s_cands.size(),
                     p1, timer);
      }
    }
  }

  // Only meaningful inside a match; the health slot holds junk elsewhere.
  const bool plausible = p1 >= 0.0f && p1 <= kHudFullHealth + 1.0f;
  if (!plausible)
    return;

  static HudTrack s_track_p1, s_track_p2;
  s_track_p1.Update(p1);
  if (p2 >= 0.0f)
    s_track_p2.Update(p2);

  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + 30.0f),
                          ImGuiCond_Always, ImVec2(0.5f, 0.0f));
  ImGui::SetNextWindowBgAlpha(0.55f);
  ImGui::Begin("TrainingHud", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);

  HudBar("P1", p1, false);
  ImGui::SameLine();
  ImGui::Text(" %02u ", timer);
  if (p2 >= 0.0f)
  {
    ImGui::SameLine();
    HudBar("P2", p2, true);
  }

  ImGui::Text("last %.1f  combo %.1f", s_track_p1.last_hit, s_track_p1.combo);
  if (p2 >= 0.0f)
  {
    ImGui::SameLine(0.0f, 120.0f);
    ImGui::Text("last %.1f  combo %.1f", s_track_p2.last_hit, s_track_p2.combo);
  }
  ImGui::End();
}

const char* ItemLabel(Item item)
{
  switch (item)
  {
  case Item::Widescreen:
    return "Widescreen (16:9)";
  case Item::InternalRes:
    return "Internal Resolution";
  case Item::AspectRatio:
    return "Aspect Ratio";
  case Item::LensFlares:
    return "Lens Flares";
  case Item::Filter:
    return "Filter";
  case Item::VSync:
    return "V-Sync";
  case Item::AntiAliasing:
    return "Anti-Aliasing";
  case Item::Anisotropy:
    return "Anisotropic Filter";
  case Item::TextureFiltering:
    return "Texture Filtering";
  case Item::TexturePacks:
    return "Texture Packs";
  case Item::PrefetchTextures:
    return "Prefetch Textures";
  case Item::ShowFPS:
    return "Show FPS";
  case Item::FreeCamera:
    return "Free Camera";
  case Item::TrainingHud:
    return "Training HUD";
  case Item::Fullscreen:
    return "Fullscreen";
  case Item::Volume:
    return "Volume";
  case Item::Muted:
    return "Mute";
  case Item::AudioLatency:
    return "Audio Latency";
  case Item::FillGaps:
    return "Fill Audio Gaps";
  case Item::Speed:
    return "Emulation Speed";
  case Item::StateSlot:
    return "State Slot";
  case Item::SaveState:
    return "Save State";
  case Item::LoadState:
    return "Load State";
  case Item::AutoResume:
    return "Auto-Resume";
  case Item::Overclock:
    return "CPU Overclock";
  case Item::NetplayMode:
    return "Netplay";
  case Item::NetplayScan:
    return "Scan for Hosts";
  case Item::NetplayAddress:
    return "Join Address";
  case Item::NetplayPort:
    return "Netplay Port";
  case Item::NetplayStart:
    return "Start Netplay";
  case Item::Reset:
    return "Reset Game";
  case Item::Quit:
    return "Quit Game";
  case Item::Apply:
    return "Apply / Save Settings";
  default:
    return "";
  }
}

// Post-processing filters offered by the Filter row. Dolphin ships 48 shaders,
// which is far too many to page through one key press at a time, so this is a
// curated set: the two written for this project (Dolphin has no scanline or CRT
// filter of its own) plus the generally useful ones already present.
//
// The empty string is Dolphin's own representation of "no shader", not a
// placeholder of ours.
struct FilterEntry
{
  const char* label;
  const char* shader;
};

constexpr std::array<FilterEntry, 8> kFilters = {{
    {"Off", ""},
    {"Scanlines", "scanlines"},
    {"CRT", "crt"},
    {"FXAA", "FXAA"},
    {"Grayscale", "grayscale"},
    {"Sepia", "sepia"},
    {"Posterize", "posterize"},
    {"Invert", "invert"},
}};

// Persists the internal resolution to the frontend's config.ini.
//
// Without this the row is a lie that lasts one session: moderngekko_run does
// `config.graphics.internal_resolution_scale = frontend_config.dolphin_scale`
// unconditionally at boot, so config.ini wins and a scale chosen here is
// silently reverted on the next launch. That is how a Deck sat at 3x -- a
// 1920x1584 target on a 1280x800 panel -- with the row right there offering 2x.
//
// Written as a raw EFB multiple (640N x 528N), which LoadConfig accepts for any
// N in 1..12; the labelled list only covers 1,2,3,4,6,8,12, so the odd scales
// have no display name to use. Rewrites just the one key, leaving the rest of
// the file (nickname, netplay address, controllers) untouched.
void WriteFrontendResolution(int scale)
{
  const std::string path = File::GetUserPath(D_USER_IDX) + "config.ini";
  std::ifstream in(path);
  if (!in)
    return;
  std::vector<std::string> lines;
  std::string line;
  bool replaced = false;
  while (std::getline(in, line))
  {
    if (line.rfind("resolution=", 0) == 0)
    {
      line = "resolution=" + std::to_string(640 * scale) + "x" +
             std::to_string(528 * scale);
      replaced = true;
    }
    lines.push_back(line);
  }
  in.close();
  if (!replaced)
    return;   // no key to update: leave the file alone rather than guess a layout

  std::ofstream out(path, std::ios::trunc);
  if (!out)
    return;
  for (const std::string& l : lines)
    out << l << '\n';
}

// Seeds the join address from the frontend's config.ini, which is where the
// runner persists it after a session. Read here rather than plumbed in from the
// frontend because the platform layer that owns this menu has no view of that
// config -- and the menu already reads and writes the sibling
// netplay-request.ini out of the same directory.
//
// True when the text is a dotted quad, in which case the octets are filled in
// so the editor has something to work on. A hostname simply returns false and
// leaves the octets alone -- see the note on netplay_addr_text.
bool ParseAddress(const std::string& text, std::array<int, 4>* addr)
{
  std::array<int, 4> parsed{};
  char trailing = 0;
  // The %c rejects "1.2.3.4.5" and "1.2.3.4x", which sscanf would otherwise
  // accept by matching the first four numbers and ignoring the rest.
  if (std::sscanf(text.c_str(), "%d.%d.%d.%d%c", &parsed[0], &parsed[1], &parsed[2],
                  &parsed[3], &trailing) != 4)
  {
    return false;
  }
  if (!std::ranges::all_of(parsed, [](int v) { return v >= 0 && v <= 255; }))
    return false;
  *addr = parsed;
  return true;
}

// A hostname is perfectly valid there -- the frontend accepts one, and a
// Tailscale MagicDNS name is exactly that -- so the text is kept whatever it
// says and only ALSO parsed into octets when it happens to be a dotted quad.
// Displaying it unchanged is what stops a name the row cannot represent from
// being quietly replaced by the octets' default.
void SeedNetplayAddress(std::string* text, std::array<int, 4>* addr)
{
  std::ifstream config(File::GetUserPath(D_USER_IDX) + "config.ini");
  if (!config)
    return;
  std::string line;
  while (std::getline(config, line))
  {
    const auto eq = line.find('=');
    if (eq == std::string::npos || line.compare(0, eq, "address") != 0)
      continue;
    std::string value = line.substr(eq + 1);
    while (!value.empty() && (value.back() == '\r' || value.back() == ' '))
      value.pop_back();
    const auto first = value.find_first_not_of(' ');
    if (first == std::string::npos)
      return;
    value = value.substr(first);
    *text = value;
    ParseAddress(value, addr);   // a hostname leaves the octets at their default
    return;
  }
}

// LAN discovery, joiner side. See the beacon in tools/netplay_session.cpp --
// these three constants and the payload layout are duplicated there because
// VideoCommon cannot include a header from tools/, so changing one without the
// other silently stops hosts being found.
constexpr enet_uint16 kDiscoveryPort = 2627;
constexpr const char* kDiscoveryMagic = "RINGOUT1";
constexpr int kDiscoveryListenMs = 2500;   // beacons are 1 s apart, so this
                                           // catches two from every host

// Blocking; runs on a worker. Returns what answered within the listen window,
// one entry per address.
std::vector<State::FoundHost> ScanForHosts()
{
  std::vector<State::FoundHost> found;

  // enet_initialize is WSAStartup on Windows and a no-op elsewhere. Netplay is
  // by definition not running when this is used -- the session has not been
  // built yet -- so nothing else has done it. Never deinitialize: the pairing
  // is refcounted by Winsock and tearing it down under a later session is worse
  // than leaking one init for the life of the process.
  static std::once_flag enet_once;
  std::call_once(enet_once, [] { enet_initialize(); });

  ENetSocket socket = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
  if (socket == ENET_SOCKET_NULL)
    return found;
  // REUSEADDR so a second copy on this machine can still scan while the first
  // is bound -- two local instances is exactly how this gets tested.
  enet_socket_set_option(socket, ENET_SOCKOPT_REUSEADDR, 1);

  ENetAddress bind_address{};
  bind_address.host = ENET_HOST_ANY;
  bind_address.port = kDiscoveryPort;
  if (enet_socket_bind(socket, &bind_address) != 0)
  {
    enet_socket_destroy(socket);
    return found;
  }

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(kDiscoveryListenMs);
  while (std::chrono::steady_clock::now() < deadline)
  {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                               deadline - std::chrono::steady_clock::now())
                               .count();
    if (remaining <= 0)
      break;

    enet_uint32 condition = ENET_SOCKET_WAIT_RECEIVE;
    if (enet_socket_wait(socket, &condition, static_cast<enet_uint32>(remaining)) != 0)
      break;
    if ((condition & ENET_SOCKET_WAIT_RECEIVE) == 0)
      continue;

    char raw[256] = {};
    ENetBuffer buffer{};
    buffer.data = raw;
    buffer.dataLength = sizeof(raw) - 1;
    ENetAddress from{};
    const int received = enet_socket_receive(socket, &from, &buffer, 1);
    if (received <= 0)
      continue;

    // "RINGOUT1 <port> <nickname>". The magic is checked before anything is
    // believed -- 2627 is an ordinary UDP port and anyone may send to it.
    std::string payload(raw, static_cast<size_t>(received));
    const std::string prefix = std::string(kDiscoveryMagic) + ' ';
    if (payload.rfind(prefix, 0) != 0)
      continue;
    payload = payload.substr(prefix.size());
    const auto space = payload.find(' ');
    if (space == std::string::npos)
      continue;

    State::FoundHost host;
    host.port = std::atoi(payload.substr(0, space).c_str());
    if (host.port <= 0 || host.port > 65535)
      continue;
    host.nickname = payload.substr(space + 1);
    // The sender's address comes from the socket, so a host never has to know
    // (or lie about) which of its own interfaces the joiner can reach.
    char ip[64] = {};
    if (enet_address_get_host_ip(&from, ip, sizeof(ip)) != 0)
      continue;
    host.address = ip;

    const bool already = std::ranges::any_of(found, [&](const State::FoundHost& h) {
      return h.address == host.address;
    });
    if (!already)
      found.push_back(std::move(host));
  }

  enet_socket_destroy(socket);
  return found;
}

std::string PlainAddress(const std::array<int, 4>& addr)
{
  return std::to_string(addr[0]) + '.' + std::to_string(addr[1]) + '.' +
         std::to_string(addr[2]) + '.' + std::to_string(addr[3]);
}

// Renders the address as a.b.c.d, and while it is being edited marks the octet
// the arrows are pointing at with >< around it. Something has to say which of
// the four is live -- there is no caret to move and no highlight below row
// granularity, so the marker is in the text itself.
std::string FormatAddress(const std::array<int, 4>& addr, int editing_octet)
{
  std::string out;
  for (int i = 0; i < 4; ++i)
  {
    if (i != 0)
      out += '.';
    if (i == editing_octet)
      out += '>' + std::to_string(addr[i]) + '<';
    else
      out += std::to_string(addr[i]);
  }
  return out;
}

std::string ItemValue(Item item, int state_slot, int netplay_mode,
                      int netplay_port, const std::string& netplay_addr_text,
                      const std::array<int, 4>& netplay_addr,
                      int netplay_addr_octet,
                      const std::vector<State::FoundHost>& netplay_hosts,
                      int netplay_host_index, bool netplay_scanning,
                      bool netplay_scanned)
{
  switch (item)
  {
  case Item::NetplayMode:
    switch (netplay_mode)
    {
    case 1:
      return "HOST";
    case 2:
      return "JOIN";
    default:
      return "OFF";
    }
  case Item::Overclock:
  {
    // Netplay takes the host's factor and pushes it to every peer through its
    // settings layer, so a local value here would be silently overridden and
    // the row would be lying. It is also the wrong thing to expose there: the
    // factor rescales CoreTiming's cycle conversion, and the recompiler's cycle
    // accounting and every determinism result were validated at 1.0.
    if (NetPlay::IsNetPlayRunning())
      return "OFF (netplay)";
    if (!Config::Get(Config::MAIN_OVERCLOCK_ENABLE))
      return "OFF";
    const int percent =
        static_cast<int>(std::lround(Config::Get(Config::MAIN_OVERCLOCK) * 100.0f));
    return std::to_string(percent) + "%";
  }
  case Item::NetplayScan:
    if (netplay_mode != 2)
      return "-";
    if (netplay_scanning)
      return "SCANNING...";
    if (!netplay_scanned)
      return "SPACE TO SCAN";
    if (netplay_hosts.empty())
      return "NONE FOUND";
    return netplay_hosts[netplay_host_index].nickname + "  (" +
           std::to_string(netplay_host_index + 1) + "/" +
           std::to_string(netplay_hosts.size()) + ")";
  case Item::NetplayAddress:
    // Only a joiner dials anything; a host binds a port and waits. Showing an
    // address on the host row would suggest it decides who connects.
    if (netplay_mode != 2)
      return "-";
    // Octets only while editing; otherwise the text, which may be a hostname.
    return netplay_addr_octet >= 0
               ? FormatAddress(netplay_addr, netplay_addr_octet)
               : netplay_addr_text;
  case Item::NetplayPort:
    return netplay_mode == 0 ? "-" : std::to_string(netplay_port);
  case Item::NetplayStart:
    // Short enough to FIT: the value column is only 110*scale wide, and the
    // full sentence was clipped mid-word ("OFF - GAME DATA MO") against the
    // panel edge. The reason goes on the hint line instead, the way the MODS
    // tab already puts its sentence there.
    if (RecompGameData::NetplayBlocked())
      return "OFF - MODDED";
    // Deliberately blunt about the cost: this is not a pause-menu toggle, the
    // session has to be built from boot.
    return netplay_mode == 0 ? "-" : "RESTARTS GAME";
  case Item::Widescreen:
    return Config::Get(Config::GFX_WIDESCREEN_HACK) ? "ON" : "OFF";
  case Item::InternalRes:
  {
    // A bare "3x" says nothing about what it costs. The EFB is 640x528, so
    // showing the render target makes the choice comparable against the panel
    // the player is actually looking at -- 3x is 1920x1584 on a 1280x800 Deck,
    // three times the pixels it can display, which is not obvious from "3x".
    const int scale = Config::Get(Config::GFX_EFB_SCALE);
    return std::to_string(scale) + "x  " + std::to_string(640 * scale) + "x" +
           std::to_string(528 * scale);
  }
  case Item::AspectRatio:
    switch (Config::Get(Config::GFX_ASPECT_RATIO))
    {
    case AspectMode::ForceWide:
      return "16:9";
    case AspectMode::ForceStandard:
      return "4:3";
    case AspectMode::Stretch:
      return "Stretch";
    default:
      return "Auto";
    }
  case Item::LensFlares:
    // Dolphin issue 10475: the sun's occlusion test reads back an EFB copy, so
    // "Store EFB Copies to Texture Only" (GFX_HACK_SKIP_EFB_COPY_TO_RAM) hides
    // lens flares. Flares ON therefore means the hack is OFF, which costs some
    // performance -- hence the toggle.
    return Config::Get(Config::GFX_HACK_SKIP_EFB_COPY_TO_RAM) ? "OFF" : "ON";
  case Item::VSync:
    return Config::Get(Config::GFX_VSYNC) ? "ON" : "OFF";
  case Item::AntiAliasing:
  {
    const u32 samples = Config::Get(Config::GFX_MSAA);
    if (samples <= 1)
      return "None";
    return std::to_string(samples) + "x " + (Config::Get(Config::GFX_SSAA) ? "SSAA" : "MSAA");
  }
  case Item::Anisotropy:
    switch (Config::Get(Config::GFX_ENHANCE_MAX_ANISOTROPY))
    {
    case AnisotropicFilteringMode::Force1x:
      return "1x";
    case AnisotropicFilteringMode::Force2x:
      return "2x";
    case AnisotropicFilteringMode::Force4x:
      return "4x";
    case AnisotropicFilteringMode::Force8x:
      return "8x";
    case AnisotropicFilteringMode::Force16x:
      return "16x";
    default:
      return "Default";
    }
  case Item::TextureFiltering:
    switch (Config::Get(Config::GFX_ENHANCE_FORCE_TEXTURE_FILTERING))
    {
    case TextureFilteringMode::Nearest:
      return "Nearest";
    case TextureFilteringMode::Linear:
      return "Linear";
    default:
      return "Default";
    }
  case Item::Filter:
  {
    const std::string current = Config::Get(Config::GFX_ENHANCE_POST_SHADER);
    for (const FilterEntry& filter : kFilters)
      if (current == filter.shader)
        return filter.label;
    // Something set it to a shader outside the curated list -- show its name
    // rather than lying about it being off.
    return current.empty() ? "Off" : current;
  }
  case Item::TexturePacks:
    return Config::Get(Config::GFX_HIRES_TEXTURES) ? "ON" : "OFF";
  case Item::PrefetchTextures:
    return Config::Get(Config::GFX_CACHE_HIRES_TEXTURES) ? "ON" : "OFF";
  case Item::ShowFPS:
    return Config::Get(Config::GFX_SHOW_FPS) ? "ON" : "OFF";
  case Item::FreeCamera:
    return Config::Get(Config::FREE_LOOK_ENABLED) ? "ON" : "OFF";
  case Item::TrainingHud:
    return Config::Get(RECOMP_TRAINING_HUD) ? "ON" : "OFF";
  case Item::Volume:
    return std::to_string(Config::Get(Config::MAIN_AUDIO_VOLUME));
  case Item::Muted:
    return Config::Get(Config::MAIN_AUDIO_MUTED) ? "ON" : "OFF";
  case Item::AudioLatency:
    return std::to_string(Config::Get(Config::MAIN_AUDIO_LATENCY)) + " ms";
  case Item::FillGaps:
    return Config::Get(Config::MAIN_AUDIO_FILL_GAPS) ? "ON" : "OFF";
  case Item::Speed:
  {
    const float speed = Config::Get(Config::MAIN_EMULATION_SPEED);
    if (speed <= 0.0f)
      return "Unlimited";
    return std::to_string(static_cast<int>(speed * 100.0f + 0.5f)) + "%";
  }
  case Item::StateSlot:
    return std::to_string(state_slot);
  case Item::AutoResume:
    return Config::Get(RECOMP_AUTO_RESUME) ? "ON" : "OFF";
  default:
    return "";
  }
}

// Is a controller plugged into port 2? Reading the config rather than SI so
// this answers the same before boot and mid-session.
bool Port2Attached()
{
  return Config::Get(Config::GetInfoForSIDevice(1)) != SerialInterface::SIDEVICE_NONE;
}

// Attach or detach port 2. ChangeDevice schedules the swap through CoreTiming
// rather than doing it here, which is what makes it safe to call while the menu
// has the core paused -- the same route Movie.cpp takes when a recording
// changes what is plugged in.
void SetPort2Attached(bool attached)
{
  const SerialInterface::SIDevices device =
      attached ? SerialInterface::SIDEVICE_GC_CONTROLLER : SerialInterface::SIDEVICE_NONE;
  Config::SetBase(Config::GetInfoForSIDevice(1), device);
  if (Core::IsRunning(Core::System::GetInstance()))
    Core::System::GetInstance().GetSerialInterface().ChangeDevice(device, 1);
}

ControllerEmu::EmulatedController* GetPad(int port)
{
  InputConfig* const config = Pad::GetConfig();
  if (config == nullptr || config->GetControllerCount() <= port || port < 0)
    return nullptr;
  return config->GetController(port);
}

// Which port the CONTROLS tab is editing. Mirrored outside the mutex on purpose:
// the row builders and the input detector all run with it released, and they
// must act on the same pad the rows were built from.
std::atomic<int> s_config_port{0};

// The pad being CONFIGURED.
ControllerEmu::EmulatedController* GetConfiguredPad()
{
  return GetPad(s_config_port.load(std::memory_order_relaxed));
}

// The pad that DRIVES the menu -- always port 1, so opening the tab on port 2
// never moves navigation out from under the hands holding the first pad.
ControllerEmu::EmulatedController* GetMenuPad() { return GetPad(0); }

// A qualified device string is "SOURCE/CID/NAME", or "SOURCE//NAME" when the
// backend does not number its devices, so the backend is everything before the
// first slash and the readable name everything after the second.
std::string DeviceSource(const std::string& qualified)
{
  const size_t slash = qualified.find('/');
  return slash == std::string::npos ? qualified : qualified.substr(0, slash);
}

std::string DeviceName(const std::string& qualified)
{
  const size_t first = qualified.find('/');
  if (first == std::string::npos)
    return qualified;
  const size_t second = qualified.find('/', first + 1);
  return second == std::string::npos ? qualified.substr(first + 1) : qualified.substr(second + 1);
}

// Every backend that currently has at least one device, in the order the
// interface reports them. A backend with no devices is deliberately not
// offered: selecting it would leave the row below with nothing to show.
std::vector<std::string> DeviceSources(const std::vector<std::string>& devices)
{
  std::vector<std::string> sources;
  for (const std::string& device : devices)
  {
    std::string source = DeviceSource(device);
    if (std::ranges::find(sources, source) == sources.end())
      sources.push_back(std::move(source));
  }
  return sources;
}

// Both return the device to switch to, or empty for "nothing to change to".
// Changing backend lands on that backend's first device so the two rows can
// never disagree about which backend is selected.
std::string CycleBackend(const State& state, int direction)
{
  const std::vector<std::string> sources = DeviceSources(state.devices);
  if (sources.empty())
    return {};

  const auto iter = std::ranges::find(sources, DeviceSource(state.device_current));
  const int index = iter == sources.end() ? 0 : static_cast<int>(iter - sources.begin());
  const int count = static_cast<int>(sources.size());
  const std::string& target = sources[(index + direction + count) % count];

  for (const std::string& device : state.devices)
  {
    if (DeviceSource(device) == target)
      return device;
  }
  return {};
}

// Moves within the current backend only, so this row never silently switches
// the row above it.
std::string CycleDevice(const State& state, int direction)
{
  const std::string source = DeviceSource(state.device_current);
  std::vector<std::string> siblings;
  for (const std::string& device : state.devices)
  {
    if (DeviceSource(device) == source)
      siblings.push_back(device);
  }
  if (siblings.empty())
    return {};

  const auto iter = std::ranges::find(siblings, state.device_current);
  const int index = iter == siblings.end() ? 0 : static_cast<int>(iter - siblings.begin());
  const int count = static_cast<int>(siblings.size());
  return siblings[(index + direction + count) % count];
}

// Reads the interface's device list and the pad's current device. Touches
// g_controller_interface and InputConfig, so like BuildControlRowsData it must
// run with the menu mutex released.
void BuildDeviceListData(std::vector<std::string>* devices, std::string* current)
{
  *devices = g_controller_interface.GetAllDeviceStrings();
  auto* const pad = GetConfiguredPad();
  *current = pad != nullptr ? pad->GetDefaultDevice().ToString() : std::string();
}

// Flattens the pad's group/control tree into the page's row list. Only inputs
// are remappable; outputs (rumble) are skipped.
// Touches InputConfig, so likewise runs with the menu mutex released.
void BuildControlRowsData(std::vector<ControlRow>* rows)
{
  rows->clear();
  auto* const pad = GetConfiguredPad();
  if (pad == nullptr)
    return;

  for (auto& group : pad->groups)
  {
    for (auto& control : group->controls)
    {
      if (control->control_ref == nullptr || !control->control_ref->IsInput())
        continue;
      rows->push_back({group->ui_name + ": " + control->ui_name, control.get()});
    }
  }
}

void StartDetection(State& state, ControllerEmu::Control* control)
{
  auto* const pad = GetConfiguredPad();
  if (pad == nullptr || control == nullptr)
    return;

  const std::vector<std::string> devices = {pad->GetDefaultDevice().ToString()};
  state.detector = std::make_unique<ciface::Core::InputDetector>();
  state.detector->Start(g_controller_interface, devices);
  state.detecting_control = control;
}

// Reads the game's Action Replay + Gecko codes from its ini (built-in defaults
// plus the user's GameSettings/<id>.ini) into the page's list. Codes are stored
// with their saved enabled state; a game with no codes yields an empty list.
// Does game-ini file I/O, so it must run with the menu mutex RELEASED -- the
// video thread blocks on that mutex inside Draw(). Results are assigned to the
// State afterwards under a short lock.
void LoadCheatCodesData(std::vector<ActionReplay::ARCode>* ar_codes,
                        std::vector<Gecko::GeckoCode>* gecko_codes,
                        std::vector<CheatRow>* cheat_rows)
{
  const SConfig& sconfig = SConfig::GetInstance();
  const Common::IniFile global_ini = sconfig.LoadDefaultGameIni();
  const Common::IniFile local_ini = sconfig.LoadLocalGameIni();

  *ar_codes = ActionReplay::LoadCodes(global_ini, local_ini);
  *gecko_codes = Gecko::LoadCodes(global_ini, local_ini);

  // RE aid: the shipped codes are Codejunkies-encrypted in the ini, and the
  // decrypted address/value pairs are what point at live game state (health,
  // timer, ...) for things like the training HUD.
  if (std::getenv("RECOMP_DUMP_ARCODES"))
  {
    for (const auto& code : *ar_codes)
    {
      std::fprintf(stderr, "[arcode] %s\n", code.name.c_str());
      for (const auto& op : code.ops)
        std::fprintf(stderr, "[arcode]   %08X %08X\n", op.cmd_addr, op.value);
    }
  }

  cheat_rows->clear();
  for (size_t i = 0; i < ar_codes->size(); ++i)
    cheat_rows->push_back({"[AR] " + (*ar_codes)[i].name, false, i});
  for (size_t i = 0; i < gecko_codes->size(); ++i)
    cheat_rows->push_back({"[Gecko] " + (*gecko_codes)[i].name, true, i});
}

// Pushes the current enabled flags to the live cheat engine. RunAllActive fires
// each frame from a VI-timed CoreTiming event, so this takes effect immediately
// without a reboot. Turning any code on also flips the master cheats switch so
// the state is coherent.
// Takes copies rather than the live State so it can run with the menu mutex
// released; the caller flushes Config afterwards.
void ApplyCheatCodes(const std::vector<ActionReplay::ARCode>& ar_codes,
                     const std::vector<Gecko::GeckoCode>& gecko_codes)
{
  const SConfig& sconfig = SConfig::GetInstance();
  const std::string game_id = sconfig.GetGameID();
  const u16 revision = sconfig.GetRevision();

  ActionReplay::ApplyCodes(ar_codes, game_id, revision);
  Gecko::SetActiveCodes(gecko_codes, game_id, revision);
}

// Widescreen needs the projection widened, not the 4:3 image stretched, so the
// hack and the aspect mode are always flipped together (same pairing as Alt+W).
void SetWidescreen(bool enable)
{
  Config::SetBase(Config::GFX_WIDESCREEN_HACK, enable);
  Config::SetBase(Config::GFX_ASPECT_RATIO, enable ? AspectMode::ForceWide : AspectMode::Auto);
}

// Returns true if a Config value changed and needs flushing. The flush itself
// (Config::Save) must happen with the menu mutex released -- see OnKey.
bool AdjustItem(Item item, int direction, State& state)
{
  switch (item)
  {
  case Item::Overclock:
  {
    // Refuse rather than change something netplay will overwrite anyway.
    if (NetPlay::IsNetPlayRunning())
      break;
    // A curated ladder, not a free slider: the interesting settings are a few
    // steps either side of stock, and underclocking matters as much as
    // overclocking -- some GameCube titles are bound by an unnecessarily fast
    // CPU starving the GPU. Index 0 is off (factor exactly 1.0, enable false)
    // so "no overclock" is a real state and not 100% with the machinery live.
    static constexpr std::array<float, 8> kFactors = {1.0f, 0.5f,  0.75f, 1.25f,
                                                      1.5f, 2.0f,  3.0f,  4.0f};
    const bool enabled = Config::Get(Config::MAIN_OVERCLOCK_ENABLE);
    const float current = Config::Get(Config::MAIN_OVERCLOCK);
    int index = 0;
    if (enabled)
    {
      for (size_t i = 1; i < kFactors.size(); ++i)
      {
        if (std::fabs(kFactors[i] - current) < 0.001f)
          index = static_cast<int>(i);
      }
    }
    index = std::clamp(index + direction, 0, static_cast<int>(kFactors.size()) - 1);
    Config::SetBase(Config::MAIN_OVERCLOCK_ENABLE, index != 0);
    Config::SetBase(Config::MAIN_OVERCLOCK, kFactors[index]);
    break;
  }
  case Item::NetplayScan:
  {
    // Picking a host is the whole point of having scanned, so selecting one
    // fills in the address and port rather than merely naming it.
    if (state.netplay_mode != 2 || state.netplay_hosts.empty())
      break;
    const int count = static_cast<int>(state.netplay_hosts.size());
    state.netplay_host_index =
        (state.netplay_host_index + direction % count + count) % count;
    const State::FoundHost& host = state.netplay_hosts[state.netplay_host_index];
    state.netplay_addr_text = host.address;
    state.netplay_port = host.port;
    ParseAddress(host.address, &state.netplay_addr);
    break;
  }
  case Item::NetplayMode:
    state.netplay_mode = std::clamp(state.netplay_mode + direction, 0, 2);
    break;
  case Item::NetplayPort:
    // Ports are not worth paging one at a time; step by 1 and let the value
    // wrap inside the ephemeral range most people will use.
    state.netplay_port = std::clamp(state.netplay_port + direction, 1024, 65535);
    break;
  case Item::NetplayStart:
    break;   // an action, not a value
  case Item::Widescreen:
    SetWidescreen(!Config::Get(Config::GFX_WIDESCREEN_HACK));
    break;
  case Item::InternalRes:
  {
    const int scale = std::clamp(Config::Get(Config::GFX_EFB_SCALE) + direction, 1, 8);
    Config::SetBase(Config::GFX_EFB_SCALE, scale);
    // Mirrored into config.ini once the mutex is released, or the frontend
    // overrides it at the next boot -- see WriteFrontendResolution.
    state.pending_resolution_scale = scale;
    break;
  }
  case Item::AspectRatio:
  {
    static constexpr std::array<AspectMode, 4> kModes = {
        AspectMode::Auto, AspectMode::ForceWide, AspectMode::ForceStandard, AspectMode::Stretch};
    const AspectMode current = Config::Get(Config::GFX_ASPECT_RATIO);
    int index = 0;
    for (size_t i = 0; i < kModes.size(); ++i)
      if (kModes[i] == current)
        index = static_cast<int>(i);
    index = std::clamp(index + direction, 0, static_cast<int>(kModes.size()) - 1);
    Config::SetBase(Config::GFX_ASPECT_RATIO, kModes[index]);
    break;
  }
  case Item::LensFlares:
    Config::SetBase(Config::GFX_HACK_SKIP_EFB_COPY_TO_RAM,
                    !Config::Get(Config::GFX_HACK_SKIP_EFB_COPY_TO_RAM));
    break;
  case Item::VSync:
    Config::SetBase(Config::GFX_VSYNC, !Config::Get(Config::GFX_VSYNC));
    break;
  case Item::AntiAliasing:
  {
    // Sample counts the backends reliably support; SSAA is left off.
    static constexpr std::array<u32, 4> kSamples = {1, 2, 4, 8};
    const u32 current = Config::Get(Config::GFX_MSAA);
    int index = 0;
    for (size_t i = 0; i < kSamples.size(); ++i)
      if (kSamples[i] == current)
        index = static_cast<int>(i);
    index = std::clamp(index + direction, 0, static_cast<int>(kSamples.size()) - 1);
    Config::SetBase(Config::GFX_MSAA, kSamples[index]);
    Config::SetBase(Config::GFX_SSAA, false);
    break;
  }
  case Item::Anisotropy:
  {
    static constexpr std::array<AnisotropicFilteringMode, 6> kModes = {
        AnisotropicFilteringMode::Default, AnisotropicFilteringMode::Force1x,
        AnisotropicFilteringMode::Force2x, AnisotropicFilteringMode::Force4x,
        AnisotropicFilteringMode::Force8x, AnisotropicFilteringMode::Force16x};
    const AnisotropicFilteringMode current = Config::Get(Config::GFX_ENHANCE_MAX_ANISOTROPY);
    int index = 0;
    for (size_t i = 0; i < kModes.size(); ++i)
      if (kModes[i] == current)
        index = static_cast<int>(i);
    index = std::clamp(index + direction, 0, static_cast<int>(kModes.size()) - 1);
    Config::SetBase(Config::GFX_ENHANCE_MAX_ANISOTROPY, kModes[index]);
    break;
  }
  case Item::TextureFiltering:
  {
    static constexpr std::array<TextureFilteringMode, 3> kModes = {
        TextureFilteringMode::Default, TextureFilteringMode::Nearest, TextureFilteringMode::Linear};
    const TextureFilteringMode current = Config::Get(Config::GFX_ENHANCE_FORCE_TEXTURE_FILTERING);
    int index = 0;
    for (size_t i = 0; i < kModes.size(); ++i)
      if (kModes[i] == current)
        index = static_cast<int>(i);
    index = std::clamp(index + direction, 0, static_cast<int>(kModes.size()) - 1);
    Config::SetBase(Config::GFX_ENHANCE_FORCE_TEXTURE_FILTERING, kModes[index]);
    break;
  }
  case Item::Filter:
  {
    const std::string current = Config::Get(Config::GFX_ENHANCE_POST_SHADER);
    int index = 0;
    for (size_t i = 0; i < kFilters.size(); ++i)
      if (current == kFilters[i].shader)
        index = static_cast<int>(i);
    index = std::clamp(index + direction, 0, static_cast<int>(kFilters.size()) - 1);
    Config::SetBase(Config::GFX_ENHANCE_POST_SHADER, std::string(kFilters[index].shader));
    break;
  }
  case Item::TexturePacks:
    // Dolphin-format packs, loaded from <userdir>/Load/Textures/GRSEAF/.
    Config::SetBase(Config::GFX_HIRES_TEXTURES, !Config::Get(Config::GFX_HIRES_TEXTURES));
    break;
  case Item::PrefetchTextures:
    // Loads the whole pack into RAM up front: smoother, but a big pack costs a
    // long load and a lot of memory.
    Config::SetBase(Config::GFX_CACHE_HIRES_TEXTURES,
                    !Config::Get(Config::GFX_CACHE_HIRES_TEXTURES));
    break;
  case Item::ShowFPS:
    Config::SetBase(Config::GFX_SHOW_FPS, !Config::Get(Config::GFX_SHOW_FPS));
    break;
  case Item::FreeCamera:
    Config::SetBase(Config::FREE_LOOK_ENABLED, !Config::Get(Config::FREE_LOOK_ENABLED));
    break;
  case Item::TrainingHud:
    Config::SetBase(RECOMP_TRAINING_HUD, !Config::Get(RECOMP_TRAINING_HUD));
    break;
  case Item::Muted:
    Config::SetBase(Config::MAIN_AUDIO_MUTED, !Config::Get(Config::MAIN_AUDIO_MUTED));
    break;
  case Item::AudioLatency:
    Config::SetBase(Config::MAIN_AUDIO_LATENCY,
                    std::clamp(Config::Get(Config::MAIN_AUDIO_LATENCY) + direction * 5, 0, 200));
    break;
  case Item::FillGaps:
    Config::SetBase(Config::MAIN_AUDIO_FILL_GAPS, !Config::Get(Config::MAIN_AUDIO_FILL_GAPS));
    break;
  case Item::Volume:
  {
    const int volume = std::clamp(Config::Get(Config::MAIN_AUDIO_VOLUME) + direction * 5, 0, 100);
    Config::SetBase(Config::MAIN_AUDIO_VOLUME, volume);
    break;
  }
  case Item::Speed:
  {
    // 0 (unlimited) sits above 200% so the wrap order reads naturally.
    static constexpr std::array<float, 8> kSpeeds = {0.25f, 0.5f,  0.75f, 1.0f,
                                                     1.25f, 1.5f,  2.0f,  0.0f};
    const float current = Config::Get(Config::MAIN_EMULATION_SPEED);
    int index = 3;
    for (size_t i = 0; i < kSpeeds.size(); ++i)
    {
      if (kSpeeds[i] == current)
      {
        index = static_cast<int>(i);
        break;
      }
    }
    index = std::clamp(index + direction, 0, static_cast<int>(kSpeeds.size()) - 1);
    Config::SetBase(Config::MAIN_EMULATION_SPEED, kSpeeds[index]);
    break;
  }
  case Item::AutoResume:
    // On quit, save a state; on next launch, load it. Save/load themselves
    // happen outside the mutex (Quit action / ScheduleAutoResumeLoad).
    Config::SetBase(RECOMP_AUTO_RESUME, !Config::Get(RECOMP_AUTO_RESUME));
    break;
  case Item::StateSlot:
    state.state_slot = std::clamp(state.state_slot + direction, 1, 8);
    return false;  // Not a Config setting; nothing to persist.
  default:
    return false;
  }

  // Writes went to the base layer above; the caller flushes them once it has
  // dropped the mutex.
  return true;
}

// Activating some rows means calling into Core, which must never happen while
// the menu mutex is held (Draw takes the same lock from the present path) and,
// for save states, must not happen while emulation is paused: State::Save goes
// through Core::RunOnCPUThread, whose PauseAndLock/RestoreStateAndUnlock pair
// restores whatever run state it found. Called with the core already paused it
// sees was_running == false, leaves the CPU paused, and the queued job never
// runs -- which wedges the caller. So Activate only *decides*; OnKey performs.
enum class Action
{
  None,
  Fullscreen,
  Quit,
  Reset,
  SaveState,
  LoadState,
  StartNetplay,
  ScanHosts,
};

// Quit, but not before the core is actually running again.
//
// Shutdown joins the CPU thread. While the menu is open that thread is paused,
// and CloseAndResume only *queues* the resume on the state worker -- so quitting
// straight after it races the resume and wedges the process, main thread in
// futex_wait and CPU thread parked. That is a real bug in the stock "Quit Game"
// row too, not just here: it hangs identically, which is why the fix lives in a
// helper both call.
//
// The wait runs on a detached thread for the reason documented on the state
// worker: the host thread is the X11/Wayland event loop, and blocking it while
// the video thread waits on a swapchain image closes the deadlock loop. The
// bound means a core that never resumes still quits rather than hanging forever.
void QuitOnceResumed(const std::function<void()>& quit_callback)
{
  std::thread([quit_callback] {
    auto& system = Core::System::GetInstance();
    for (int i = 0; i < 300 && Core::GetState(system) != Core::State::Running; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (Core::GetState(system) != Core::State::Running)
      std::fprintf(stderr, "[menu] core did not resume; quitting anyway\n");
    quit_callback();
  }).detach();
}

// Hand the netplay request to the runner and let it restart into the lobby.
//
// A file rather than a callback because the decision has to outlive this
// process's emulation session: the core is torn down, then the runner reads
// this, deletes it, and brings up the lobby. Netplay cannot be entered in
// place -- the lobby runs before the core boots.
bool WriteNetplayRequest(int mode, int port, const std::string& addr)
{
  const std::string path = File::GetUserPath(D_USER_IDX) + "netplay-request.ini";
  std::ofstream out(path, std::ios::trunc);
  if (!out)
    return false;
  out << "[Netplay]\n"
      << "mode = " << (mode == 1 ? "host" : "join") << '\n'
      << "port = " << port << '\n';
  // Only meaningful to a joiner, and writing it unconditionally would let a
  // stale address from an earlier join override the host path's own settings.
  // Empty is left out entirely so the runner falls back to the configured one
  // rather than being handed nothing to dial.
  if (mode == 2 && !addr.empty())
    out << "address = " << addr << '\n';
  out.close();
  return static_cast<bool>(out);
}

Action DecideAction(Item item, State& state, bool* needs_config_save)
{
  switch (item)
  {
  case Item::Fullscreen:
    return Action::Fullscreen;
  case Item::SaveState:
    return Action::SaveState;
  case Item::LoadState:
    return Action::LoadState;
  case Item::Reset:
    return Action::Reset;
  case Item::Quit:
    return Action::Quit;
  case Item::NetplayScan:
    // Only a joiner looks for anyone, and the listen blocks for seconds -- so
    // OnKey runs it on a worker after unlocking, like every other slow action.
    return state.netplay_mode == 2 ? Action::ScanHosts : Action::None;
  case Item::NetplayAddress:
    // Space starts editing; the arrows then belong to the octets until Space
    // ends it. Inert unless joining, matching what the row displays -- only a
    // joiner dials an address.
    //
    // Starting an edit is also what commits the octets over a hostname the row
    // cannot show. That is deliberate and takes a keypress on this exact row:
    // the alternative, converting on sight, is how a MagicDNS name would get
    // silently replaced by whatever the octets happened to hold.
    if (state.netplay_mode == 2)
    {
      state.netplay_addr_octet = 0;
      state.netplay_addr_text = PlainAddress(state.netplay_addr);
    }
    return Action::None;
  case Item::NetplayStart:
    // Patched game data desyncs against an unmodified peer, so the session is
    // refused HERE rather than at connect: starting it costs a restart of the
    // game, and finding out afterwards would mean the player pays that price to
    // be told no. The MODS tab carries the same sentence next to the cause.
    if (RecompGameData::NetplayBlocked())
      return Action::None;
    // Off means the row is inert, so Enter cannot restart the game by accident
    // while somebody is paging past it.
    return state.netplay_mode == 0 ? Action::None : Action::StartNetplay;
  case Item::Apply:
    // Settings already take effect as they are changed; this flushes them to
    // disk and gives an explicit confirm step.
    *needs_config_save = true;
    return Action::None;
  default:
    // Enter deliberately does NOT cycle values -- Left/Right do that. Enter used
    // to double as "next value", which made it far too easy to change a setting
    // while trying to confirm one.
    return Action::None;
  }
}
}  // namespace

bool IsOpen()
{
  std::lock_guard<std::mutex> guard(s_state.mutex);
  return s_state.open;
}

void Toggle()
{
  // Read the persisted join address before taking the lock -- same rule the
  // rest of this file follows for file I/O. Once per process: the runner only
  // rewrites config.ini on its way out to the lobby, and by then this process
  // is quitting anyway.
  bool seed_address = false;
  {
    std::lock_guard<std::mutex> guard(s_state.mutex);
    seed_address = !s_state.netplay_addr_seeded;
    s_state.netplay_addr_seeded = true;
  }
  if (seed_address)
  {
    std::string seeded_text = "127.0.0.1";
    std::array<int, 4> seeded = {127, 0, 0, 1};
    SeedNetplayAddress(&seeded_text, &seeded);
    std::lock_guard<std::mutex> guard(s_state.mutex);
    s_state.netplay_addr_text = seeded_text;
    s_state.netplay_addr = seeded;
  }

  bool open_now;
  {
    std::lock_guard<std::mutex> guard(s_state.mutex);
    s_state.open = !s_state.open;
    open_now = s_state.open;
    if (open_now)
    {
      s_state.selected = 0;
      s_state.tab = Tab::System;
    }
    // Never reopen mid-edit: selection resets to the tab row above, so an octet
    // index left over from last time would point at a row nobody is on.
    s_state.netplay_addr_octet = -1;
    s_state.netplay_addr_run = 0;
    s_state.netplay_addr_run_dir = 0;
  }

  // Escape pauses. The pause itself is handed to the worker thread -- doing it
  // on the host thread deadlocks against the compositor (see RequestCoreState).
  if (open_now)
  {
    std::lock_guard<std::mutex> lock(s_core_state_mutex);
    if (!s_settings_guard)
      s_settings_guard = std::make_unique<Config::ConfigChangeCallbackGuard>();
  }
  RequestCoreState(open_now);

  std::fprintf(stderr, "[menu] toggle open=%d (core %s)\n", open_now ? 1 : 0,
               open_now ? "pausing" : "resuming");
}

void CloseAndResume()
{
  {
    std::lock_guard<std::mutex> guard(s_state.mutex);
    if (!s_state.open)
      return;
    s_state.open = false;
  }
  RequestCoreState(false);   // resumes, then flushes the staged settings
}

void OnKey(Key key)
{
  Action action = Action::None;
  int slot = 1;
  std::function<void()> fullscreen_callback;
  std::function<void()> quit_callback;
  int netplay_mode_snapshot = 0;
  int netplay_port_snapshot = 2626;
  std::string netplay_addr_snapshot = "127.0.0.1";
  int resolution_to_persist = -1;

  // Deferred work. NOTHING below may call into Config/Core/InputConfig while
  // the menu mutex is held: those can block on the CPU thread while the video
  // thread is blocked on this very mutex inside Draw(). Collect intent here,
  // act after unlocking.
  bool needs_config_save = false;
  bool needs_cheat_apply = false;
  bool enable_cheats_master = false;
  bool needs_build_controls = false;
  bool needs_load_cheats = false;
  bool needs_load_mods = false;
  // Toggling a mod is a directory rescan away from being visible, and both the
  // write and the rescan are file I/O -- so the key handler only records what
  // to do and the work happens once the mutex is released.
  std::string toggle_mod_name;
  bool toggle_mod_enabled = false;
  bool needs_restore_request = false;
  int install_mod_index = -1;
  bool needs_texture_reload = false;
  bool needs_refresh_devices = false;
  // Deferred like every other write here: SetPort2Attached touches the config
  // system and CoreTiming, neither of which belongs under this file's mutex.
  bool toggle_port2 = false;
  bool port2_enabled = false;
  std::string new_device;
  ControllerEmu::Control* changed_pad_control = nullptr;
  std::vector<ActionReplay::ARCode> ar_snapshot;
  std::vector<Gecko::GeckoCode> gecko_snapshot;

  // Config::SetBase still happens under the mutex (AdjustItem) and a plain Set
  // fires OnConfigChanged -> callbacks synchronously. Declared out here so it
  // outlives the lock and callbacks defer until the mutex is long released.
  const Config::ConfigChangeCallbackGuard config_callback_guard;

  {
    std::lock_guard<std::mutex> guard(s_state.mutex);
    if (!s_state.open)
      return;

    // Ignore everything while waiting for a button press to bind.
    if (s_state.detector != nullptr)
      return;

    const int row_count = 1 + RowCount(s_state);  // row 0 = tab selector

    // Row 0: left/right switches tab, up/down moves into the list.
    if (s_state.selected == 0)
    {
      switch (key)
      {
      case Key::Left:
      case Key::Right:
      {
        const int dir = key == Key::Left ? -1 : 1;
        s_state.tab = static_cast<Tab>(
            (static_cast<int>(s_state.tab) + dir + kTabCount) % kTabCount);
        // These two build their rows on entry, but that means game-ini I/O and
        // InputConfig access -- deferred until the mutex is released.
        if (s_state.tab == Tab::Controls)
          needs_build_controls = true;
        else if (s_state.tab == Tab::Cheats)
          needs_load_cheats = true;
        else if (s_state.tab == Tab::Mods)
          needs_load_mods = true;
        break;
      }
      case Key::Up:
        s_state.selected = row_count - 1;
        break;
      case Key::Down:
        s_state.selected = row_count > 1 ? 1 : 0;
        break;
      case Key::Activate:
        break;
      }
    }
    else
    {
      const int index = s_state.selected - 1;

      // Editing an address octet takes over all four arrows: Up/Down would
      // otherwise walk off the row mid-edit and leave the octet marker behind
      // on a row nobody is on. Space ends the edit, and it is the only way out
      // that keeps Esc meaning "close the menu" everywhere.
      bool address_edit_consumed_key = false;
      if (s_state.netplay_addr_octet >= 0)
      {
        const auto& items = TabItems(s_state.tab);
        const bool on_address_row =
            index < static_cast<int>(items.size()) &&
            items[index] == Item::NetplayAddress;
        if (on_address_row)
        {
          address_edit_consumed_key = true;
          switch (key)
          {
          case Key::Left:
            s_state.netplay_addr_octet = (s_state.netplay_addr_octet + 3) % 4;
            s_state.netplay_addr_run = 0;
            s_state.netplay_addr_run_dir = 0;
            break;
          case Key::Right:
            s_state.netplay_addr_octet = (s_state.netplay_addr_octet + 1) % 4;
            s_state.netplay_addr_run = 0;
            s_state.netplay_addr_run_dir = 0;
            break;
          case Key::Up:
          case Key::Down:
          {
            const int dir = key == Key::Up ? 1 : -1;
            // Held keys arrive as repeats, so a run counts them and the step
            // grows: single presses stay precise, holding covers the range.
            if (dir == s_state.netplay_addr_run_dir)
              ++s_state.netplay_addr_run;
            else
              s_state.netplay_addr_run = 0;
            s_state.netplay_addr_run_dir = dir;
            const int step = s_state.netplay_addr_run < 4    ? 1
                             : s_state.netplay_addr_run < 12 ? 5
                                                             : 25;
            int& octet = s_state.netplay_addr[s_state.netplay_addr_octet];
            // Wrap rather than clamp: 0 to 255 is then one press, and an octet
            // has no meaningful end to stop at the way a port range does.
            octet = ((octet + dir * step) % 256 + 256) % 256;
            // The octets are now what the user means, so they become the text.
            s_state.netplay_addr_text = PlainAddress(s_state.netplay_addr);
            break;
          }
          case Key::Activate:
            s_state.netplay_addr_octet = -1;
            s_state.netplay_addr_run = 0;
            s_state.netplay_addr_run_dir = 0;
            break;
          }
        }
        else
        {
          // Selection moved off the row some other way; do not keep editing.
          s_state.netplay_addr_octet = -1;
        }
      }

      // The address lives in the request file and is persisted by the runner,
      // not in Dolphin's Config, so editing it never sets needs_config_save.
      if (!address_edit_consumed_key)
      switch (key)
      {
      case Key::Up:
        s_state.selected = (s_state.selected - 1 + row_count) % row_count;
        break;
      case Key::Down:
        s_state.selected = (s_state.selected + 1) % row_count;
        break;
      case Key::Left:
      case Key::Right:
      {
        const int dir = key == Key::Left ? -1 : 1;
        if (s_state.tab == Tab::Controls)
        {
          const int control_index = index - kControlsHeaderRows;
          if (index == kPortRow)
          {
            s_state.config_port = s_state.config_port == 0 ? 1 : 0;
            s_config_port.store(s_state.config_port, std::memory_order_relaxed);
            // The rows below belong to the OTHER pad now, so they have to be
            // rebuilt -- and that touches InputConfig, so it waits for unlock.
            needs_build_controls = true;
          }
          else if (index == kBackendRow)
          {
            new_device = CycleBackend(s_state, dir);
          }
          else if (index == kDeviceRow)
          {
            new_device = CycleDevice(s_state, dir);
          }
          else if (index == kPort2Row)
          {
            toggle_port2 = true;
            port2_enabled = !Port2Attached();
          }
          // Left clears a binding; the engine-side refresh happens after unlock.
          else if (key == Key::Left && control_index >= 0 &&
                   control_index < static_cast<int>(s_state.control_rows.size()))
          {
            auto* const control = s_state.control_rows[control_index].control;
            control->control_ref->SetExpression("");
            changed_pad_control = control;
          }
          // Applied after unlock, but the snapshot the rows are drawn from is
          // state, so it updates here or the row would lag a frame behind.
          if (!new_device.empty())
            s_state.device_current = new_device;
        }
        else if (s_state.tab == Tab::Mods)
        {
          // Left/Right toggles the same rows Space does. Everything else in
          // this menu changes a value with the arrows, and a row that only
          // answered to Space read as broken.
          ToggleModRow(index, &toggle_mod_name, &toggle_mod_enabled,
                       &needs_texture_reload, &needs_config_save, &install_mod_index);
        }
        else if (s_state.tab != Tab::Cheats)
        {
          const auto& items = TabItems(s_state.tab);
          if (index < static_cast<int>(items.size()))
            needs_config_save = AdjustItem(items[index], dir, s_state);
        }
        break;
      }
      case Key::Activate:
        if (s_state.tab == Tab::Controls)
        {
          const int control_index = index - kControlsHeaderRows;
          // On either header row, Space re-scans instead of rebinding: a pad
          // plugged in after the menu opened is otherwise invisible until the
          // tab is left and re-entered.
          if (index == kPortRow)
          {
            s_state.config_port = s_state.config_port == 0 ? 1 : 0;
            s_config_port.store(s_state.config_port, std::memory_order_relaxed);
            needs_build_controls = true;
          }
          else if (index == kBackendRow || index == kDeviceRow)
            needs_refresh_devices = true;
          else if (index == kPort2Row)
          {
            toggle_port2 = true;
            port2_enabled = !Port2Attached();
          }
          else if (control_index < static_cast<int>(s_state.control_rows.size()))
            StartDetection(s_state, s_state.control_rows[control_index].control);
        }
        else if (s_state.tab == Tab::Cheats)
        {
          if (index == 0)
          {
            enable_cheats_master = !Config::Get(Config::MAIN_ENABLE_CHEATS);
          }
          else if (index - 1 < static_cast<int>(s_state.cheat_rows.size()))
          {
            const CheatRow& row = s_state.cheat_rows[index - 1];
            if (row.gecko)
              s_state.gecko_codes[row.index].enabled = !s_state.gecko_codes[row.index].enabled;
            else
              s_state.ar_codes[row.index].enabled = !s_state.ar_codes[row.index].enabled;
            enable_cheats_master = true;
          }
          ar_snapshot = s_state.ar_codes;
          gecko_snapshot = s_state.gecko_codes;
          needs_cheat_apply = true;
          needs_config_save = true;
        }
        else if (s_state.tab == Tab::Mods)
        {
          if (index == 1 + static_cast<int>(s_state.mod_rows.size()) + 1)
            needs_restore_request = true;
          else
            ToggleModRow(index, &toggle_mod_name, &toggle_mod_enabled,
                         &needs_texture_reload, &needs_config_save, &install_mod_index);
        }
        else
        {
          const auto& items = TabItems(s_state.tab);
          if (index < static_cast<int>(items.size()))
            action = DecideAction(items[index], s_state, &needs_config_save);
        }
        break;
      }
    }

    slot = s_state.state_slot;
    fullscreen_callback = s_state.fullscreen_callback;
    quit_callback = s_state.quit_callback;
    netplay_mode_snapshot = s_state.netplay_mode;
    netplay_port_snapshot = s_state.netplay_port;
    netplay_addr_snapshot = s_state.netplay_addr_text;
    resolution_to_persist = s_state.pending_resolution_scale;
    s_state.pending_resolution_scale = -1;
  }

  if (resolution_to_persist > 0)
    WriteFrontendResolution(resolution_to_persist);

  // Everything below runs with the mutex released, so these engine calls can
  // safely block without wedging the video thread inside Draw().
  if (needs_build_controls)
  {
    std::vector<ControlRow> built;
    BuildControlRowsData(&built);
    std::vector<std::string> devices;
    std::string current;
    BuildDeviceListData(&devices, &current);
    std::lock_guard<std::mutex> guard(s_state.mutex);
    s_state.control_rows = std::move(built);
    s_state.devices = std::move(devices);
    s_state.device_current = std::move(current);
  }

  if (needs_load_cheats)
  {
    std::vector<ActionReplay::ARCode> ar;
    std::vector<Gecko::GeckoCode> gecko;
    std::vector<CheatRow> cheat_rows;
    LoadCheatCodesData(&ar, &gecko, &cheat_rows);
    std::lock_guard<std::mutex> guard(s_state.mutex);
    s_state.ar_codes = std::move(ar);
    s_state.gecko_codes = std::move(gecko);
    s_state.cheat_rows = std::move(cheat_rows);
  }

  if (!toggle_mod_name.empty())
    RecompMods::SetEnabled(toggle_mod_name, toggle_mod_enabled);

  if (install_mod_index >= 0)
  {
    RecompMods::Mod mod;
    {
      std::lock_guard<std::mutex> guard(s_state.mutex);
      if (install_mod_index < static_cast<int>(s_state.mod_rows.size()))
        mod = s_state.mod_rows[install_mod_index];
    }
    std::string message;
    if (!mod.name.empty())
      RecompMods::RequestInstall(mod, &message);
    std::lock_guard<std::mutex> guard(s_state.mutex);
    s_state.mod_message = std::move(message);
  }

  if (needs_restore_request)
  {
    std::string message;
    RecompGameData::RequestRestore(&message);
    std::lock_guard<std::mutex> guard(s_state.mutex);
    s_state.mod_message = std::move(message);
  }

  if (needs_load_mods)
  {
    auto mods = RecompMods::Scan();
    auto data = RecompGameData::Get();
    std::lock_guard<std::mutex> guard(s_state.mutex);
    s_state.mod_rows = std::move(mods);
    s_state.game_data = std::move(data);
    // A stale result from a previous visit would sit there claiming something
    // about state that has since changed. An install message from THIS visit is
    // not stale, so only entering the tab clears it.
    if (install_mod_index < 0 && !needs_restore_request)
      s_state.mod_message.clear();
  }

  // Nothing to do on this thread: the texture cache reloads on the video thread
  // when it next sees the generation move. Kept as an explicit flag so the
  // reason the toggle takes effect is visible at the call site rather than
  // being an invisible side effect of SetEnabled.
  static_cast<void>(needs_texture_reload);

  // Re-scan, then re-read: RefreshDevices is what picks up a pad plugged in
  // while the menu was already open.
  if (toggle_port2)
  {
    SetPort2Attached(port2_enabled);
    needs_config_save = true;
  }

  if (needs_refresh_devices)
  {
    g_controller_interface.RefreshDevices();
    std::vector<std::string> devices;
    std::string current;
    BuildDeviceListData(&devices, &current);
    std::lock_guard<std::mutex> guard(s_state.mutex);
    s_state.devices = std::move(devices);
    s_state.device_current = std::move(current);
  }

  // Every binding is resolved against the pad's default device, so switching
  // device has to re-resolve all of them, not just one -- hence
  // UpdateReferences rather than the single-reference call used for a rebind.
  if (!new_device.empty())
  {
    if (auto* const pad = GetConfiguredPad())
    {
      pad->SetDefaultDevice(new_device);
      pad->UpdateReferences(g_controller_interface);
      if (auto* const config = Pad::GetConfig())
        config->SaveConfig();
    }
  }

  if (changed_pad_control != nullptr)
  {
    if (auto* const pad = GetConfiguredPad())
    {
      pad->UpdateSingleControlReference(g_controller_interface,
                                        changed_pad_control->control_ref.get());
      if (auto* const config = Pad::GetConfig())
        config->SaveConfig();
    }
  }

  if (needs_cheat_apply)
  {
    Config::SetBase(Config::MAIN_ENABLE_CHEATS, enable_cheats_master);
    ApplyCheatCodes(ar_snapshot, gecko_snapshot);
  }

  if (needs_config_save)
    Config::Save();

  switch (action)
  {
  case Action::Fullscreen:
    if (fullscreen_callback)
      fullscreen_callback();
    break;
  case Action::Reset:
    // Close (and resume) first: ResetButton_Tap only schedules the reset, so it
    // lands once the CPU thread is running again rather than against a paused
    // core.
    CloseAndResume();
    Core::System::GetInstance().GetProcessorInterface().ResetButton_Tap();
    break;
  case Action::ScanHosts:
  {
    // Off-thread: the listen blocks for seconds and this is the host thread,
    // which also has to keep drawing the menu (the core is paused, so Draw only
    // happens via PumpFrame from here). Blocking would freeze the overlay and
    // look exactly like the crash this menu has been mistaken for before.
    {
      std::lock_guard<std::mutex> guard(s_state.mutex);
      if (s_state.netplay_scanning)
        break;                       // already looking; ignore the repeat
      s_state.netplay_scanning = true;
    }
    // Detached, and it always finishes on its own within the listen window --
    // it never waits on a condition variable, which is what made the old
    // CoreStateWorker wedge process exit.
    std::thread([] {
      std::vector<State::FoundHost> found = ScanForHosts();
      std::lock_guard<std::mutex> guard(s_state.mutex);
      s_state.netplay_hosts = std::move(found);
      s_state.netplay_host_index = 0;
      s_state.netplay_scanning = false;
      s_state.netplay_scanned = true;
      // Adopt the first host straight away; scanning and then having to press
      // Right once to apply the only result would be a pointless extra step.
      if (!s_state.netplay_hosts.empty())
      {
        const State::FoundHost& host = s_state.netplay_hosts.front();
        s_state.netplay_addr_text = host.address;
        s_state.netplay_port = host.port;
        ParseAddress(host.address, &s_state.netplay_addr);
      }
    }).detach();
    break;
  }
  case Action::StartNetplay:
    // No auto-resume snapshot: a netplay session starts from boot on every
    // peer, so restoring this machine's save would desync it immediately.
    if (!quit_callback)
      break;
    if (!WriteNetplayRequest(netplay_mode_snapshot, netplay_port_snapshot,
                             netplay_addr_snapshot))
    {
      std::fprintf(stderr, "[netplay] could not write the request file\n");
      break;
    }
    std::fprintf(stderr, "[netplay] restarting into the lobby\n");
    CloseAndResume();
    QuitOnceResumed(quit_callback);
    break;
  case Action::Quit:
    if (!quit_callback)
      break;
    if (Config::Get(RECOMP_AUTO_RESUME) &&
        Core::GetState(Core::System::GetInstance()) == Core::State::Running)
    {
      // Capture the auto-resume state before quitting. State::SaveAs only
      // QUEUES: the snapshot runs later on the CPU thread and the file is
      // written by the compress worker (temp file + rename), so quitting right
      // away could tear the core down first. A worker deletes the old file,
      // queues the save, waits for the renamed file to appear, then quits.
      // Never block the host thread on the CPU thread here (see the pause
      // deadlock note above).
      std::thread([quit_callback] {
        auto& system = Core::System::GetInstance();
        const std::string path = AutoResumePath();
        File::Delete(path);
        std::fprintf(stderr, "[autoresume] saving %s\n", path.c_str());
        ::State::SaveAs(system, path);
        for (int i = 0; i < 1000 && !File::Exists(path); ++i)
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::fprintf(stderr, "[autoresume] save %s\n",
                     File::Exists(path) ? "complete" : "TIMED OUT");
        quit_callback();
      }).detach();
    }
    else
    {
      // Same paused-core teardown hazard as StartNetplay: the menu is open, so
      // the CPU thread is parked and a straight quit_callback() here wedges
      // shutdown. Resume first, then quit once it has taken.
      CloseAndResume();
      QuitOnceResumed(quit_callback);
    }
    break;
  case Action::SaveState:
  case Action::LoadState:
  {
    // Closing is cosmetic -- the core is never paused, so RunOnCPUThread always
    // sees a running CPU thread and the queued job actually executes.
    CloseAndResume();
    auto& system = Core::System::GetInstance();
    std::fprintf(stderr, "[menu] %s slot %d\n",
                 action == Action::SaveState ? "save" : "load", slot);
    if (action == Action::SaveState)
      ::State::Save(system, slot);
    else
      ::State::Load(system, slot);
    break;
  }
  case Action::None:
    break;
  }
}

void OnEscape()
{
  {
    std::lock_guard<std::mutex> guard(s_state.mutex);

    // While open, Escape unwinds one level at a time: cancel a pending bind,
    // then leave the subpage, and only then close. When closed, every path
    // below must fall through to Toggle so Escape still opens the menu.
    if (s_state.open)
    {
      if (s_state.detector != nullptr)
      {
        s_state.detector.reset();
        s_state.detecting_control = nullptr;
        return;
      }
      // An address edit is a level too: closing straight out of it would leave
      // the octet marker set, and the row would still be in edit mode the next
      // time the menu opened.
      if (s_state.netplay_addr_octet >= 0)
      {
        s_state.netplay_addr_octet = -1;
        s_state.netplay_addr_run = 0;
        s_state.netplay_addr_run_dir = 0;
        return;
      }
      // Tabs are switched in place, so Escape always just closes the menu.
    }
  }

  Toggle();
}

void SetFullscreenCallback(std::function<void()> callback)
{
  std::lock_guard<std::mutex> guard(s_state.mutex);
  s_state.fullscreen_callback = std::move(callback);
}

void SetQuitCallback(std::function<void()> callback)
{
  std::lock_guard<std::mutex> guard(s_state.mutex);
  s_state.quit_callback = std::move(callback);
}

void SetFastForward(bool enable)
{
  // The override lives in the CurrentRun layer so releasing the key just
  // deletes it and the user's configured speed (base layer, Speed row) shows
  // through again — restoring a saved value here would instead mask any speed
  // change made while the key was down.
  static std::atomic<bool> s_active{false};
  if (s_active.exchange(enable) == enable)
    return;
  if (enable)
  {
    Config::SetCurrent(Config::MAIN_EMULATION_SPEED, 0.0f);
    OSD::AddMessage("Fast-forward", 500);
  }
  else
  {
    Config::DeleteKey(Config::LayerType::CurrentRun, Config::MAIN_EMULATION_SPEED);
  }
}

void ScheduleAutoResumeLoad()
{
  if (!Config::Get(RECOMP_AUTO_RESUME))
    return;
  const std::string path = AutoResumePath();
  if (!File::Exists(path))
    return;
  // State::LoadAs needs a running CPU thread to execute the queued job, so
  // wait for Running off the host thread, plus a beat for the backends to
  // settle before swapping the whole machine state.
  std::thread([path] {
    auto& system = Core::System::GetInstance();
    for (int i = 0; i < 3000 && Core::GetState(system) != Core::State::Running; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (Core::GetState(system) != Core::State::Running)
    {
      std::fprintf(stderr, "[autoresume] core never reached Running; not loading\n");
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    std::fprintf(stderr, "[autoresume] loading %s\n", path.c_str());
    ::State::LoadAs(system, path);
  }).detach();
}

void Draw()
{
  // The HUD is independent of the menu: it renders whenever enabled, and first
  // so an open menu draws over it.
  DrawTrainingHud();

  Tab tab;
  int selected;
  int state_slot;
  bool detecting;
  bool editing_address = false;
  bool cheats_enabled = false;
  std::string mod_message;
  // label, value, highlight-value-green
  std::vector<std::tuple<std::string, std::string, bool>> rows;

  {
    std::lock_guard<std::mutex> guard(s_state.mutex);
    if (!s_state.open)
      return;

    tab = s_state.tab;
    selected = s_state.selected;
    state_slot = s_state.state_slot;
    detecting = s_state.detector != nullptr;
    editing_address = s_state.netplay_addr_octet >= 0;

    switch (tab)
    {
    case Tab::Controls:
    {
      // Read from the snapshot rather than the pad, so Draw stays off
      // InputConfig while holding the menu mutex.
      const bool has_device = !s_state.device_current.empty();
      // Which player the rows below belong to. Dolphin gives DEFAULT BINDINGS
      // TO PORT 1 ONLY -- InputConfig::LoadConfig clears the rest, so that four
      // pads do not all land on one device -- which means port 2 starts blank
      // and is unusable until it is bound here. Attaching it without this row
      // would unlock VS Battle for a player who then cannot move.
      rows.emplace_back("Configure", s_state.config_port == 0 ? "PORT 1" : "PORT 2", false);
      rows.emplace_back("Input Backend", has_device ? DeviceSource(s_state.device_current) : "-",
                        false);
      rows.emplace_back("Device", has_device ? DeviceName(s_state.device_current) : "none found",
                        false);
      const bool port2 = Port2Attached();
      rows.emplace_back("Player 2 pad", port2 ? "ON" : "OFF", port2);
      for (const auto& row : s_state.control_rows)
      {
        std::string value = row.control->control_ref->GetExpression();
        if (value.empty())
          value = "-";
        rows.emplace_back(row.label, std::move(value), false);
      }
      break;
    }
    case Tab::Mods:
    {
      const bool packs_on = Config::Get(Config::GFX_HIRES_TEXTURES);
      rows.emplace_back("HD texture pack", packs_on ? "ON" : "OFF", packs_on);
      for (const auto& mod : s_state.mod_rows)
      {
        // Only a texture mod is a switch. A skin unpacked from a .rar is shown
        // with what it actually needs instead of an OFF that would never move.
        if (mod.kind == RecompMods::Kind::Textures)
          rows.emplace_back("  " + mod.name, mod.enabled ? "ON" : "OFF", mod.enabled);
        else if (mod.kind == RecompMods::Kind::GameData && mod.installed)
          rows.emplace_back("  " + mod.name, "INSTALLED", true);
        else if (mod.kind == RecompMods::Kind::GameData && mod.installable)
          rows.emplace_back("  " + mod.name, "INSTALL", false);
        else
          rows.emplace_back("  " + mod.name, mod.note, false);
      }

      // Not a toggle -- a fact about the files on disk. It sits with the mods
      // because this is where a player who has just installed a skin will look
      // for it, and because it is the reason netplay may have gone away.
      const auto& data = s_state.game_data;
      const char* value = "?";
      switch (data.status)
      {
      case RecompGameData::Status::Pristine:
        value = "ORIGINAL";
        break;
      case RecompGameData::Status::Modified:
        value = "MODIFIED";
        break;
      case RecompGameData::Status::Unknown:
        value = "UNKNOWN";
        break;
      }
      rows.emplace_back("Game data", value,
                        data.status == RecompGameData::Status::Pristine);
      // The status word alone does not tell a player why netplay vanished, so
      // the sentence goes where the key hint would be. A result from an actual
      // restore request outranks it.
      mod_message = s_state.mod_message.empty() ? data.detail : s_state.mod_message;
      if (data.status == RecompGameData::Status::Modified)
      {
        rows.emplace_back("Restore original game data",
                          data.restorable ? "on next launch" : "disc image missing", false);
      }
      break;
    }
    case Tab::Cheats:
      cheats_enabled = Config::Get(Config::MAIN_ENABLE_CHEATS);
      rows.emplace_back("Enable Cheats", cheats_enabled ? "ON" : "OFF", cheats_enabled);
      for (const auto& row : s_state.cheat_rows)
      {
        const bool on = row.gecko ? s_state.gecko_codes[row.index].enabled
                                  : s_state.ar_codes[row.index].enabled;
        rows.emplace_back(row.label, on ? "ON" : "OFF", on);
      }
      break;
    default:
      for (const Item item : TabItems(tab))
        rows.emplace_back(ItemLabel(item),
                          ItemValue(item, state_slot, s_state.netplay_mode,
                                    s_state.netplay_port,
                                    s_state.netplay_addr_text,
                                    s_state.netplay_addr,
                                    s_state.netplay_addr_octet,
                                    s_state.netplay_hosts,
                                    s_state.netplay_host_index,
                                    s_state.netplay_scanning,
                                    s_state.netplay_scanned),
                          false);
      break;
    }
  }

  const float scale = ImGui::GetIO().DisplayFramebufferScale.x;
  const ImVec2 display = ImGui::GetIO().DisplaySize;

  ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Always,
                          ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(460.0f * scale, 0.0f), ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.88f);

  if (ImGui::Begin("##recomp_menu", nullptr,
                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoInputs |
                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize |
                       ImGuiWindowFlags_NoFocusOnAppearing))
  {
    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1.0f), "RING OUT  -  Ver 1.0");
    ImGui::Separator();

    // Tab bar. Selected row 0 means the tab strip itself has focus, so mark it
    // with arrows to show left/right will move between tabs.
    const bool tab_focused = selected == 0;
    for (int i = 0; i < kTabCount; ++i)
    {
      const Tab t = static_cast<Tab>(i);
      if (i != 0)
        ImGui::SameLine();

      if (t == tab)
      {
        const ImVec4 color = tab_focused ? ImVec4(1.0f, 1.0f, 0.45f, 1.0f) :
                                           ImVec4(1.0f, 0.85f, 0.4f, 1.0f);
        ImGui::TextColored(color, tab_focused ? "<%s>" : "[%s]", TabName(t));
      }
      else
      {
        ImGui::TextColored(ImVec4(0.45f, 0.45f, 0.45f, 1.0f), " %s ", TabName(t));
      }
    }

    ImGui::Separator();
    ImGui::Spacing();

    const float value_column = ImGui::GetContentRegionAvail().x - 110.0f * scale;
    const float row_height = ImGui::GetTextLineHeightWithSpacing();
    const bool scrolling = rows.size() > 14;

    if (scrolling)
      ImGui::BeginChild("##rows", ImVec2(0.0f, row_height * 14.0f), false,
                        ImGuiWindowFlags_NoScrollbar);

    if (rows.empty())
    {
      ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "  (nothing here)");
      if (tab == Tab::Cheats)
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                           "  add codes to GameSettings/GRSEAF.ini");
      if (tab == Tab::Mods)
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                           "  put texture mods in Load/Mods/<name>/");
    }

    for (size_t i = 0; i < rows.size(); ++i)
    {
      const bool is_selected = static_cast<int>(i) + 1 == selected;
      const ImVec4 color = is_selected ? ImVec4(1.0f, 1.0f, 0.45f, 1.0f) :
                                         ImVec4(0.85f, 0.85f, 0.85f, 1.0f);

      ImGui::TextColored(color, "%s %s", is_selected ? ">" : " ", std::get<0>(rows[i]).c_str());

      const std::string& value = std::get<1>(rows[i]);
      if (!value.empty())
      {
        ImGui::SameLine(value_column);
        if (is_selected && detecting)
          ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "[press input]");
        else if (std::get<2>(rows[i]))
          ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", value.c_str());
        else
          ImGui::TextColored(color, "%s", value.c_str());
      }

      if (is_selected && scrolling)
        ImGui::SetScrollHereY(0.5f);
    }

    if (scrolling)
      ImGui::EndChild();

    ImGui::Spacing();
    ImGui::Separator();

    // Read outside the state mutex on purpose: it takes its own, and nothing
    // above still holds ours here.
    const bool netplay_blocked = RecompGameData::NetplayBlocked();
    const char* hint = "Up/Down select   Left/Right change   Space confirm   Esc close";
    if (detecting)
      hint = "Press a key or button...   Esc cancel";
    else if (editing_address)
      hint = "Left/Right octet   Up/Down value   Space done";
    else if (tab_focused)
      hint = "Left/Right switch tab   Down enter list   Esc close";
    else if (tab == Tab::Controls && selected == kPortRow + 1)
      hint = "Left/Right switch player   the rows below bind that pad   Esc close";
    else if (tab == Tab::Controls && selected == kPort2Row + 1)
      hint = "Space toggle   off means one-player only   Esc close";
    else if (tab == Tab::Controls && selected >= kBackendRow + 1 &&
             selected <= kDeviceRow + 1)
      hint = "Left/Right change   Space rescan devices   Esc close";
    else if (tab == Tab::Controls)
      hint = "Space rebind   Left clear   Up/Down select   Esc close";
    else if (tab == Tab::Cheats)
      hint = "Space toggle   Up/Down select   Esc close";
    else if (tab == Tab::Mods)
      hint = "Space toggle   Up/Down select   Esc close";
    // The row can only say "OFF - MODDED" in the space it has, so the sentence
    // that explains it goes here whenever the cursor is on that row.
    else if (tab == Tab::System && netplay_blocked && selected >= 1 &&
             selected <= static_cast<int>(TabItems(tab).size()) &&
             TabItems(tab)[selected - 1] == Item::NetplayStart)
      hint = "Game data is modified -- restore it in MODS to play online.";
    if (tab == Tab::Mods && !mod_message.empty())
      hint = mod_message.c_str();
    ImGui::TextColored(ImVec4(0.65f, 0.65f, 0.65f, 1.0f), "%s", hint);
  }
  ImGui::End();
}

// Drives an in-progress input detection. Emulation is paused while the menu is
// open, so nothing else is polling the controller backends -- UpdateInput has
// to be called here or no input would ever be seen.
void UpdateDetection()
{
  ControllerEmu::Control* control = nullptr;
  ciface::Core::InputDetector::Results results;

  {
    std::lock_guard<std::mutex> guard(s_state.mutex);
    if (s_state.detector == nullptr)
      return;

    g_controller_interface.UpdateInput();
    s_state.detector->Update(std::chrono::seconds(3), std::chrono::milliseconds(0),
                             std::chrono::seconds(5));
    if (!s_state.detector->IsComplete())
      return;

    results = s_state.detector->TakeResults();
    control = s_state.detecting_control;
    s_state.detector.reset();
    s_state.detecting_control = nullptr;
  }

  // Applying the mapping touches InputConfig/ControllerInterface, so do it with
  // the menu lock released.
  if (control == nullptr || results.empty())
    return;

  auto* const pad = GetConfiguredPad();
  if (pad == nullptr)
    return;

  ciface::MappingCommon::RemoveSpuriousTriggerCombinations(&results);
  const std::string expression = ciface::MappingCommon::BuildExpression(
      results, pad->GetDefaultDevice(), ciface::MappingCommon::Quote::On);

  control->control_ref->SetExpression(expression);
  pad->UpdateSingleControlReference(g_controller_interface, control->control_ref.get());
  if (auto* const config = Pad::GetConfig())
    config->SaveConfig();
}

// Debug aid: RECOMP_MENU_AUTOOPEN=<seconds> opens the menu by itself that long
// after the first tick, so the overlay can be verified without a keypress
// (synthetic input does not work on native Wayland).
// Drive the overlay from the pad, so it can be reached on a machine with no
// keyboard. That is the Steam Deck in Game Mode, where Escape -- the only way
// in until now -- cannot be typed at all.
//
// Back opens and closes; the D-pad navigates; A activates; B backs out. None of
// Back/Guide/the stick clicks are bound by the generated GC pad profile, so
// nothing here can fire during play. Names come from Dolphin's SDL backend
// (s_sdl_button_names).
//
// Reads the device the pad is actually mapped to rather than hunting for "a
// gamepad": if the player remapped it, that is still the one in their hands.
void PollMenuGamepad()
{
  ControllerEmu::EmulatedController* const pad = GetMenuPad();
  if (pad == nullptr)
    return;
  const auto device = g_controller_interface.FindDevice(pad->GetDefaultDevice());
  if (device == nullptr)
    return;

  // Device state is refreshed by SI on the CPU thread -- but only while the
  // emulator is running, and opening this menu pauses it. Without pumping it
  // here the first press would open the overlay and then nothing would move.
  // Only while open, so this never races the CPU thread's own polling.
  if (IsOpen())
    g_controller_interface.UpdateInput();

  struct Binding
  {
    const char* input;
    Key key;
  };
  // Analog sticks are deliberately absent: they rest at small non-zero values
  // and would need a deadband and repeat-rate of their own.
  static constexpr Binding kBindings[] = {
      {"Pad N", Key::Up},
      {"Pad S", Key::Down},
      {"Pad W", Key::Left},
      {"Pad E", Key::Right},
      {"Button S", Key::Activate},
  };

  // Edge-triggered: a held button must not repeat, or one press would run the
  // whole way down a list. Sized for the bindings plus B; Back is handled
  // separately below because it needs a hold, not an edge, and so keeps its own
  // state. The spare slot is left rather than renumbering B's index.
  static bool held[std::size(kBindings) + 2] = {};
  const auto pressed = [&](const char* name, bool* was_held) {
    const auto* const input = device->FindInput(name);
    const bool down = input != nullptr && input->GetState() > 0.5;
    const bool edge = down && !*was_held;
    *was_held = down;
    return edge;
  };

  // BACK OPENS ON A HOLD, NOT A TAP.
  //
  // View (the SDL "Back" input) was bound as a plain edge so Game Mode could
  // reach this overlay without a keyboard. But opening PAUSES THE CORE, and on
  // a handheld that button sits right under the thumb: sessions were ending
  // mid-play with nobody meaning to touch it. Instrumenting the edge settled
  // what was happening -- every occurrence logged `Back=1.00` from
  // 'SDL/0/Steam Deck Controller', with no device churn -- so these were real
  // presses, just unintended ones.
  //
  // Only the OPENING direction gets the friction. Opening by accident stops the
  // game; closing by accident just resumes it, so a tap still dismisses.
  {
    using Clock = std::chrono::steady_clock;
    constexpr auto kHoldToOpen = std::chrono::milliseconds(500);

    const auto* const back = device->FindInput("Back");
    const bool down = back != nullptr && back->GetState() > 0.5;

    static bool was_down = false;
    static bool consumed = false;   // this press has already done something
    static Clock::time_point down_since{};

    const bool press = down && !was_down;
    if (press)
    {
      down_since = Clock::now();
      consumed = false;
    }
    was_down = down;

    if (IsOpen())
    {
      // Dismiss on the PRESS, not the release: waiting for release made closing
      // feel laggy, which is the wrong trade for the direction that costs
      // nothing. `consumed` is what stops the hold that OPENED the menu from
      // closing it again -- that press is already spent, and only a NEW press
      // can dismiss.
      if (press && !consumed)
      {
        consumed = true;
        OnEscape();
        return;
      }
    }
    else if (down && !consumed && Clock::now() - down_since >= kHoldToOpen)
    {
      consumed = true;   // one open per press, however long it is held
      OnEscape();
      return;
    }
  }

  if (!IsOpen())
  {
    // Still sample the rest so a button held while opening is not seen as a
    // fresh press on the first frame the menu is up.
    for (std::size_t i = 0; i < std::size(kBindings); ++i)
      pressed(kBindings[i].input, &held[i]);
    pressed("Button E", &held[std::size(kBindings) + 1]);
    return;
  }

  if (pressed("Button E", &held[std::size(kBindings) + 1]))
  {
    OnEscape();  // unwinds one level, exactly as Escape does
    return;
  }
  for (std::size_t i = 0; i < std::size(kBindings); ++i)
  {
    if (pressed(kBindings[i].input, &held[i]))
      OnKey(kBindings[i].key);
  }
}

void HostTick()
{
  PollMenuGamepad();

  // Free Look's camera is driven by its own input mapping (Shift+WASDQE to move,
  // Shift+mouse to look) and needs pumping from the host thread every iteration
  // -- DolphinQt does this from its HotkeyScheduler, which NoGUI has no
  // equivalent of. Device state itself stays fresh because SI polls
  // ControllerInterface on the CPU thread each frame.
  if (Config::Get(Config::FREE_LOOK_ENABLED))
    FreeLook::UpdateInput();

  static const int delay = [] {
    const char* const env = std::getenv("RECOMP_MENU_AUTOOPEN");
    return env != nullptr ? std::atoi(env) : 0;
  }();
  if (delay <= 0)
    return;

  static const auto start = std::chrono::steady_clock::now();
  static bool fired = false;
  if (fired || std::chrono::steady_clock::now() - start < std::chrono::seconds(delay))
    return;

  // RECOMP_MENU_STRESS=<n> hammers open/close n times to reproduce the
  // rapid-Escape deadlock without a human at the keyboard.
  if (const char* const stress = std::getenv("RECOMP_MENU_STRESS"))
  {
    fired = true;
    const int count = std::atoi(stress);
    std::fprintf(stderr, "[menu] stress: %d toggles\n", count);
    for (int i = 0; i < count; ++i)
    {
      OnEscape();
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
      std::fprintf(stderr, "[menu] stress %d/%d open=%d\n", i + 1, count, IsOpen() ? 1 : 0);
    }
    std::fprintf(stderr, "[menu] stress complete, survived\n");
    return;
  }

  fired = true;
  std::fprintf(stderr, "[menu] auto-open firing\n");
  Toggle();

  // RECOMP_MENU_AUTOPAGE=cheats|controls jumps straight to a subpage, so those
  // lists can be verified without navigating.
  if (const char* const page = std::getenv("RECOMP_MENU_AUTOPAGE"))
  {
    // Loaders run before taking the lock -- same rule as OnKey.
    if (std::string(page) == "cheats")
    {
      std::vector<ActionReplay::ARCode> ar;
      std::vector<Gecko::GeckoCode> gecko;
      std::vector<CheatRow> cheat_rows;
      LoadCheatCodesData(&ar, &gecko, &cheat_rows);
      std::lock_guard<std::mutex> guard(s_state.mutex);
      s_state.ar_codes = std::move(ar);
      s_state.gecko_codes = std::move(gecko);
      s_state.cheat_rows = std::move(cheat_rows);
      s_state.tab = Tab::Cheats;
      std::fprintf(stderr, "[menu] auto-page cheats: %zu codes\n", s_state.cheat_rows.size());
    }
    else if (std::string(page) == "controls")
    {
      std::vector<ControlRow> built;
      BuildControlRowsData(&built);
      std::vector<std::string> devices;
      std::string current;
      BuildDeviceListData(&devices, &current);
      std::lock_guard<std::mutex> guard(s_state.mutex);
      s_state.control_rows = std::move(built);
      s_state.devices = std::move(devices);
      s_state.device_current = std::move(current);
      s_state.tab = Tab::Controls;
      std::fprintf(stderr, "[menu] auto-page controls: %zu rows\n", s_state.control_rows.size());
    }
  }
  std::fprintf(stderr, "[menu] auto-open done, open=%d\n", IsOpen() ? 1 : 0);
}

// The menu DOES pause emulation again (Escape = pause), but this must still
// never present.
//
// The original design paused the core and redrew the overlay by calling
// g_presenter->Present() from the host thread. That is unsound: Vulkan command
// buffers belong to the video thread, and "CPU paused" does not mean the video
// thread is idle -- it can still be draining the FIFO. Two threads then touch
// the backend at once. It cost three separate failures: a host-thread deadlock
// against the compositor, and a video thread wedged inside
// VKTexture::TransitionToLayout mid texture upload. Serialising Present alone
// could not fix it, because the collision is with the video thread's *other*
// GPU work, not with another Present.
//
// Pausing is safe now only because the two things that made it unsafe are gone:
//   * the pause itself runs on the CoreState worker, never the host thread, so
//     the compositor deadlock cannot form;
//   * settings are STAGED while paused and applied only after the resume (see
//     s_settings_guard), so no backend reconfigure ever lands on a paused core.
// Presenting from here would reintroduce the third failure, so it stays banned:
// this function still only services input detection, which touches no GPU state.
//
// KNOWN CONSEQUENCE: with the CPU paused the video thread stops producing
// frames, so the overlay is not repainted while the menu is open -- it shows
// whatever was on screen when the pause landed. Fixing that needs a redraw
// posted TO the video thread (AsyncRequests), never a Present from here.
void PumpFrame()
{
  if (!IsOpen())
    return;

  UpdateDetection();

  // With the core paused the CPU thread submits nothing, so the video thread
  // produces no frames and the overlay would sit frozen on whatever was on
  // screen when the pause landed. Fix it the only sound way: ask the VIDEO
  // thread to re-present, never present from here. RunGpuLoop pulls async
  // events BEFORE its "do nothing while paused" early-out, so the request is
  // serviced even while paused, and Presenter::Present() runs OnScreenUI's
  // Finalize/DrawImGui, which is what repaints the menu.
  //
  // Throttled to ~60 Hz: PumpFrame is called once per host-loop iteration,
  // which spins far faster than the display refresh.
  static std::chrono::steady_clock::time_point s_last_repaint{};
  const auto now = std::chrono::steady_clock::now();
  if (now - s_last_repaint < std::chrono::milliseconds(16))
    return;
  s_last_repaint = now;

  AsyncRequests::GetInstance()->PushEvent([] {
    if (g_presenter)
      g_presenter->Present();
  });
}
}  // namespace RecompMenu
