// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Smooth Motion: on a display faster than 60 Hz, one picture per refresh.
//
// Every rendered batch of a frame is recorded -- vertices, indices, pipeline,
// bound textures (held, so they stay alive), the three constant blocks and the
// viewport -- together with the EFB clears between them. The last two complete
// frames are kept. Render() draws the last one again into a framebuffer of its
// own (the EFB is never touched; the next frame may already be drawing into
// it), with matrices, projection, lights and float positions/normals moved
// toward the matching batch of the frame before. Display only: nothing here
// feeds back into emulation.
//
// All of it runs on the video thread. The maths is in RecompSmoothMotionMath.h.

#include <array>

#include "Common/BitSet.h"
#include "Common/CommonTypes.h"
#include "Common/MathUtil.h"
#include "VideoCommon/RenderState.h"

namespace Common
{
class BlockingLoop;
}

class AbstractPipeline;
class AbstractTexture;
struct PortableVertexDeclaration;

namespace RecompSmoothMotion
{
// What VertexManagerBase::Flush knows about a batch it just drew.
struct DrawCapture
{
  const AbstractPipeline* pipeline = nullptr;
  PrimitiveType primitive = PrimitiveType::Triangles;
  const u8* vertices = nullptr;
  u32 num_vertices = 0;
  u32 stride = 0;
  const u16* indices = nullptr;
  u32 num_indices = 0;
  const PortableVertexDeclaration* decl = nullptr;
  BitSet32 used_textures;
  const std::array<SamplerState, 8>* samplers = nullptr;
};

// The option is on and offered. Decided once per frame, at EndFrame, so a
// frame is recorded whole or not at all.
bool Recording();

// Called right after RenderDrawCall; reads the shader constants and viewport
// the draw used.
void RecordDraw(const DrawCapture& draw);
void RecordClear(const MathUtil::Rectangle<int>& target_rc, bool color_enable, bool alpha_enable,
                 bool z_enable, u32 color, u32 z);

// The XFB copy ends a frame: recording -> last -> before.
void EndFrame(const MathUtil::Rectangle<int>& xfb_source_rc);

// The last frame blended toward the one before (alpha 1 = the last frame as it
// was, 0 = the frame before), and the rectangle of it the XFB shows. nullptr
// when the real XFB should be shown instead: no pair of frames yet, or a
// scene cut.
const AbstractTexture* Render(float alpha, MathUtil::Rectangle<int>* out_rc);

// Drops both frames and the held textures and pipelines. Called when the
// option goes off and when the backend's config changes (pipelines die).
void Reset();

// Backend shutdown, before the texture cache and the gfx go: drops the frames
// and this module's own framebuffer.
void Shutdown();

// ---- Pacing ---------------------------------------------------------------
// A pacer thread wakes once per display refresh and flags the GPU thread,
// which presents between chunks of FIFO commands (Service). Only with the GPU
// thread: single-core has none, and there frames are presented as usual.

// The option is on and offered right now (the swap chain's present mode).
bool Active();

void StartPacer(Common::BlockingLoop* gpu_loop);
void StopPacer();

// The pacer owns presenting: ViSwap only marks frames complete.
bool Pacing();

// GPU thread, between FIFO chunks: presents if a refresh is due.
void Service();

// RINGOUT_SMOOTH_STATS=1: a histogram of present gaps every 600 presents.
void NotePresent();

// RINGOUT_SMOOTH_INBETWEEN=1: show only in-between pictures at alpha 0.5, the
// on-screen check that what they draw is right.
bool InBetweenOnly();

// Frames ended so far (a diagnostic).
unsigned FramesEnded();
}  // namespace RecompSmoothMotion
