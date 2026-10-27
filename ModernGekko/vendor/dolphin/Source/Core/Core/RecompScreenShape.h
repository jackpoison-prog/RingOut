// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Ultrawide screen shapes on top of the game's own 16:9 mode (RecompWidescreen).
//
// In WIDE mode the game swaps in one small family of constants: a 3D aspect
// factor (1.24138 = 36/29) and the 2D squeeze that maps its 4:3-authored layout
// into the anamorphic frame (0.80556 = 29/36, plus an offset around x=320).
// Scaling that family from 16:9 to another shape makes the game lay its 3D and
// 2D out for that shape, the same way it already does for 16:9. The recompiled
// code reads every one of them from guest RAM (checked on the US module,
// 2026-10-25), so writing RAM reaches the native code.
//
// No Dolphin headers: the tables and maths are unit-tested on their own
// (tests/screen_shape_test.cpp); RecompWidescreen does the reads and writes.

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>
#include <vector>

namespace RecompScreenShape
{
enum class Shape : std::uint8_t
{
  Wide16x9 = 0,
  Wide16x10,
  Ultra21x9,  // 64:27 -- what 2560x1080 and 3440x1440 panels really are
  Super32x9,
};
constexpr int kShapeCount = 4;

enum class Role : std::uint8_t
{
  Factor,   // grows with the shape: the 3D aspect
  Squeeze,  // 1 / factor: the 2D squeeze around x=320
  Offset,   // 320 * (squeeze - 1): pairs with a squeeze in the HUD inverse map
};

struct Constant
{
  std::uint32_t address;
  std::uint32_t original_bits;  // exactly what the disc's DOL holds
  Role role;
};

struct Table
{
  std::string_view game_id;
  std::array<Constant, 5> constants;
};

using ReadU32 = std::uint32_t (*)(void* ctx, std::uint32_t address);

struct Write
{
  std::uint32_t address;
  std::uint32_t bits;
};

struct AspectPair
{
  int width;
  int height;
};

namespace detail
{
// USA and SC2 Plus share one layout (Plus checked on its own dump for the
// native 16:9 work). 0x804615F8 (0.8 at 8001A888, purpose unknown) is
// deliberately absent until an on-screen check says it matters.
constexpr std::array<Constant, 5> kUsConstants = {{
    {0x804626B0, 0x3F9EE584, Role::Factor},   // 1.24138, 3D aspect (80085174/852F8/85790)
    {0x80461500, 0x3F4E38E4, Role::Squeeze},  // 0.80556, 2D squeeze (80013704)
    {0x80464030, 0x3F4E38E4, Role::Squeeze},  // 0.80556, menu/HUD inverse map
    {0x8046402C, 0xC278E38C, Role::Offset},   // -62.222, menu/HUD inverse map
    {0x80464608, 0x3F4E38E4, Role::Squeeze},  // 0.80556 (80150CB8)
}};

// PAL: the same five, read from its own DOL by their readers (3 / 5 / 12 / 12 / 1,
// as on US). JP has none of these values -- its wide mode is built differently
// (no IsWide(), no squeeze helpers) -- so it stays at 16:9.
constexpr std::array<Constant, 5> kPalConstants = {{
    {0x80476828, 0x3F9EE584, Role::Factor},   // 1.24138, 3D aspect (80085E18/85F9C/86434)
    {0x80475640, 0x3F4E38E4, Role::Squeeze},  // 0.80556, 2D squeeze (8001688C)
    {0x80478228, 0x3F4E38E4, Role::Squeeze},  // 0.80556, menu/HUD inverse map
    {0x80478224, 0xC278E38C, Role::Offset},   // -62.222, menu/HUD inverse map
    {0x80478828, 0x3F4E38E4, Role::Squeeze},  // 0.80556 (8015A9FC)
}};

constexpr std::array<Table, 3> kTables = {{
    {"GRSEAF", kUsConstants},
    {"GRSEPS", kUsConstants},
    {"GRSPAF", kPalConstants},
}};

inline double Aspect(Shape s)
{
  switch (s)
  {
  case Shape::Wide16x10:
    return 16.0 / 10.0;
  case Shape::Ultra21x9:
    return 64.0 / 27.0;
  case Shape::Super32x9:
    return 32.0 / 9.0;
  case Shape::Wide16x9:
  default:
    return 16.0 / 9.0;
  }
}

inline float FromBits(std::uint32_t bits)
{
  float f;
  std::memcpy(&f, &bits, sizeof f);
  return f;
}

inline std::uint32_t ToBits(float f)
{
  std::uint32_t bits;
  std::memcpy(&bits, &f, sizeof bits);
  return bits;
}
}  // namespace detail

inline const Table* FindTable(std::string_view game_id)
{
  for (const Table& t : detail::kTables)
  {
    if (t.game_id == game_id)
      return &t;
  }
  return nullptr;
}

// The bits a constant holds at shape s. 16:9 is the disc's own value, exactly,
// so restoring never depends on a float round trip.
inline std::uint32_t ValueBits(const Constant& c, Shape s)
{
  if (s == Shape::Wide16x9)
    return c.original_bits;
  const double scale = detail::Aspect(s) / detail::Aspect(Shape::Wide16x9);
  const double original = detail::FromBits(c.original_bits);
  double value = original;
  switch (c.role)
  {
  case Role::Factor:
    value = original * scale;
    break;
  case Role::Squeeze:
    value = original / scale;
    break;
  case Role::Offset:
    value = 320.0 * ((1.0 + original / 320.0) / scale - 1.0);
    break;
  }
  return detail::ToBits(static_cast<float>(value));
}

// Which shape guest RAM holds now. nullopt when it is none of ours -- a build
// these addresses were not taken from, or a half-written batch -- and then
// nothing may be written.
inline std::optional<Shape> CurrentShape(const Table& t, ReadU32 read, void* ctx)
{
  for (int i = 0; i < kShapeCount; ++i)
  {
    const Shape s = static_cast<Shape>(i);
    bool all = true;
    for (const Constant& c : t.constants)
    {
      if (read(ctx, c.address) != ValueBits(c, s))
      {
        all = false;
        break;
      }
    }
    if (all)
      return s;
  }
  return std::nullopt;
}

// The writes that take RAM to `want`: all five, or none (already there, or not
// ours to touch).
inline std::vector<Write> Writes(const Table& t, ReadU32 read, void* ctx, Shape want)
{
  std::vector<Write> out;
  const std::optional<Shape> now = CurrentShape(t, read, ctx);
  if (!now || *now == want)
    return out;
  for (const Constant& c : t.constants)
    out.push_back({c.address, ValueBits(c, want)});
  return out;
}

// Dolphin's Custom aspect for a shape; 16:9 stays on ForceWide as before.
inline std::optional<AspectPair> CustomAspect(Shape s)
{
  switch (s)
  {
  case Shape::Wide16x10:
    return AspectPair{16, 10};
  case Shape::Ultra21x9:
    return AspectPair{64, 27};
  case Shape::Super32x9:
    return AspectPair{32, 9};
  case Shape::Wide16x9:
  default:
    return std::nullopt;
  }
}

// A Custom aspect we set. Anything else is the player's own choice and is left
// alone.
inline bool IsOurCustomAspect(int width, int height)
{
  return (width == 16 && height == 10) || (width == 64 && height == 27) ||
         (width == 32 && height == 9);
}

// The game's 2D -- menus and HUD -- is 4:3 art that even its own 16:9 mode
// only stretches; none of the constants above reach it. On a wider shape the
// video backend narrows the game's orthographic (2D) draws by this factor, so
// the 2D shows exactly as it does at 16:9, centred, while the 3D fills the
// screen. A shape narrower than 16:9 leaves it alone: it would not fit.
inline float OrthoSqueeze(Shape s)
{
  const double k = detail::Aspect(Shape::Wide16x9) / detail::Aspect(s);
  return k < 1.0 ? static_cast<float>(k) : 1.0f;
}

// Movies are 16:9 video and would stretch to an ultrawide shape, so while one
// is on screen the display drops to 16:9. The movie player calls Note() for
// each frame it converts (30 fps, so every other emulated frame); Tick() runs
// once per emulated frame and says whether a movie is showing. The hold rides
// over gaps between decoded frames so the display does not flicker.
constexpr int kMovieHoldFrames = 20;

struct MovieGate
{
  int frames_since = kMovieHoldFrames;

  void Note() { frames_since = 0; }

  bool Tick()
  {
    const bool showing = frames_since < kMovieHoldFrames;
    if (frames_since < kMovieHoldFrames)
      ++frames_since;
    return showing;
  }
};

inline const char* Label(Shape s)
{
  switch (s)
  {
  case Shape::Wide16x10:
    return "16:10";
  case Shape::Ultra21x9:
    return "21:9";
  case Shape::Super32x9:
    return "32:9";
  case Shape::Wide16x9:
  default:
    return "16:9";
  }
}
}  // namespace RecompScreenShape
