// Copyright 2026 RingOut contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// The Steam Deck's own controls, read directly (RingOut#26).
//
// Launched through Steam, the Deck's pad reaches SDL only as "Steam Virtual
// Gamepad", and that device can read centred and unpressed however it is bound
// (measured on the Deck 2026-09-01 and reported again in 2.0: launched from its
// folder the game worked, launched from Steam it saw no input in either mode, with
// Steam Input on or off). Dolphin also has a backend that reads the Deck's
// built-in controls straight from the hardware, "SteamDeck/0/Steam Deck", and it
// is listed in every launch context. So on a Deck a pad bound to the Deck's own
// controller is moved onto that device, with its bindings renamed from the SDL
// names the default profile uses to that backend's names.
//
// No Dolphin headers: unit-tested on its own (tests/deck_native_pad_test.cpp).

#include <array>
#include <string>
#include <string_view>
#include <utility>

namespace RingOutDeckPad
{
// SDL gamepad name -> SteamDeck backend name (Dolphin's SDLGamepad.h and
// SteamDeck.cpp). Stick Y: both read + for up -- Dolphin's SDL backend inverts
// SDL's axis, and the Deck's report is + for up (SDL negates it).
inline constexpr std::array<std::pair<std::string_view, std::string_view>, 26> kNames = {{
    {"Button S", "A"},
    {"Button E", "B"},
    {"Button W", "X"},
    {"Button N", "Y"},
    {"Shoulder L", "L1"},
    {"Shoulder R", "R1"},
    {"Trigger L", "L2"},
    {"Trigger R", "R2"},
    {"Start", "Menu"},
    {"Back", "View"},
    {"Guide", "Steam"},
    {"Thumb L", "L3"},
    {"Thumb R", "R3"},
    {"Pad N", "D-Pad Up"},
    {"Pad S", "D-Pad Down"},
    {"Pad W", "D-Pad Left"},
    {"Pad E", "D-Pad Right"},
    {"Left X-", "Left Stick X-"},
    {"Left X+", "Left Stick X+"},
    {"Left Y-", "Left Stick Y-"},
    {"Left Y+", "Left Stick Y+"},
    {"Right X-", "Right Stick X-"},
    {"Right X+", "Right Stick X+"},
    {"Right Y-", "Right Stick Y-"},
    {"Right Y+", "Right Stick Y+"},
    {"Misc 1", "Quick Access"},
}};

// Whether a device string ("SOURCE/ID/NAME") is the Deck's own pad as SDL sees
// it, in or out of Steam.
inline bool IsDeckSdlPad(std::string_view device)
{
  if (!device.starts_with("SDL/"))
    return false;
  return device.ends_with("/Steam Deck Controller") || device.ends_with("/Steam Virtual Gamepad");
}

inline bool IsDeckNative(std::string_view device)
{
  return device.starts_with("SteamDeck/");
}

// A binding expression with every `quoted` SDL control renamed. A name with no
// SteamDeck twin is left as it is (it binds to nothing, as it would have).
inline std::string Translate(std::string_view expr)
{
  std::string out;
  out.reserve(expr.size() + 16);
  std::size_t i = 0;
  while (i < expr.size())
  {
    if (expr[i] != '`')
    {
      out += expr[i++];
      continue;
    }
    const std::size_t end = expr.find('`', i + 1);
    if (end == std::string_view::npos)
    {
      out += expr.substr(i);
      break;
    }
    const std::string_view name = expr.substr(i + 1, end - i - 1);
    std::string_view mapped = name;
    for (const auto& [sdl, deck] : kNames)
    {
      if (name == sdl)
      {
        mapped = deck;
        break;
      }
    }
    out += '`';
    out += mapped;
    out += '`';
    i = end + 1;
  }
  return out;
}
}  // namespace RingOutDeckPad
