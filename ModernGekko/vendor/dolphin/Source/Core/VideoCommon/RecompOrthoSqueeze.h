// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>

// Horizontal factor for full-width orthographic (2D) draws, around the centre
// of the screen. 1 leaves them alone. Written once a frame by the CPU thread
// (RecompWidescreen, from the screen shape); read by the video thread when it
// loads a projection (VertexShaderManager). Display only: nothing here reaches
// guest RAM.
namespace RecompOrthoSqueeze
{
inline std::atomic<float> g_factor{1.0f};

// Whether 3D is narrowed with the 2D as well, for a screen whose 3D the game
// laid out with its 2D (RecompWidescreen's face-off screen: each fighter stands
// over its name). Written with g_factor; read the same way.
inline std::atomic<bool> g_3d_follows_2d{false};

// A draw spanning the whole screen width -- a fade, a flash, the pause dim, a
// cinema bar -- is meant to cover the screen and is left full width. lo and hi
// are its x extent in unsqueezed clip space, where the screen is -1..1.
// Apply factor k to a projection (4x4, row-major, as VertexShaderManager
// builds it). 2D: x scale and x translation, narrowing it around the centre.
// 3D: k is 1 unless g_3d_follows_2d, and then x scale and the off-centre term
// narrow with the 2D. Otherwise the game's own constants already widen a 3D
// view -- including the character-select model, whose camera the game shifts
// sideways to sit beside its 2D: it used to move in with the 2D, but the side
// panels it stands in now reach the screen edges (EdgeAnchor), and unmoved it
// stands in their middle.
inline void ApplyToProjection(float* m, bool orthographic, float k)
{
  if (orthographic)
  {
    m[0] *= k;
    m[3] *= k;
  }
  else
  {
    m[0] *= k;
    m[2] *= k;
  }
}

inline constexpr float kEdge = 0.98f;

inline bool CoversFullWidth(float lo, float hi)
{
  return lo <= -kEdge && hi >= kEdge;
}

// The menus' drifting background is tiled from quads exactly 512 or 1024 of the
// 640 units across (1.6 or 3.2 in clip space), most of them part off screen.
// Each tile alone covers neither edge, but together they are the screen's
// background: narrowed, the outer tile's edge drifts across the picture. Nothing
// else on the routes censused has these widths while at rest (a panel sliding in
// passes through them for a frame).
inline bool IsBackgroundTile(float lo, float hi)
{
  constexpr float kTol = 0.0015f;
  const float w = hi - lo;
  return (w > 1.6f - kTol && w < 1.6f + kTol) || (w > 3.2f - kTol && w < 3.2f + kTol);
}

// A 2D draw against one side of the screen but not the other -- the pillars
// and side panels that frame character select -- belongs to the screen, not to
// the 16:9 layout between them, so its outer side goes to that edge instead of
// moving in with the layout. A narrow one (a border) keeps its 16:9 width and
// moves out; a wider one stretches from its inner side, which stays where the
// squeeze puts it, so it still meets the layout. As x' = a * x + b in clip
// space; `on` false for any other draw.
struct EdgeMap
{
  bool on = false;
  float a = 1.0f;
  float b = 0.0f;
};

inline EdgeMap EdgeAnchor(float lo, float hi, float k)
{
  constexpr float kStripWidth = 0.1f;
  // A draw that runs PAST an edge is part of the layout -- a panel the game
  // hangs off the side or slides in (the weapon-name bar with its sockets) --
  // so it keeps the squeeze, and the 2D drawn on it (the lit weapon dots)
  // stays on it. Frames authored to the screen end exactly at +-1.
  constexpr float kOverhang = 0.002f;
  if (lo < -1.0f - kOverhang || hi > 1.0f + kOverhang)
    return {};
  const bool left = lo <= -kEdge && hi < kEdge;
  const bool right = hi >= kEdge && lo > -kEdge;
  if (!left && !right)
    return {};
  const float side = left ? -1.0f : 1.0f;
  if (hi - lo <= kStripWidth)
    return {true, k, side * (1.0f - k)};
  // Fix the outer edge (side) and send the inner one (x_in) to k * x_in.
  const float x_in = left ? hi : lo;
  const float a = (side - k * x_in) / (side - x_in);
  return {true, a, side * (1.0f - a)};
}

// The last edge-anchored draw: its map, its x extent, and the squeeze factor and frame it was
// anchored under -- a map from another frame or another screen shape is nobody's to follow.
struct Anchor
{
  EdgeMap map;
  float lo = 0.0f;
  float hi = 0.0f;
  float k = 0.0f;
  int frame = 0;
};

// A lone triangle lying on an anchored panel is swept over it -- the mode
// menu's highlight glint, one quad drawn as two triangles split where the glint
// is -- so it takes the panel's map. Mapped on its own each half follows a
// different rule (the left one touches the edge) and the halves part. Quads keep
// their own rule: the caption over the bar, the pillars inside character
// select's panels. `on` false for any other draw. `vertices`: 0 for a line or
// point batch (three of those are no triangle); `k`, `frame`: the current ones.
inline EdgeMap OnAnchor(float lo, float hi, unsigned vertices, const Anchor& last, float k, int frame)
{
  constexpr float kTol = 0.0005f;
  if (vertices != 3 || !last.map.on || last.k != k || last.frame != frame || lo < last.lo - kTol ||
      hi > last.hi + kTol)
    return {};
  return last.map;
}
}
