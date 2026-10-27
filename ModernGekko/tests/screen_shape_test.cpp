// The ultrawide screen shapes (RecompScreenShape.h): the per-disc constant
// tables, the scaling maths, the guard that refuses a build the tables were not
// made from, and which Dolphin aspect each shape shows at.
//
// Fake guest RAM stands in for the console: the header reads through a
// function pointer, so nothing here needs Dolphin.

#include "RecompScreenShape.h"
#include "../VideoCommon/RecompOrthoSqueeze.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>

namespace {

using namespace RecompScreenShape;

int g_failures = 0;

void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++g_failures;
  }
}

float AsFloat(std::uint32_t bits) {
  float f;
  std::memcpy(&f, &bits, sizeof f);
  return f;
}

bool Near(float a, float b) { return std::fabs(a - b) < 1e-3f; }

struct FakeRam {
  std::map<std::uint32_t, std::uint32_t> words;
  static std::uint32_t Read(void* ctx, std::uint32_t address) {
    const auto& w = static_cast<FakeRam*>(ctx)->words;
    const auto it = w.find(address);
    return it == w.end() ? 0 : it->second;
  }
  void Fill(const Table& t, Shape s) {
    for (const Constant& c : t.constants) words[c.address] = ValueBits(c, s);
  }
};

const Constant* ByAddress(const Table& t, std::uint32_t address) {
  for (const Constant& c : t.constants)
    if (c.address == address) return &c;
  return nullptr;
}

void TestTables() {
  Check(FindTable("GRSEAF") != nullptr, "US disc has a table");
  Check(FindTable("GRSEPS") != nullptr, "SC2 Plus has a table");
  Check(FindTable("GRSJAF") == nullptr, "JP: its wide mode has none of these constants");
  Check(FindTable("GRSPAF") != nullptr, "PAL has a table");
  // PAL, read from its own DOL by the constants' readers (2026-10-25): the
  // same five, with the same reader counts as US (3 / 5 / 12 / 12 / 1).
  if (const Table* pal = FindTable("GRSPAF")) {
    Check(ByAddress(*pal, 0x80476828) && ByAddress(*pal, 0x80476828)->original_bits == 0x3F9EE584 &&
              ByAddress(*pal, 0x80476828)->role == Role::Factor, "PAL 3D factor");
    Check(ByAddress(*pal, 0x80475640) && ByAddress(*pal, 0x80475640)->role == Role::Squeeze, "PAL 2D squeeze");
    Check(ByAddress(*pal, 0x80478228) && ByAddress(*pal, 0x80478228)->role == Role::Squeeze, "PAL HUD squeeze");
    Check(ByAddress(*pal, 0x80478224) && ByAddress(*pal, 0x80478224)->original_bits == 0xC278E38C &&
              ByAddress(*pal, 0x80478224)->role == Role::Offset, "PAL HUD offset");
    Check(ByAddress(*pal, 0x80478828) && ByAddress(*pal, 0x80478828)->role == Role::Squeeze, "PAL squeeze at 8015A9FC");
    FakeRam ram;
    ram.Fill(*pal, Shape::Wide16x9);
    Check(CurrentShape(*pal, FakeRam::Read, &ram) == Shape::Wide16x9, "PAL originals read as 16:9");
    Check(Writes(*pal, FakeRam::Read, &ram, Shape::Ultra21x9).size() == 5, "PAL 16:9 -> 21:9 writes all five");
  }
  Check(FindTable("XXXXXX") == nullptr, "unknown disc has no table");
  const Table* t = FindTable("GRSEAF");
  if (t == nullptr) return;
  // The bits the US DOL holds (read from main.dol, 2026-10-25).
  Check(ByAddress(*t, 0x804626B0) && ByAddress(*t, 0x804626B0)->original_bits == 0x3F9EE584, "3D factor original");
  Check(ByAddress(*t, 0x80461500) && ByAddress(*t, 0x80461500)->original_bits == 0x3F4E38E4, "2D squeeze original");
  Check(ByAddress(*t, 0x80464030) && ByAddress(*t, 0x80464030)->original_bits == 0x3F4E38E4, "HUD squeeze original");
  Check(ByAddress(*t, 0x8046402C) && ByAddress(*t, 0x8046402C)->original_bits == 0xC278E38C, "HUD offset original");
  Check(ByAddress(*t, 0x80464608) && ByAddress(*t, 0x80464608)->original_bits == 0x3F4E38E4, "squeeze at 80154F74 original");
  Check(ByAddress(*t, 0x804615F8) == nullptr, "the unknown 0.8 is not touched");
}

