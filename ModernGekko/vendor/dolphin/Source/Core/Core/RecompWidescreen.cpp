// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/RecompWidescreen.h"

#include <atomic>
#include <optional>
#include <string>

#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Core/Config/ConfigManager.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/HW/Memmap.h"
#include "Core/Movie.h"
#include "Core/NetPlay/NetPlayProto.h"
#include "Core/System.h"
#include "VideoCommon/RecompOrthoSqueeze.h"
#include "VideoCommon/VideoConfig.h"

namespace Config
{
// Kept apart from GFX_WIDESCREEN_HACK on purpose: that key is what produced
// RingOut#10, and an install that saved it ON is migrated off it below.
static const Info<bool> GFX_NATIVE_WIDESCREEN{{System::GFX, "Settings", "NativeWidescreen"},
                                              false};
// RecompScreenShape::Shape as an int: 0 16:9, 1 16:10, 2 21:9 (64:27), 3 32:9.
static const Info<int> GFX_SCREEN_SHAPE{{System::GFX, "Settings", "ScreenShape"}, 0};
}  // namespace Config

namespace RecompWidescreen
{
namespace
{
// Where each disc keeps its Screen Ratio setting. Found 2026-09-12: on the US
// disc by diffing RAM between a 4:3 and a WIDE save and write-watching the
// in-game toggle; on JP and PAL by finding the same flag-writing routine by its
// instruction pattern, then locating the menu item that points at it.
//
// The game's options menu is a table of items. The Screen Ratio item keeps its
// value at +8, writes it through a null-terminated list of bound variables, and
// calls its callback at +48 -- which sets the display flag = (value != 0). The
// renderer follows that flag; forcing it alone switches the layout on screen.
// Writing all three leaves the game's own menu showing the right choice and
// lets the game save it like any other setting.
struct Layout
{
  const char* game_id;
  u32 flag_object_pointer;  // r13-relative slot holding the flag's owner
  u32 item;                 // the Screen Ratio menu item
  u32 bound_variable;       // what the item writes its value through
  u32 callback;             // the item's +48; checked before any write
};

constexpr u32 kFlagOffset = 76;
constexpr u32 kItemValueOffset = 8;
constexpr u32 kItemCallbackOffset = 48;

constexpr Layout kLayouts[] = {
    {"GRSEAF", 0x80460870, 0x80287A48, 0x803EDD1C, 0x801693EC},  // USA
    {"GRSJAF", 0x804B79B0, 0x802DEBBC, 0x80445DF4, 0x80164180},  // Japan, Rev 1
    {"GRSPAF", 0x804749B0, 0x8028F358, 0x80401CDC, 0x8016FEB4},  // Europe
    // SC2 Plus, a community mod of the US disc. It hooks the US text in place,
    // and a RAM dump shows the same item, list, callback and flag slot -- checked
    // on its own dump, not assumed from GRSEAF.
    {"GRSEPS", 0x80460870, 0x80287A48, 0x803EDD1C, 0x801693EC},
};

const Layout* FindLayout()
{
  const std::string id = SConfig::GetInstance().GetGameID();
  for (const Layout& layout : kLayouts)
  {
    if (id == layout.game_id)
      return &layout;
  }
  return nullptr;
}

bool InRam(const Memory::MemoryManager& memory, u32 address, u32 size)
{
  return address >= 0x80000000u && address - 0x80000000u + size <= memory.GetRamSizeReal();
}

bool GuestWritesAllowed(Core::System& system)
{
  const auto& movie = system.GetMovie();
  return !NetPlay::IsNetPlayRunning() && !movie.IsRecordingInput() && !movie.IsPlayingInput();
}

// Set by OnFrame when this disc's constants held none of our shapes: a build
// the table was not taken from. Host thread reads it for the menu.
std::atomic<bool> s_foreign_build{false};

// CPU thread only (the movie hook and OnFrame both run there).
RecompScreenShape::MovieGate s_movie_gate;

const RecompScreenShape::Table* FindShapeTable()
{
  return RecompScreenShape::FindTable(SConfig::GetInstance().GetGameID());
}

u32 ReadGuestU32(void* memory, u32 address)
{
  return static_cast<Memory::MemoryManager*>(memory)->Read_U32(address);
}
}  // namespace

bool IsNative()
{
  return FindLayout() != nullptr;
}

bool IsEnabled()
{
  return IsNative() ? Config::Get(Config::GFX_NATIVE_WIDESCREEN) :
                      Config::Get(Config::GFX_WIDESCREEN_HACK);
}

bool Toggle()
{
  if (!IsNative())
  {
    // The old pairing, unchanged: the hack widens the projection and ForceWide
    // shows it at 16:9.
    const bool enable = !Config::Get(Config::GFX_WIDESCREEN_HACK);
    Config::SetBase(Config::GFX_WIDESCREEN_HACK, enable);
    Config::SetBase(Config::GFX_ASPECT_RATIO, enable ? AspectMode::ForceWide : AspectMode::Auto);
    return true;
  }

  if (!GuestWritesAllowed(Core::System::GetInstance()))
    return false;
  Config::SetBase(Config::GFX_NATIVE_WIDESCREEN, !Config::Get(Config::GFX_NATIVE_WIDESCREEN));
  return true;
}

RecompScreenShape::Shape GetShape()
{
  const int v = Config::Get(Config::GFX_SCREEN_SHAPE);
  return v >= 0 && v < RecompScreenShape::kShapeCount ? static_cast<RecompScreenShape::Shape>(v) :
                                                         RecompScreenShape::Shape::Wide16x9;
}

bool ShapesAvailable()
{
  return FindShapeTable() != nullptr && !s_foreign_build.load(std::memory_order_relaxed);
}

bool CycleShape(int direction)
{
  if (!ShapesAvailable() || !GuestWritesAllowed(Core::System::GetInstance()))
    return false;
  const int n = RecompScreenShape::kShapeCount;
  const int next = ((static_cast<int>(GetShape()) + direction) % n + n) % n;
  Config::SetBase(Config::GFX_SCREEN_SHAPE, next);
  return true;
}

void NoteMovieFrame()
{
  s_movie_gate.Note();
}

void OnFrame(Core::System& system)
{
  const Layout* const layout = FindLayout();
  if (layout == nullptr)
    return;

  auto& memory = system.GetMemory();
  if (memory.GetRAM() == nullptr)
    return;

  // Same disc ID, different build: a revision this table was not made from
  // would have other code at these addresses. The item's callback field is the
  // cheapest proof it is the build that was mapped.
  if (memory.Read_U32(layout->item + kItemCallbackOffset) != layout->callback)
    return;

  // Null until the game has built the object, which it does during boot. Until
  // then there is no flag to read, and nothing to write.
  const u32 object = memory.Read_U32(layout->flag_object_pointer);
  if (!InRam(memory, object, kFlagOffset + 1))
    return;

  // Migration. An install that saved the old hack ON was asking for
  // widescreen; give it the real thing and turn the hack off, or the two would
  // stack. GetBase, not Get: netplay syncs the hack through its own layer, and
  // reacting to that every frame would fight it.
  if (Config::GetBase(Config::GFX_WIDESCREEN_HACK))
  {
    Config::SetBase(Config::GFX_WIDESCREEN_HACK, false);
    Config::SetBase(Config::GFX_NATIVE_WIDESCREEN, true);
  }

  bool game_wide = memory.Read_U8(object + kFlagOffset) != 0;
  const bool want = Config::Get(Config::GFX_NATIVE_WIDESCREEN);

  // Only on a mismatch, never as a per-frame poke. The save load rewrites the
  // flag from the card during boot, so this also runs once after that.
  if (want != game_wide && GuestWritesAllowed(system))
  {
    const u32 value = want ? 1 : 0;
    memory.Write_U8(static_cast<u8>(value), object + kFlagOffset);
    memory.Write_U32(value, layout->item + kItemValueOffset);
    memory.Write_U32(value, layout->bound_variable);
    game_wide = want;
  }

  // Screen shape: scale the wide-mode constants (RecompScreenShape.h). 4:3
  // wants the originals back, so turning widescreen off undoes a shape.
  using RecompScreenShape::Shape;
  Shape shown = Shape::Wide16x9;
  if (const RecompScreenShape::Table* table = FindShapeTable())
  {
    const std::optional<Shape> now =
        RecompScreenShape::CurrentShape(*table, ReadGuestU32, &memory);
    s_foreign_build.store(!now.has_value(), std::memory_order_relaxed);
    if (now)
    {
      shown = *now;
      const Shape want_shape = game_wide ? GetShape() : Shape::Wide16x9;
      if (want_shape != *now && GuestWritesAllowed(system))
      {
        for (const RecompScreenShape::Write& w :
             RecompScreenShape::Writes(*table, ReadGuestU32, &memory, want_shape))
          memory.Write_U32(w.bits, w.address);
        shown = want_shape;
      }
    }
  }

  // The display follows the GAME -- its flag and the constants in RAM -- not
  // the preference, so a replay or a netplay session recorded in the other
  // mode still shows correctly. Only the modes this manages are touched: a
  // player who picked 4:3, Stretch or their own Custom ratio keeps it.
  // A movie on screen shows at 16:9 with bars at the sides, not stretched.
  // Display only: the constants in RAM keep the shape. Ticked every frame,
  // managed or not, so the hold never goes stale.
  const bool movie = s_movie_gate.Tick();
  const AspectMode aspect = Config::GetBase(Config::GFX_ASPECT_RATIO);
  const int custom_w = Config::GetBase(Config::GFX_CUSTOM_ASPECT_RATIO_WIDTH);
  const int custom_h = Config::GetBase(Config::GFX_CUSTOM_ASPECT_RATIO_HEIGHT);
  const bool managed = aspect == AspectMode::Auto || aspect == AspectMode::ForceWide ||
                       (aspect == AspectMode::Custom &&
                        RecompScreenShape::IsOurCustomAspect(custom_w, custom_h));
  if (!managed)
  {
    RecompOrthoSqueeze::g_factor.store(1.0f, std::memory_order_relaxed);
    return;
  }
  const std::optional<RecompScreenShape::AspectPair> pair =
      game_wide && !movie ? RecompScreenShape::CustomAspect(shown) : std::nullopt;
  // The 2D follows the display: narrowed only while it shows our shape.
  RecompOrthoSqueeze::g_factor.store(pair ? RecompScreenShape::OrthoSqueeze(shown) : 1.0f,
                                     std::memory_order_relaxed);
  const AspectMode desired =
      !game_wide ? AspectMode::Auto : pair ? AspectMode::Custom : AspectMode::ForceWide;
  if (pair && (custom_w != pair->width || custom_h != pair->height))
  {
    Config::SetBase(Config::GFX_CUSTOM_ASPECT_RATIO_WIDTH, pair->width);
    Config::SetBase(Config::GFX_CUSTOM_ASPECT_RATIO_HEIGHT, pair->height);
  }
  if (aspect != desired)
    Config::SetBase(Config::GFX_ASPECT_RATIO, desired);
}
}  // namespace RecompWidescreen
