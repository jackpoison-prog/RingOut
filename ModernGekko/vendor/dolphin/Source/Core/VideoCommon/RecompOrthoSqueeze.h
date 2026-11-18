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

// Horizontal factor for 3D (perspective) draws: RecompScreenShape::
// PerspectiveSqueeze of the shape on screen, 1 otherwise. The game draws its 3D
// for 16:9 at every shape, so without this the display stretches it. Effects
// the game projected from the 3D scene and drew in 2D (the scene-effect mark)
// take it too, so they stay on what they belong to. Written with g_factor.
inline std::atomic<float> g_3d_factor{1.0f};

// A draw spanning the whole screen width -- a fade, a flash, the pause dim, a
// cinema bar -- is meant to cover the screen and is left full width. lo and hi
// are its x extent in unsqueezed clip space, where the screen is -1..1.
// Apply factor k to a projection (4x4, row-major, as VertexShaderManager
// builds it). 2D: x scale and x translation, narrowing it around the centre.
// 3D: x scale by g_3d_factor, around the point the camera looks at. The
// off-centre term stays: the character-select model's camera is shifted to
// stand it in its side panel, and scaled with the rest it moved in toward the
// middle (measured 2026-11-16, 21:9). On the face-off screen, whose fighters are
// laid out with its 2D (g_3d_follows_2d), x scale and off-centre term both take
// the 2D's factor.
inline void ApplyToProjection(float* m, bool orthographic, float k, bool off_centre_too = true)
{
  if (orthographic)
  {
    m[0] *= k;
    m[3] *= k;
  }
  else
  {
    m[0] *= k;
    if (off_centre_too)
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

// Character select stands each chosen fighter in its side panel through an
// off-centre camera, which the shape leaves where it is, while the fighter's
// name under it narrows with the rest of the 2D: at 32:9 the name sat halfway
// to the middle (measured 2026-11-17). So small 2D draws under such a model keep
// their 16:9 place, which is under it, as in the original. The models are those
// drawn with an off-centre perspective (|m2| over 0.2; the fight and the
// face-off use centred cameras); a model's centre on screen is -m2. A frame's
// 2D is matched against the models of the frame before, since either may come
// first. Video thread only.
struct OffCentreModels
{
  static constexpr int kMax = 2;  // one per side in a two-player select
  float seen[kMax] = {};
  int n_seen = 0;
  float last[kMax] = {};
  int n_last = 0;
  // Where each side's model actually stands on screen (index 0 left, 1 right):
  // the mean of its draws' origins as displayed. The game moves the model a
  // little with the shape, so its camera's -m2 is not quite it. Kept from the
  // last frame that drew the model, for the silhouette that comes first.
  float body_sum[2] = {};
  int body_n[2] = {};
  float body[2] = {};
  bool has_body[2] = {};

  void NoteBody(float x)
  {
    const int side = x < 0.0f ? 0 : 1;
    body_sum[side] += x;
    ++body_n[side];
  }

  void Note(float m2)
  {
    if (m2 > -0.2f && m2 < 0.2f)
      return;
    NoteCentre(-m2);
  }

  // A model's centre on screen, from its camera or from its silhouette.
  void NoteCentre(float centre)
  {
    for (int i = 0; i < n_seen; ++i)
    {
      if (seen[i] - centre < 0.05f && centre - seen[i] < 0.05f)
        return;
    }
    if (n_seen < kMax)
      seen[n_seen++] = centre;
  }

  void OnFrame()
  {
    for (int i = 0; i < kMax; ++i)
      last[i] = seen[i];
    n_last = n_seen;
    n_seen = 0;
    for (int side = 0; side < 2; ++side)
    {
      if (body_n[side] > 0)
      {
        body[side] = body_sum[side] / static_cast<float>(body_n[side]);
        has_body[side] = true;
      }
      body_sum[side] = 0.0f;
      body_n[side] = 0;
    }
  }

  // The model on the side of x (left or right of the middle), from last frame,
  // or nothing.
  bool OnSide(float x, float* centre) const
  {
    for (int i = 0; i < n_last; ++i)
    {
      if ((last[i] < 0.0f) == (x < 0.0f))
      {
        *centre = last[i];
        return true;
      }
    }
    return false;
  }

  // The fighter's name, under the model: a 2D draw in the strip along the
  // bottom (its top below -0.6 in clip y; the glyphs span -0.95..-0.78),
  // narrower than a panel, on a model's side of the screen and clear of the
  // portrait grid in the middle (|x| over 0.35) -- including while the name
  // slides in from off the edge. It is narrowed by k like the rest of the 2D,
  // but around the model's centre instead of the screen's, so it ends under the
  // model at its narrowed width. Only the bottom strip: the costume hint along
  // the top and the "Select character." box beside the model are one line of
  // draws each, and moving the part near the model split them (2026-11-17).
  // `on` false for any other draw. lo, hi: x extent; y_top: the top of the y
  // extent, both in unsqueezed clip space.
  EdgeMap Under(float lo, float hi, float y_top, float k) const
  {
    if (hi - lo > 0.6f || y_top > -0.6f)
      return {};
    const float x = 0.5f * (lo + hi);
    float c;
    if ((x > -0.35f && x < 0.35f) || !OnSide(x, &c))
      return {};
    return {true, k, c * (1.0f - k)};
  }
};

// When a fighter is picked, character select shows a flat silhouette of it --
// a full-height 2D image that slides in from off the screen edge, widening as it
// comes (from 0.04 to 0.7 in clip space at 16:9: right side -1.27 to -0.3), and
// stops centred on the model, which then grows out of it. The model keeps its
// place and takes the 3D's width, so the silhouette must too, all the way in: as
// an edge or an overhanging draw it was stretched to the screen edge or pulled
// in to the middle, and stood larger and beside the model at every shape but
// 16:9 (measured 2026-11-17). Not the panel behind it (0.8 wide) nor the pillar
// strip along the edge (from exactly the edge, 0.05 wide).
// The map narrowing it by k3d around the model's centre `c` (negative on the
// left), or `on` false.
inline bool IsSelectSilhouette(float lo, float hi, float y_lo, float y_hi)
{
  constexpr float kTol = 0.002f;
  if (y_lo > -kEdge || y_hi < kEdge)
    return false;
  // Mirror the right side onto the left.
  if (lo > 0.0f)
  {
    const float l = -hi;
    hi = -lo;
    lo = l;
  }
  const float w = hi - lo;
  if (w <= 0.0f || w > 0.71f || hi < -1.32f || hi > -0.29f)
    return false;
  const bool from_edge = lo > -1.0f - kTol && lo < -1.0f + kTol;
  return !(from_edge && w < 0.69f);
}

// The silhouette at rest spans -1..-0.3 at 16:9 (mirrored on the right), so
// its centre is -0.65: narrowed by k3d, that centre goes to `target`, where the
// model stands.
inline constexpr float kSilhouetteRestCentre = 0.65f;

inline EdgeMap SelectSilhouette(float lo, float hi, float y_lo, float y_hi, float target, float k3d)
{
  if (!IsSelectSilhouette(lo, hi, y_lo, y_hi))
    return {};
  const float rest = target < 0.0f ? -kSilhouetteRestCentre : kSilhouetteRestCentre;
  return {true, k3d, target - k3d * rest};
}

// Where character select stands the 1P model at 16:9 (-m2 of its camera); the
// 2P model mirrors it. For the first silhouette, before any model is drawn.
inline constexpr float kSelectModelX = 0.6438f;
}