void TestMaths() {
  const Table* t = FindTable("GRSEAF");
  if (t == nullptr) return;
  const Constant& factor = *ByAddress(*t, 0x804626B0);
  const Constant& squeeze = *ByAddress(*t, 0x80461500);
  const Constant& offset = *ByAddress(*t, 0x8046402C);
  for (const Constant& c : t->constants)
    Check(ValueBits(c, Shape::Wide16x9) == c.original_bits, "16:9 is the original bits exactly");
  Check(Near(AsFloat(ValueBits(factor, Shape::Ultra21x9)), 1.65517f), "21:9 (64:27) factor");
  Check(Near(AsFloat(ValueBits(squeeze, Shape::Ultra21x9)), 0.60417f), "21:9 squeeze");
  Check(Near(AsFloat(ValueBits(offset, Shape::Ultra21x9)), -126.667f), "21:9 offset");
  Check(Near(AsFloat(ValueBits(factor, Shape::Super32x9)), 2.48276f), "32:9 factor");
  Check(Near(AsFloat(ValueBits(squeeze, Shape::Super32x9)), 0.40278f), "32:9 squeeze");
  Check(Near(AsFloat(ValueBits(factor, Shape::Wide16x10)), 1.11724f), "16:10 factor");
}

void TestGuardAndWrites() {
  const Table* t = FindTable("GRSEAF");
  if (t == nullptr) return;
  FakeRam ram;
  ram.Fill(*t, Shape::Wide16x9);
  Check(CurrentShape(*t, FakeRam::Read, &ram) == Shape::Wide16x9, "originals read as 16:9");

  auto w = Writes(*t, FakeRam::Read, &ram, Shape::Ultra21x9);
  Check(w.size() == 5, "16:9 -> 21:9 writes all five");
  for (const Write& x : w)
    Check(x.bits == ValueBits(*ByAddress(*t, x.address), Shape::Ultra21x9), "writes carry 21:9 bits");

  ram.Fill(*t, Shape::Ultra21x9);
  Check(CurrentShape(*t, FakeRam::Read, &ram) == Shape::Ultra21x9, "our 21:9 values are recognised");
  w = Writes(*t, FakeRam::Read, &ram, Shape::Super32x9);
  Check(w.size() == 5, "21:9 -> 32:9 mid-fight writes all five");
  for (const Write& x : w)
    Check(x.bits == ValueBits(*ByAddress(*t, x.address), Shape::Super32x9), "writes carry 32:9 bits");

  // A save state made at 32:9, loaded with the preference at 16:9 (or with
  // Widescreen turned off): ours, so the originals go back.
  ram.Fill(*t, Shape::Super32x9);
  w = Writes(*t, FakeRam::Read, &ram, Shape::Wide16x9);
  Check(w.size() == 5, "32:9 -> 16:9 restores all five");
  for (const Write& x : w)
    Check(x.bits == ByAddress(*t, x.address)->original_bits, "restores the original bits");

  Check(Writes(*t, FakeRam::Read, &ram, Shape::Super32x9).empty(), "already there: no writes");

  // Same game ID, other data at one address: a build the table was not made from.
  ram.Fill(*t, Shape::Wide16x9);
  ram.words[0x80464030] = 0xDEADBEEF;
  Check(!CurrentShape(*t, FakeRam::Read, &ram).has_value(), "foreign build is not a shape");
  Check(Writes(*t, FakeRam::Read, &ram, Shape::Super32x9).empty(), "foreign build: never written");

  // Mixed shapes (half a batch) are not ours either.
  ram.Fill(*t, Shape::Wide16x9);
  ram.words[0x804626B0] = ValueBits(*ByAddress(*t, 0x804626B0), Shape::Ultra21x9);
  Check(!CurrentShape(*t, FakeRam::Read, &ram).has_value(), "mixed shapes are not a shape");
}

void TestAspect() {
  Check(!CustomAspect(Shape::Wide16x9).has_value(), "16:9 uses ForceWide, not Custom");
  const auto u = CustomAspect(Shape::Ultra21x9);
  Check(u && u->width == 64 && u->height == 27, "21:9 shows at 64:27");
  const auto s = CustomAspect(Shape::Super32x9);
  Check(s && s->width == 32 && s->height == 9, "32:9 shows at 32:9");
  const auto d = CustomAspect(Shape::Wide16x10);
  Check(d && d->width == 16 && d->height == 10, "16:10 shows at 16:10");
  Check(IsOurCustomAspect(64, 27) && IsOurCustomAspect(32, 9) && IsOurCustomAspect(16, 10), "our pairs");
  Check(!IsOurCustomAspect(5, 4), "a player's own Custom aspect is not ours");
  Check(std::strcmp(Label(Shape::Ultra21x9), "21:9") == 0 && std::strcmp(Label(Shape::Wide16x10), "16:10") == 0,
        "labels");
}

