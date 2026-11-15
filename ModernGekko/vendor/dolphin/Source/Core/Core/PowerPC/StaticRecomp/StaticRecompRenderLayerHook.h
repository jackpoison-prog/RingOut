// Copyright 2026 RingOut contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <cstring>

// Scene effects under a wide screen shape: the guest functions the run loop
// hooks to tell the GPU when the game draws one (RecompLayerMarkers.h).
//
// Ultrawide narrows 2D draws to 16:9 so the HUD keeps its shape
// (RecompOrthoSqueeze.h). But the game also draws effects as 2D sprites it
// projected from the 3D scene on the CPU -- Inferno's fire, weapon flashes --
// and those were projected with the already-widened camera. Narrowing them
// pulled them off the bodies they belong to.
//
// The game draws its render list one item at a time through item_pc(item,
// state), in layers: 1 and 2 inside each 3D view (called from the per-view
// drawer, which returns to per_view_return, or from layer_pc(list, layer) with
// layer 1 or 2), and 6 -- its layers 3 to 5, the HUD -- once after every view.
// Menus draw their panels in a per-view layer too, so the layer alone stretched
// character select. What separates them, measured over the arcade route: an
// effect sprite has one texture stage (item+0x2f == 1) and a texture setup of 2
// or more (item+0x13); menu panels have three stages, or setup 1.
//
// Unset is kStaticRecompNoLayerHook, which is not 4-byte aligned and so never
// equals a guest PC. Only the discs with screen shapes have a row: JP has no
// ultrawide (RecompScreenShape.h).
inline constexpr std::uint32_t kStaticRecompNoLayerHook = 0xFFFFFFFFu;

struct StaticRecompRenderLayerHooks
{
  std::uint32_t layer_pc = kStaticRecompNoLayerHook;         // fn(list r3, layer r4)
  std::uint32_t item_pc = kStaticRecompNoLayerHook;          // fn(item r3, state r4)
  std::uint32_t per_view_return = kStaticRecompNoLayerHook;  // LR when the per-view drawer calls item_pc
};

struct StaticRecompRenderLayerHookRow
{
  const char* game_id;
  StaticRecompRenderLayerHooks hooks;
};

// Keep in step with the --gated-pc lists in setup.sh / setup.ps1 (the
// fmv_hooks test checks both): ungated, --direct-calls jumps past the hook. A
// gated PC reaches the dispatcher only while the run loop arms the module's
// gate, under a screen shape wider than 16:9 (StaticRecompCore m_gate_*).
inline constexpr StaticRecompRenderLayerHookRow kStaticRecompRenderLayerHookTable[] = {
    {"GRSEAF", {0x80020348u, 0x8001F804u, 0x8002032Cu}},
    // Plus is a hack of the US text: the same code at the same addresses.
    {"GRSEPS", {0x80020348u, 0x8001F804u, 0x8002032Cu}},
    // PAL: all three sit +0x1D4 from the US ones (the layer function matched
    // byte for byte with branch targets masked; the other two by its calls).
    {"GRSPAF", {0x8002051Cu, 0x8001F9D8u, 0x80020500u}},
};

inline StaticRecompRenderLayerHooks StaticRecompFindRenderLayerHooks(const char* game_id)
{
  if (game_id == nullptr)
    return {};
  for (const auto& row : kStaticRecompRenderLayerHookTable)
  {
    if (std::strcmp(row.game_id, game_id) == 0)
      return row.hooks;
  }
  return {};
}

// Whether item r3, drawn now, is a scene effect (see above). `layer` is the
// last layer_pc call's layer; `from_per_view` whether the per-view drawer called.
inline bool StaticRecompIsSceneEffect(bool from_per_view, std::uint32_t layer,
                                      std::uint8_t texture_stages, std::uint8_t texture_setup)
{
  return (from_per_view || layer != 6) && texture_stages == 1 && texture_setup >= 2;
}
