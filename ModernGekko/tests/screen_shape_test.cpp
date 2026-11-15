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
  // same four, with the same reader counts as US (3 / 12 / 12 / 1).
  if (const Table* pal = FindTable("GRSPAF")) {
    Check(ByAddress(*pal, 0x80476828) && ByAddress(*pal, 0x80476828)->original_bits == 0x3F9EE584 &&
              ByAddress(*pal, 0x80476828)->role == Role::Factor, "PAL 3D factor");
    Check(ByAddress(*pal, 0x80475640) == nullptr, "PAL 2D position squeeze stays 16:9");
    Check(ByAddress(*pal, 0x80478228) && ByAddress(*pal, 0x80478228)->role == Role::Squeeze, "PAL HUD squeeze");
    Check(ByAddress(*pal, 0x80478224) && ByAddress(*pal, 0x80478224)->original_bits == 0xC278E38C &&
              ByAddress(*pal, 0x80478224)->role == Role::Offset, "PAL HUD offset");
    Check(ByAddress(*pal, 0x80478828) && ByAddress(*pal, 0x80478828)->role == Role::Squeeze, "PAL squeeze at 8015A9FC");
    FakeRam ram;
    ram.Fill(*pal, Shape::Wide16x9);
    Check(CurrentShape(*pal, FakeRam::Read, &ram) == Shape::Wide16x9, "PAL originals read as 16:9");
    Check(Writes(*pal, FakeRam::Read, &ram, Shape::Ultra21x9).size() == 4, "PAL 16:9 -> 21:9 writes all four");
  }
  Check(FindTable("XXXXXX") == nullptr, "unknown disc has no table");
  const Table* t = FindTable("GRSEAF");
  if (t == nullptr) return;
  // The bits the US DOL holds (read from main.dol, 2026-10-25).
  Check(ByAddress(*t, 0x804626B0) && ByAddress(*t, 0x804626B0)->original_bits == 0x3F9EE584, "3D factor original");
  // The GPU narrows 2D; scaling this too overlapped the portrait grid.
  Check(ByAddress(*t, 0x80461500) == nullptr, "2D position squeeze stays 16:9");
  Check(ByAddress(*t, 0x80464030) && ByAddress(*t, 0x80464030)->original_bits == 0x3F4E38E4, "HUD squeeze original");
  Check(ByAddress(*t, 0x8046402C) && ByAddress(*t, 0x8046402C)->original_bits == 0xC278E38C, "HUD offset original");
  Check(ByAddress(*t, 0x80464608) && ByAddress(*t, 0x80464608)->original_bits == 0x3F4E38E4, "squeeze at 80154F74 original");
  Check(ByAddress(*t, 0x804615F8) == nullptr, "the unknown 0.8 is not touched");
}

