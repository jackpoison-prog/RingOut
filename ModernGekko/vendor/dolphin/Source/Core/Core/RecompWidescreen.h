// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Core/RecompScreenShape.h"

namespace Core
{
class System;
}

// Widescreen through the game's OWN 16:9 mode rather than Dolphin's widescreen
// hack (RingOut#10).
//
// The hack widens the 3D projection and leaves every 2D layer laid out for 4:3,
// so on this game the character-select models come out thin and misplaced
// against stretched menus. The game has had a real 16:9 mode all along --
// Options > Display Settings > Screen Ratio -- which renders anamorphic and lays
// its own screens out for it. All it needs from Dolphin is a forced 16:9 display:
// Aspect "Auto" does NOT detect it.
//
// The game keeps that choice in its save, so no config key can flip it. This
// writes the game's own state instead, on the CPU thread between frames.
namespace RecompWidescreen
{
// True when the running disc's 16:9 mode is mapped here. Other discs keep the
// hack, which is still better than nothing for them.
bool IsNative();

// What the VIDEO row and Alt+W report.
bool IsEnabled();

// Host thread. Flips the preference; OnFrame applies it on the next frame.
// Returns false, changing nothing, when it would have to write guest memory
// during netplay or a replay -- either would desync.
bool Toggle();

// The screen shape the player chose (meaningful only while widescreen is on).
RecompScreenShape::Shape GetShape();

// True when this disc and build have a constant table (RecompScreenShape.h):
// false for unmapped discs, and for a build whose constants did not match.
bool ShapesAvailable();

// Host thread. Steps the shape by direction (+1/-1, wrapping); OnFrame applies
// it on the next frame. Returns false, changing nothing, when shapes are not
// available or guest writes are not allowed (netplay, a replay).
bool CycleShape(int direction);

// CPU thread: the movie player converted a frame. While movies play the
// display shows 16:9 whatever the shape (RecompScreenShape::MovieGate).
void NoteMovieFrame();

// CPU thread, once per emulated frame.
void OnFrame(Core::System& system);
}  // namespace RecompWidescreen
