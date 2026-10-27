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

// A draw spanning the whole screen width -- a fade, a flash, the pause dim, a
// cinema bar -- is meant to cover the screen and is left full width. lo and hi
// are its x extent in unsqueezed clip space, where the screen is -1..1.
// Apply factor k to a projection (4x4, row-major, as VertexShaderManager
// builds it). 2D: x scale and x translation, narrowing it around the centre.
// 3D: only the off-centre x term -- the game's own constants already widen a
// 3D view, but a camera it shifted sideways to sit beside its 2D (the
// character-select model) must move in with that 2D. Centred cameras have no
// such term and are untouched.
inline void ApplyToProjection(float* m, bool orthographic, float k)
{
  if (orthographic)
  {
    m[0] *= k;
    m[3] *= k;
  }
  else
  {
    m[2] *= k;
  }
}

inline bool CoversFullWidth(float lo, float hi)
{
  constexpr float kEdge = 0.98f;
  return lo <= -kEdge && hi >= kEdge;
}
}