// Movies are 16:9 video: while one is on screen the display drops to 16:9
// (bars at the sides) instead of stretching it to the shape. The gate is fed
// one Note() per decoded movie frame and one Tick() per emulated frame.
void TestMovieGate() {
  MovieGate gate;
  Check(!gate.Tick(), "no movie yet: not showing");
  gate.Note();
  Check(gate.Tick(), "a movie frame: showing");
  // Movies decode at 30 fps, so a frame arrives every other tick; a short gap
  // must not flicker the display back to the shape.
  for (int i = 1; i < kMovieHoldFrames; ++i)
    Check(gate.Tick(), "held through a gap shorter than the hold");
  Check(!gate.Tick(), "movie over: back to the shape after the hold");
  gate.Note();
  Check(gate.Tick(), "next movie: showing again");
}

// The game's 2D (menus, HUD) is 4:3 art the game itself only stretches for
// 16:9. Wider shapes narrow its orthographic draws so the 2D shows exactly as
// at 16:9, centred; narrower shapes leave it alone (it would not fit).
void TestOrthoSqueeze() {
  Check(OrthoSqueeze(Shape::Wide16x9) == 1.0f, "16:9: 2D untouched");
  Check(Near(OrthoSqueeze(Shape::Ultra21x9), 0.75f), "21:9 (64:27): 2D in the middle 3/4");
  Check(Near(OrthoSqueeze(Shape::Super32x9), 0.5f), "32:9: 2D in the middle half");
  Check(OrthoSqueeze(Shape::Wide16x10) == 1.0f, "16:10 is narrower than 16:9: 2D untouched");
}

// A 2D draw spanning the whole screen width -- a fade, a flash, the pause dim,
// the replay's cinema bar -- is meant to cover the screen, so it is NOT
// narrowed. Extents are in unsqueezed clip space, where the screen is -1..1.
void TestFullWidthDraw() {
  using RecompOrthoSqueeze::CoversFullWidth;
  Check(CoversFullWidth(-1.0f, 1.0f), "0..640 quad covers the screen");
  Check(CoversFullWidth(-1.05f, 1.2f), "overscanned quad covers the screen");
  Check(!CoversFullWidth(-0.9f, -0.1f), "a health bar does not");
  Check(!CoversFullWidth(-1.0f, 0.5f), "left two-thirds does not");
  Check(!CoversFullWidth(-0.5f, 1.0f), "right three-quarters does not");
}

// How the factor reaches a projection (Dolphin's 4x4, row-major). 2D: x scale
// and x translation, so it narrows around the centre. 3D: only the off-centre
// x term, so a camera the game shifted sideways to sit beside its 2D (the
// character-select model) moves in with the 2D, and a centred camera -- every
// fight -- is untouched.
void TestApplyToProjection() {
  using RecompOrthoSqueeze::ApplyToProjection;
  float ortho[16] = {2.f / 640, 0, 0, -1, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  ApplyToProjection(ortho, true, 0.75f);
  Check(Near(ortho[0], 0.75f * 2.f / 640) && Near(ortho[3], -0.75f), "2D: scale and offset narrowed");
  Check(ortho[5] == 1.f, "2D: y untouched");
  float persp[16] = {1.2f, 0, -0.4f, 0, 0, 1.6f, 0, 0, 0, 0, -1, -1, 0, 0, -1, 0};
  ApplyToProjection(persp, false, 0.5f);
  Check(persp[0] == 1.2f, "3D: x scale untouched (the game's own factor already widens it)");
  Check(Near(persp[2], -0.2f), "3D: off-centre x term follows the 2D");
  float centred[16] = {1.2f, 0, 0, 0, 0, 1.6f, 0, 0, 0, 0, -1, -1, 0, 0, -1, 0};
  ApplyToProjection(centred, false, 0.5f);
  Check(centred[2] == 0.f && centred[0] == 1.2f, "3D: a centred camera is unchanged");
}

}  // namespace

int main() {
  TestTables();
  TestMaths();
  TestGuardAndWrites();
  TestAspect();
  TestMovieGate();
  TestOrthoSqueeze();
  TestFullWidthDraw();
  TestApplyToProjection();
  if (g_failures == 0) std::printf("screen_shape: all checks passed\n");
  return g_failures == 0 ? 0 : 1;
}