void TestMaths() {
  const Table* t = FindTable("GRSEAF");
  if (t == nullptr) return;
  const Constant& factor = *ByAddress(*t, 0x804626B0);
  const Constant& squeeze = *ByAddress(*t, 0x80464030);
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
  Check(w.size() == 4, "16:9 -> 21:9 writes all four");
  for (const Write& x : w)
    Check(x.bits == ValueBits(*ByAddress(*t, x.address), Shape::Ultra21x9), "writes carry 21:9 bits");

  ram.Fill(*t, Shape::Ultra21x9);
  Check(CurrentShape(*t, FakeRam::Read, &ram) == Shape::Ultra21x9, "our 21:9 values are recognised");
  w = Writes(*t, FakeRam::Read, &ram, Shape::Super32x9);
  Check(w.size() == 4, "21:9 -> 32:9 mid-fight writes all four");
  for (const Write& x : w)
    Check(x.bits == ValueBits(*ByAddress(*t, x.address), Shape::Super32x9), "writes carry 32:9 bits");

  // A save state made at 32:9, loaded with the preference at 16:9 (or with
  // Widescreen turned off): ours, so the originals go back.
  ram.Fill(*t, Shape::Super32x9);
  w = Writes(*t, FakeRam::Read, &ram, Shape::Wide16x9);
  Check(w.size() == 4, "32:9 -> 16:9 restores all four");
  for (const Write& x : w)
    Check(x.bits == ByAddress(*t, x.address)->original_bits, "restores the original bits");

  Check(Writes(*t, FakeRam::Read, &ram, Shape::Super32x9).empty(), "already there: no writes");

  // The face-off screen: the HUD inverse map goes back to 16:9 while the
  // shape's other constants stay, and the mixed state is still recognised.
  ram.Fill(*t, Shape::Super32x9);
  w = Writes(*t, FakeRam::Read, &ram, Shape::Super32x9, true);
  Check(w.size() == 4, "face-off at 32:9 rewrites the set");
  for (const Write& x : w)
  {
    const Constant& c = *ByAddress(*t, x.address);
    ram.words[x.address] = x.bits;
    Check(x.bits == (c.face_off_16x9 ? c.original_bits : ValueBits(c, Shape::Super32x9)),
          "face-off: inverse map at 16:9, the rest at 32:9");
  }
  bool fo = false;
  Check(CurrentShape(*t, FakeRam::Read, &ram, &fo) == Shape::Super32x9 && fo,
        "face-off form reads as 32:9, face-off");
  Check(Writes(*t, FakeRam::Read, &ram, Shape::Super32x9, true).empty(), "face-off: already there");
  w = Writes(*t, FakeRam::Read, &ram, Shape::Super32x9);
  Check(w.size() == 4, "leaving the face-off restores the wide set");
  Check(ByAddress(*t, 0x80464030)->face_off_16x9 && ByAddress(*t, 0x8046402C)->face_off_16x9 &&
            !ByAddress(*t, 0x804626B0)->face_off_16x9 && !ByAddress(*t, 0x80464608)->face_off_16x9,
        "only the inverse map is held");

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
  Check(RecompOrthoSqueeze::IsBackgroundTile(-1.5847f, 0.0153f), "a 1.6 tile drifting off the left");
  Check(RecompOrthoSqueeze::IsBackgroundTile(0.0153f, 1.6153f), "its neighbour");
  Check(RecompOrthoSqueeze::IsBackgroundTile(2.199f, 5.399f), "a 3.2 tile");
  Check(!RecompOrthoSqueeze::IsBackgroundTile(-1.625f, -0.022f), "a panel sliding in, width 1.603");
  Check(!RecompOrthoSqueeze::IsBackgroundTile(-1.0035f, -0.0785f), "the weapon bar");
  Check(!RecompOrthoSqueeze::IsBackgroundTile(-0.1063f, 0.9688f), "a 1.075 layer");
  Check(CoversFullWidth(-1.0f, 1.0f), "0..640 quad covers the screen");
  Check(CoversFullWidth(-1.05f, 1.2f), "overscanned quad covers the screen");
  Check(!CoversFullWidth(-0.9f, -0.1f), "a health bar does not");
  Check(!CoversFullWidth(-1.0f, 0.5f), "left two-thirds does not");
  Check(!CoversFullWidth(-0.5f, 1.0f), "right three-quarters does not");
}

// A draw against one screen edge belongs to the screen: character select's
// pillars (narrow) move out to the edge at 16:9 width, its side panels (wide)
// stretch out from an inner side that stays where the squeeze puts it. The
// fight HUD has no such draw (none touched one edge in a logged fight frame).
void TestEdgeAnchor() {
  using RecompOrthoSqueeze::EdgeAnchor;
  const float k = 0.75f;
  const auto at = [](const RecompOrthoSqueeze::EdgeMap& m, float x) { return m.a * x + m.b; };
  Check(!EdgeAnchor(-0.9f, -0.1f, k).on, "a health bar is not anchored");
  Check(!EdgeAnchor(-1.0f, 1.0f, k).on, "a full-width draw is not anchored");
  const auto pillar = EdgeAnchor(-1.0f, -0.95f, k);
  Check(pillar.on && Near(at(pillar, -1.0f), -1.0f) && Near(pillar.a, k), "left pillar: at the edge, 16:9 width");
  const auto rpillar = EdgeAnchor(0.95f, 1.0f, k);
  Check(rpillar.on && Near(at(rpillar, 1.0f), 1.0f) && Near(rpillar.a, k), "right pillar: at the edge, 16:9 width");
  const auto panel = EdgeAnchor(-1.0f, -0.2f, k);
  Check(panel.on && Near(at(panel, -1.0f), -1.0f) && Near(at(panel, -0.2f), -0.2f * k), "left panel: edge to edge, inner side squeezed");
  const auto half = EdgeAnchor(0.0f, 1.0f, k);
  Check(half.on && Near(at(half, 0.0f), 0.0f) && Near(at(half, 1.0f), 1.0f), "right half: unsqueezed");
  // The weapon-select name bar runs past the left edge (-1.0035 in a logged
  // frame) and carries the socket row in its texture, while the lit dots are
  // separate squeezed draws: stretched, the bar's sockets left the dots. Past
  // an edge means layout, so it squeezes like the dots; so does a panel sliding
  // in from off-screen.
  Check(!EdgeAnchor(-1.0035f, -0.0785f, k).on, "weapon bar past the left edge: squeezed, not anchored");
  Check(!EdgeAnchor(-1.05f, -0.49f, k).on, "panel sliding in from the left: squeezed");
  Check(!EdgeAnchor(0.3f, 1.01f, k).on, "panel past the right edge: squeezed");
  Check(EdgeAnchor(-1.001f, -0.2f, k).on, "a frame within rounding of the edge is still anchored");
}

