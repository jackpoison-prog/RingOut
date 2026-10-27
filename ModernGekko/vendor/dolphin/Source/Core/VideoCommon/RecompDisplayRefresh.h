// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// The refresh rate of the monitor the game window is on, for Smooth Motion.
// X11 (XRandR) and Win32 ask the OS; anything else reports 0 (unknown), and
// GFX_SMOOTH_MOTION_REFRESH_HZ overrides whatever the OS says.

struct WindowSystemInfo;

namespace RecompDisplayRefresh
{
// Remembers the window. Called by the Vulkan backend at initialize.
void SetWindow(const WindowSystemInfo& wsi);

// Asks the OS again for the window set above; returns the new value. Any thread.
double Refresh();

// The ini override if set, else the last Refresh() result. Any thread.
double Current();

// Vulkan is running and Current() is above 61 Hz.
bool SmoothMotionOffered();
}  // namespace RecompDisplayRefresh
