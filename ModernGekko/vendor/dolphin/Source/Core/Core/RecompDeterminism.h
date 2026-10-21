// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"

struct GCPadStatus;

namespace Core
{
class System;
}

// Per-frame state hashing, used to answer whether the statically recompiled
// core is deterministic -- the question netplay depends on entirely. Two runs
// from the same start with the same inputs must produce identical logs; the
// first frame where they differ is the desync.
//
// Inert unless RINGOUT_DETERMINISM_LOG names a file, so it costs one predicted
// branch per frame in a normal run.
namespace RecompDeterminism
{
// True when RINGOUT_DETERMINISM_LOG is set. The runtime consults this to force
// the emulation into a shape where a per-frame hash means anything -- see
// dolphin_runtime.cpp, which turns off the CPU thread when it is on.
bool IsActive();

// RINGOUT_DETERMINISM_ASPLAYED, with NOHASH: keep the scripted route and the
// frame counter but leave the emulation as a player gets it -- dual-core, and no
// wish for determinism, so the GPU thread runs in Dolphin's default "auto" mode
// with no per-swap wait for it. The deterministic shape serialises the CPU and
// GPU threads at every frame swap; on a 2-core machine, where they compete, that
// understates play. Runs are not reproducible and the route can drift.
bool IsAsPlayed();

void OnFrame(Core::System& system);

// Write watch. A dump says which address diverged; it cannot say what put the
// value there, and an address without a writer is not yet a subsystem. Setting
// RINGOUT_DETERMINISM_WATCH to a guest address makes the StaticRecomp core name
// the recompiled block that stores to it, which the symbol map turns into a
// function.
//
// Requires RINGOUT_DETERMINISM_LOG, like the rest of the harness. Optional:
// RINGOUT_DETERMINISM_WATCH_SIZE (bytes, default 4) and
// RINGOUT_DETERMINISM_WATCH_MAX (reports before it goes quiet, default 20).
bool IsWatchArmed();
u32 WatchGuestAddress();
u32 WatchSize();

// Reported by the core from inside the module's write journal, so `old_value`
// is the pre-image: the store has not committed yet.
void ReportWatchWrite(u32 block_pc, u32 lr, u32 size, u32 old_value, u64 timebase);

// Scripted controller input, keyed on the emulated frame (RINGOUT_DETERMINISM_INPUT).
//
// The harness has always run with no input at all, which makes its determinism
// claim much weaker than it looks: netplay desyncs on gameplay, not on a title
// screen that nothing is driving. Feeding both runs the same inputs is what
// actually tests the thing netplay depends on.
//
// Keyed on the frame, not on wall-clock time, and that distinction is the whole
// point. Writing presses into Dolphin's Pipe input device works for driving the
// game by hand, but which emulated frame a press lands on then depends on host
// timing -- so two runs would receive different input and diverge for a reason
// that says nothing about the core. Frame-keyed playback is reproducible by
// construction.
//
// Returns true when it filled `status`, in which case the host controller is
// ignored entirely -- a stray keypress during a measurement run must not be able
// to perturb it.
//
// Per SI port: a script line may carry a leading "P1"/"P2" selector (default
// P1), and only the ports the script names are driven -- an unscripted port
// still falls through to the host pad. A VS-mode route needs this, because each
// side confirms its own character and health and the stage pick belongs to 2P.
bool ScriptedPad(int device_number, ::GCPadStatus* status);

// Reports that the watched word changed to `value`, seen from `site`. The
// journal covers guest stores; this covers everyone else -- DMA, and anything
// the chassis writes into guest RAM natively -- by noticing the change after
// the fact. `site` is where the poll ran, which is the actual finding: a change
// first seen after CoreTiming::Advance came from a scheduled hardware event,
// while one seen after a dispatch came from that block's MMIO side effects.
// Callers filter on their own cached copy first, so this is off the hot path.
void PollWatch(const char* site, u32 pc, u32 value);
}