// How the factor reaches a projection (Dolphin's 4x4, row-major). 2D: x scale
// and x translation, so it narrows around the centre. 3D: the caller passes 1
// except on the face-off screen, whose fighters stand over 2D names; there x
// scale and the off-centre term narrow too.
void TestApplyToProjection() {
  using RecompOrthoSqueeze::ApplyToProjection;
  float ortho[16] = {2.f / 640, 0, 0, -1, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  ApplyToProjection(ortho, true, 0.75f);
  Check(Near(ortho[0], 0.75f * 2.f / 640) && Near(ortho[3], -0.75f), "2D: scale and offset narrowed");
  Check(ortho[5] == 1.f, "2D: y untouched");
  float persp[16] = {1.2f, 0, -0.4f, 0, 0, 1.6f, 0, 0, 0, 0, -1, -1, 0, 0, -1, 0};
  ApplyToProjection(persp, false, 1.0f);
  Check(persp[0] == 1.2f && persp[2] == -0.4f, "3D at k 1 (every screen but the face-off) is unchanged");
  float faceoff[16] = {1.2f, 0, -0.4f, 0, 0, 1.6f, 0, 0, 0, 0, -1, -1, 0, 0, -1, 0};
  ApplyToProjection(faceoff, false, 0.5f);
  Check(Near(faceoff[0], 0.6f) && Near(faceoff[2], -0.2f) && faceoff[5] == 1.6f,
        "face-off 3D: x scale and off-centre term narrow with the 2D, y untouched");
}

// What the game sweeps over an anchored panel goes with it. The mode menu's
// highlight is a bar from the left edge to its inner side (-1..-0.275) with a
// glint swept over it: one quad drawn as two lone triangles, split where the
// glint is. Mapped each on its own -- the left half by the edge rule, the right
// half squeezed -- the halves part, and the bar's left showed a strip and a dark
// gap at 21:9 for half a second every two. Each triangle on the bar takes the
// bar's map; quads (the caption over it, character select's pillars inside its
// panels) keep their own.
void TestOnAnchor() {
  using RecompOrthoSqueeze::EdgeAnchor;
  using RecompOrthoSqueeze::OnAnchor;
  const float k = 0.75f;
  const auto at = [](const RecompOrthoSqueeze::EdgeMap& m, float x) { return m.a * x + m.b; };
  const RecompOrthoSqueeze::Anchor bar{EdgeAnchor(-1.0f, -0.275f, k), -1.0f, -0.275f, k, 7};
  for (const float split : {-0.925f, -0.78f})  // narrow and wide left halves
  {
    const auto left = OnAnchor(-1.0f, split, 3, bar, k, 7), right = OnAnchor(split, -0.275f, 3, bar, k, 7);
    Check(left.on && right.on && left.a == bar.map.a && left.b == bar.map.b && right.a == bar.map.a &&
              right.b == bar.map.b, "both halves of the glint take the bar's map");
    Check(Near(at(left, split), at(right, split)), "so they meet where the glint is split");
  }
  Check(Near(at(bar.map, -1.0f), -1.0f) && Near(at(bar.map, -0.275f), -0.275f * k), "the bar: edge to inner side");
  Check(!OnAnchor(-0.6f, -0.5f, 4, bar, k, 7).on, "a caption letter (a quad) over the bar keeps the squeeze");
  Check(!OnAnchor(-1.0f, -0.95f, 4, bar, k, 7).on, "a pillar (a quad) inside a panel keeps its own rule");
  Check(!OnAnchor(-0.925f, -0.2f, 3, bar, k, 7).on, "a triangle reaching past the bar is not on it");
  Check(!OnAnchor(-0.925f, -0.275f, 3, RecompOrthoSqueeze::Anchor{}, k, 7).on, "no anchored draw yet: nothing to follow");
  const RecompOrthoSqueeze::Anchor right{EdgeAnchor(0.3f, 1.0f, k), 0.3f, 1.0f, k, 7};
  Check(OnAnchor(0.5f, 1.0f, 3, right, k, 7).on, "the same on the right");
  Check(!OnAnchor(-0.925f, -0.275f, 3, bar, k, 8).on, "a bar from an earlier frame is not followed");
  Check(!OnAnchor(-0.925f, -0.275f, 3, bar, 0.6f, 7).on, "nor one anchored under another screen shape");
  Check(!OnAnchor(-0.925f, -0.275f, 0, bar, k, 7).on, "nor by a line or point batch (passed as 0)");
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
  TestEdgeAnchor();
  TestOnAnchor();
  TestApplyToProjection();
  if (g_failures == 0) std::printf("screen_shape: all checks passed\n");
  return g_failures == 0 ? 0 : 1;
}
