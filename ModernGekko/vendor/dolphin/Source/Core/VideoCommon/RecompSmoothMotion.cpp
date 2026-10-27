// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/RecompSmoothMotion.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <thread>
#include <vector>

#include "Common/BlockingLoop.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/System.h"
#include "VideoCommon/AbstractFramebuffer.h"
#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/AbstractTexture.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/ConstantManager.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/GeometryShaderManager.h"
#include "VideoCommon/NativeVertexFormat.h"
#include "VideoCommon/PixelShaderManager.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/RecompDisplayRefresh.h"
#include "VideoCommon/RecompSmoothMotionMath.h"
#include "VideoCommon/TextureCacheBase.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/VertexShaderManager.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/XFMemory.h"

namespace RecompSmoothMotion
{
namespace
{
struct Draw
{
  u64 sig = 0;
  u32 tex0 = 0;  // diagnostics only
  bool blend = false;
  const AbstractPipeline* pipeline = nullptr;  // nullptr: recorded for alignment, not replayed
  std::vector<u8> vertices;
  std::vector<u16> indices;
  u32 stride = 0;
  u32 num_vertices = 0;
  AttributeFormat position{};
  AttributeFormat normal{};
  Place place;  // where it sits, for RepairPairs
  std::vector<ByteRange> moving;  // position, normals, colours: VerticesInOrder
  MeshStats stats;                 // of the float positions, for VerticesInOrder
  VertexShaderConstants vs{};
  PixelShaderConstants ps{};
  GeometryShaderConstants gs{};
  std::array<RcTcacheEntry, 8> textures{};
  std::array<SamplerState, 8> samplers{};
  BitSet32 used;
  AbstractGfx::ViewportAndScissor viewport{};
};

struct Clear
{
  MathUtil::Rectangle<int> rc;
  bool color_enable = false, alpha_enable = false, z_enable = false;
  u32 color = 0, z = 0;
};

struct Event
{
  bool is_clear = false;
  Clear clear{};
  Draw draw;
};

struct Frame
{
  std::vector<Event> events;
  MathUtil::Rectangle<int> xfb_rc;
};

struct State
{
  Frame recording;
  Frame last;    // the newest complete frame
  Frame before;  // the one before it
  // For each draw of `last`, in order: the index into before.events of its
  // partner, or -1. Empty when the pair is not worth blending.
  std::vector<int> partner;
  // Per draw of `last`, like partner: whether its vertices blend toward the
  // partner's (VerticesInOrder).
  std::vector<u8> vertex_blend;
  bool have_pair = false;

  std::unique_ptr<AbstractTexture> color, depth, resolved;
  std::unique_ptr<AbstractFramebuffer> framebuffer;
};

State s_state;
bool s_recording = false;

// Pacing. s_pacer is touched by the GPU thread only (ViSwap and Service both
// run there); the pacer thread only raises s_due.
Pacer s_pacer;
// Presents in the current stats window that showed a blended picture.
int s_blended_in_window = 0;
// The slowest EndFrame steps in the window (s): align, repair, vertex maps.
double s_cost[5] = {0.0, 0.0, 0.0, 0.0, 0.0};  // + render, present: summed (s)
int s_cost_n[5] = {0, 0, 0, 0, 0};
// Frames ended (diagnostics: names the pair a dumped picture came from).
unsigned s_frames_ended = 0;
std::thread s_pacer_thread;
std::atomic<bool> s_pacer_run{false};
std::atomic<bool> s_due{false};

double NowSeconds()
{
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void PacerMain(Common::BlockingLoop* gpu_loop)
{
  using Clock = std::chrono::steady_clock;
  auto next = Clock::now();
  auto requery = next + std::chrono::seconds(2);
  while (s_pacer_run.load(std::memory_order_relaxed))
  {
    const double hz = RecompDisplayRefresh::Current();
    if (!Active())
    {
      next = Clock::now() + std::chrono::milliseconds(100);
    }
    else
    {
      // Absolute deadlines, so the rate does not drift with wake-up latency;
      // a late wake (or a long pause) restarts the schedule rather than
      // firing a burst to catch up.
      next += std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / hz));
      if (next < Clock::now())
        next = Clock::now();
    }
    std::this_thread::sleep_until(next);
    if (Active())
    {
      s_due.store(true, std::memory_order_release);
      gpu_loop->Wakeup();
    }
    // The window may have moved to a monitor with another refresh.
    if (Clock::now() >= requery)
    {
      RecompDisplayRefresh::Refresh();
      requery = Clock::now() + std::chrono::seconds(2);
    }
  }
}

u64 Signature(PrimitiveType primitive, u32 num_vertices, u32 num_indices, u32 stride)
{
  u64 h = 1469598103934665603ull;
  for (const u64 x : {static_cast<u64>(primitive), static_cast<u64>(num_vertices),
                      static_cast<u64>(num_indices), static_cast<u64>(stride),
                      static_cast<u64>(bpmem.tex.GetUnit(0).texImage3.image_base),
                      static_cast<u64>(xfmem.projection.type)})
  {
    h = (h ^ x) * 1099511628211ull;
  }
  return h;
}

template <typename T>
void LerpBlock(T& out, const T& last, const T& before, float alpha)
{
  static_assert(sizeof(T) % sizeof(float) == 0);
  LerpFloats(reinterpret_cast<float*>(&out), reinterpret_cast<const float*>(&last),
             reinterpret_cast<const float*>(&before), sizeof(T) / sizeof(float), alpha);
}

void LerpAttribute(std::vector<u8>& out, const std::vector<u8>& last, const std::vector<u8>& before,
                   u32 count, u32 stride, const AttributeFormat& a, float alpha)
{
  if (!a.enable || a.integer || a.type != ComponentFormat::Float)
    return;
  for (u32 v = 0; v < count; ++v)
  {
    const u32 at = v * stride + a.offset;
    LerpFloats(reinterpret_cast<float*>(out.data() + at),
               reinterpret_cast<const float*>(last.data() + at),
               reinterpret_cast<const float*>(before.data() + at), a.components, alpha);
  }
}

bool EnsureFramebuffer(State& st)
{
  const u32 width = g_framebuffer_manager->GetEFBWidth();
  const u32 height = g_framebuffer_manager->GetEFBHeight();
  if (st.color && st.color->GetWidth() == width && st.color->GetHeight() == height &&
      st.color->GetSamples() == g_framebuffer_manager->GetEFBSamples() && st.framebuffer)
  {
    return true;
  }
  st.framebuffer.reset();
  st.resolved.reset();
  st.color = g_gfx->CreateTexture(FramebufferManager::GetEFBColorTextureConfig(width, height),
                                  "Smooth Motion color");
  st.depth = g_gfx->CreateTexture(FramebufferManager::GetEFBDepthTextureConfig(width, height),
                                  "Smooth Motion depth");
  if (!st.color || !st.depth)
    return false;
  if (st.color->GetSamples() > 1)
  {
    st.resolved = g_gfx->CreateTexture(
        TextureConfig(width, height, 1, st.color->GetLayers(), 1, st.color->GetFormat(),
                      AbstractTextureFlag_RenderTarget, AbstractTextureType::Texture_2DArray),
        "Smooth Motion resolved");
    if (!st.resolved)
      return false;
  }
  st.framebuffer = g_gfx->CreateFramebuffer(st.color.get(), st.depth.get());
  return st.framebuffer != nullptr;
}
}  // namespace

// VertexManagerBase's friend: the replay needs its protected upload calls.
class Replayer
{
public:
  static void Clear(VertexManagerBase& vm, const MathUtil::Rectangle<int>& rc, bool color_enable,
                    bool alpha_enable, bool z_enable, u32 color, u32 z)
  {
    // The same uniforms AbstractGfx::ClearRegion uses.
    struct
    {
      float clear_color[4];
      float clear_depth;
      float padding1, padding2, padding3;
    } uniforms = {{static_cast<float>((color >> 16) & 0xFF) / 255.0f,
                   static_cast<float>((color >> 8) & 0xFF) / 255.0f,
                   static_cast<float>((color >> 0) & 0xFF) / 255.0f,
                   static_cast<float>((color >> 24) & 0xFF) / 255.0f},
                  static_cast<float>(z & 0xFFFFFF) / 16777216.0f};
    if (!g_backend_info.bSupportsReversedDepthRange)
      uniforms.clear_depth = 1.0f - uniforms.clear_depth;
    vm.UploadUtilityUniforms(&uniforms, sizeof(uniforms));
    g_gfx->SetPipeline(g_framebuffer_manager->GetClearPipeline(color_enable, alpha_enable, z_enable));
    g_gfx->SetViewportAndScissor(rc);
    g_gfx->Draw(0, 3);
  }

  static void DrawOne(VertexManagerBase& vm, const Draw& d, const std::vector<u8>& vertices,
                      const VertexShaderConstants& vs)
  {
    auto& system = Core::System::GetInstance();
    auto& vsm = system.GetVertexShaderManager();
    auto& psm = system.GetPixelShaderManager();
    auto& gsm = system.GetGeometryShaderManager();
    u32 base_vertex, base_index;
    vm.UploadUtilityVertices(vertices.data(), d.stride, d.num_vertices, d.indices.data(),
                             static_cast<u32>(d.indices.size()), &base_vertex, &base_index);
    vsm.constants = vs;
    psm.constants = d.ps;
    gsm.constants = d.gs;
    vsm.dirty = psm.dirty = gsm.dirty = true;
    vm.UploadUniforms();
    g_gfx->SetPipeline(d.pipeline);
    for (u32 i = 0; i < 8; ++i)
    {
      if (d.used[i] && d.textures[i])
      {
        g_gfx->SetTexture(i, d.textures[i]->texture.get());
        g_gfx->SetSamplerState(i, d.samplers[i]);
      }
    }
    const auto& v = d.viewport;
    g_gfx->SetViewport(v.viewport_x, v.viewport_y, v.viewport_width, v.viewport_height,
                       v.viewport_near_depth, v.viewport_far_depth);
    g_gfx->SetScissorRect(v.scissor_rect);
    g_gfx->DrawIndexed(base_index, static_cast<u32>(d.indices.size()), base_vertex);
  }

  static void Finish() { VertexManagerBase::InvalidateConstants(); }
};

bool Recording()
{
  return s_recording;
}

// Float positions through the draw's position matrix. Per-vertex matrices
// (posmtx) would need each vertex's own matrix: left unknown.
static Place PlaceOf(const Draw& d, bool per_vertex_matrix)
{
  Place place;
  place.sig = d.sig;
  const AttributeFormat& a = d.position;
  if (per_vertex_matrix || !a.enable || a.integer || a.type != ComponentFormat::Float ||
      a.components < 2 || d.num_vertices == 0)
  {
    return place;
  }
  const auto& m = d.vs.posnormalmatrix;
  std::vector<std::array<float, 3>> points(d.num_vertices);
  for (u32 v = 0; v < d.num_vertices; ++v)
  {
    float in[3] = {0.0f, 0.0f, 0.0f};
    std::memcpy(in, d.vertices.data() + v * d.stride + a.offset, 4 * std::min(a.components, 3));
    for (int r = 0; r < 3; ++r)
      points[v][r] = m[r][0] * in[0] + m[r][1] * in[1] + m[r][2] * in[2] + m[r][3];
  }
  for (const auto& pt : points)
    for (int c = 0; c < 3; ++c)
      place.center[c] += pt[c] / static_cast<float>(d.num_vertices);
  float r2 = 0.0f;
  for (const auto& pt : points)
  {
    float d2 = 0.0f;
    for (int c = 0; c < 3; ++c)
      d2 += (pt[c] - place.center[c]) * (pt[c] - place.center[c]);
    r2 = std::max(r2, d2);
  }
  place.radius = std::sqrt(r2);
  place.known = std::isfinite(place.radius);
  return place;
}

void RecordDraw(const DrawCapture& c)
{
  if (!s_recording)
    return;
  Draw& d = s_state.recording.events.emplace_back().draw;
  d.sig = Signature(c.primitive, c.num_vertices, c.num_indices, c.stride);
  d.tex0 = bpmem.tex.GetUnit(0).texImage3.image_base << 5;
  d.blend = bpmem.blendmode.blend_enable;
  d.pipeline = c.pipeline;
  // Lines and points expanded in the vertex shader need a shifted base vertex
  // the replay does not give them: kept for the alignment only.
  if (g_ActiveConfig.UseVSForLinePointExpand() &&
      (c.primitive == PrimitiveType::Points || c.primitive == PrimitiveType::Lines))
  {
    d.pipeline = nullptr;
  }
  d.stride = c.stride;
  d.num_vertices = c.num_vertices;
  d.vertices.assign(c.vertices, c.vertices + static_cast<size_t>(c.num_vertices) * c.stride);
  d.indices.assign(c.indices, c.indices + c.num_indices);
  d.position = c.decl->position;
  d.normal = c.decl->normals[0];
  d.used = c.used_textures;
  for (u32 i = 0; i < 8; ++i)
  {
    d.textures[i] = g_texture_cache->GetBoundEntry(i);
    d.samplers[i] = (*c.samplers)[i];
  }
  auto& system = Core::System::GetInstance();
  d.vs = system.GetVertexShaderManager().constants;
  d.ps = system.GetPixelShaderManager().constants;
  d.gs = system.GetGeometryShaderManager().constants;
  d.viewport = g_gfx->GetStoredViewportAndScissor();
  d.place = PlaceOf(d, c.decl->posmtx.enable);
  // Colours too: lit on the CPU, they change every frame.
  auto moves = [&](const AttributeFormat& a) {
    if (a.enable)
    {
      d.moving.push_back({static_cast<u32>(a.offset),
                          static_cast<u32>(a.components) * GetElementSize(a.type)});
    }
  };
  moves(c.decl->position);
  const AttributeFormat& pa = c.decl->position;
  if (pa.enable && !pa.integer && pa.type == ComponentFormat::Float)
  {
    d.stats = StatsOf(d.vertices.data(), d.num_vertices, d.stride, static_cast<u32>(pa.offset),
                      pa.components);
  }
  for (const AttributeFormat& n : c.decl->normals)
    moves(n);
  for (const AttributeFormat& col : c.decl->colors)
    moves(col);
}

void RecordClear(const MathUtil::Rectangle<int>& target_rc, bool color_enable, bool alpha_enable,
                 bool z_enable, u32 color, u32 z)
{
  if (!s_recording)
    return;
  Event& e = s_state.recording.events.emplace_back();
  e.is_clear = true;
  e.clear = {target_rc, color_enable, alpha_enable, z_enable, color, z};
}

void EndFrame(const MathUtil::Rectangle<int>& xfb_source_rc)
{
  const bool want = Config::Get(Config::GFX_SMOOTH_MOTION) &&
                    RecompDisplayRefresh::SmoothMotionOffered();
  if (!want)
  {
    if (s_recording)
      Reset();
    s_recording = false;
    return;
  }
  if (!s_recording)
  {
    // Start recording with the next frame; this one was not recorded.
    s_recording = true;
    return;
  }

  State& st = s_state;
  st.recording.xfb_rc = g_framebuffer_manager->ConvertEFBRectangle(xfb_source_rc);
  st.before = std::move(st.last);
  st.last = std::move(st.recording);
  st.recording = {};

  const double t_align = NowSeconds();
  std::vector<u64> sig_last, sig_before;
  std::vector<Place> place_last, place_before;
  std::vector<int> at_before;
  for (const Event& e : st.last.events)
  {
    if (!e.is_clear)
    {
      sig_last.push_back(e.draw.sig);
      place_last.push_back(e.draw.place);
    }
  }
  for (size_t i = 0; i < st.before.events.size(); ++i)
  {
    if (!st.before.events[i].is_clear)
    {
      sig_before.push_back(st.before.events[i].draw.sig);
      place_before.push_back(st.before.events[i].draw.place);
      at_before.push_back(static_cast<int>(i));
    }
  }
  std::vector<int> match = AlignDraws(sig_last, sig_before);
  // A scene cut is judged on the order alone: look-alikes that RepairPairs
  // unpairs are effects that moved, not a different scene.
  const size_t aligned = static_cast<size_t>(
      std::count_if(match.begin(), match.end(), [](int m) { return m >= 0; }));
  const double t_repair = NowSeconds();
  if (const char* v = std::getenv("RINGOUT_SMOOTH_FRAMELOG");
      v && static_cast<long>(s_frames_ended) == std::atol(v))
  {
    for (size_t i = 0; i < match.size(); ++i)
    {
      const Place& a = place_last[i];
      if (match[i] < 0 || !a.known)
        continue;
      const Place& b = place_before[match[i]];
      float d2 = 0.0f;
      for (int c = 0; c < 3; ++c)
        d2 += (a.center[c] - b.center[c]) * (a.center[c] - b.center[c]);
      std::fprintf(stderr,
                   "[smooth] pl #%zu c %.3f %.3f %.3f <- %.3f %.3f %.3f r %.4f/%.4f moved %.4f\n", i,
                   a.center[0], a.center[1], a.center[2], b.center[0], b.center[1], b.center[2],
                   a.radius, b.radius, std::sqrt(d2));
    }
  }
  RepairPairs(place_last, place_before, match);
  const double t_repaired = NowSeconds();
  size_t matched = 0;
  for (int& m : match)
  {
    if (m >= 0)
    {
      m = at_before[m];
      ++matched;
    }
  }
  st.have_pair = !st.before.events.empty() && ShouldBlend(aligned, match.size());
  // RINGOUT_SMOOTH_DRAWLOG=1: every 120 frames, which draws found no partner.
  static const bool drawlog = std::getenv("RINGOUT_SMOOTH_DRAWLOG") != nullptr;
  static int frame_no = 0;
  if (drawlog && ++frame_no % 120 == 0)
  {
    std::fprintf(stderr, "[smooth] frame %d: %zu/%zu draws matched\n", frame_no, matched,
                 match.size());
    size_t k = 0;
    for (const Event& e : st.last.events)
    {
      if (e.is_clear)
        continue;
      if (match[k] < 0)
      {
        std::fprintf(stderr, "[smooth]   unmatched #%zu verts %u idx %zu stride %u tex0 %06x blend %d\n",
                     k, e.draw.num_vertices, e.draw.indices.size(), e.draw.stride, e.draw.tex0,
                     e.draw.blend ? 1 : 0);
      }
      else if (std::getenv("RINGOUT_SMOOTH_TEXLOG"))
      {
        const Draw& p = st.before.events[match[k]].draw;
        float dt = 0.0f, dl = 0.0f;
        for (size_t r = 0; r < e.draw.vs.texmatrices.size(); ++r)
          for (int c = 0; c < 4; ++c)
          {
            const float x = std::fabs(e.draw.vs.texmatrices[r][c] - p.vs.texmatrices[r][c]);
            (c == 3 ? dt : dl) = std::max(c == 3 ? dt : dl, x);
          }
        if (dt > 0.0f || dl > 0.0f)
          std::fprintf(stderr, "[smooth]   texmtx #%zu verts %u tex0 %06x blend %d dtrans %.4f dlin %.4f\n",
                       k, e.draw.num_vertices, e.draw.tex0, e.draw.blend ? 1 : 0, dt, dl);
        if (dl > 0.05f || dt > 0.05f)
          for (size_t r = 0; r < 6 && r < e.draw.vs.texmatrices.size(); ++r)
            std::fprintf(stderr, "[smooth]     row %zu  %.6f %.6f %.6f %.6f  <-  %.6f %.6f %.6f %.6f\n", r,
                         e.draw.vs.texmatrices[r][0], e.draw.vs.texmatrices[r][1],
                         e.draw.vs.texmatrices[r][2], e.draw.vs.texmatrices[r][3],
                         p.vs.texmatrices[r][0], p.vs.texmatrices[r][1], p.vs.texmatrices[r][2],
                         p.vs.texmatrices[r][3]);
      }
      ++k;
    }
  }
  st.partner = st.have_pair ? std::move(match) : std::vector<int>{};
  const double t_map = NowSeconds();
  st.vertex_blend.assign(st.partner.size(), 0);
  size_t k = 0;
  for (const Event& e : st.last.events)
  {
    if (e.is_clear || st.partner.empty())
      continue;
    const Draw& d = e.draw;
    const int m = st.partner[k];
    u8& blend = st.vertex_blend[k++];
    if (m < 0)
    {
      if (const char* v = std::getenv("RINGOUT_SMOOTH_FRAMELOG");
          v && static_cast<long>(s_frames_ended) == std::atol(v))
      {
        std::fprintf(stderr, "[smooth] fl #%zu UNMATCHED verts %u stride %u tex0 %06x blend %d "
                     "sig %016llx c %.3f %.3f %.3f\n",
                     k - 1, d.num_vertices, d.stride, d.tex0, d.blend ? 1 : 0,
                     static_cast<unsigned long long>(d.sig), d.stats.center[0], d.stats.center[1],
                     d.stats.center[2]);
        for (size_t b = 0; b < st.before.events.size(); ++b)
        {
          const Draw& q = st.before.events[b].draw;
          if (!st.before.events[b].is_clear && q.num_vertices == d.num_vertices)
            std::fprintf(stderr, "[smooth]    before ev%zu verts %u tex0 %06x sig %016llx\n", b,
                         q.num_vertices, q.tex0, static_cast<unsigned long long>(q.sig));
        }
      }
      continue;
    }
    const Draw& p = st.before.events[m].draw;
    const AttributeFormat& a = d.position;
    // RINGOUT_SMOOTH_FRAMELOG=<n>: every draw of frame n and its partner.
    static const long framelog = [] {
      const char* v = std::getenv("RINGOUT_SMOOTH_FRAMELOG");
      return v ? std::atol(v) : -1L;
    }();
    if (static_cast<long>(s_frames_ended) == framelog)
    {
      std::fprintf(stderr,
                   "[smooth] fl #%zu<-ev%d verts %u/%u stride %u tex0 %06x/%06x pos %d/%d/%d "
                   "blend %d c %.3f %.3f %.3f <- %.3f %.3f %.3f r %.4f\n",
                   k - 1, m, d.num_vertices, p.num_vertices, d.stride, d.tex0, p.tex0,
                   a.enable ? 1 : 0, a.integer ? 1 : 0, static_cast<int>(a.type), d.blend ? 1 : 0,
                   d.stats.center[0], d.stats.center[1], d.stats.center[2], p.stats.center[0],
                   p.stats.center[1], p.stats.center[2], std::sqrt(d.stats.radius2));
    }
    if (p.vertices.size() != d.vertices.size() || p.stride != d.stride || !a.enable ||
        a.integer || a.type != ComponentFormat::Float)
    {
      continue;
    }
    blend = VerticesInOrder(d.vertices.data(), p.vertices.data(), d.num_vertices, d.stride,
                            d.moving, static_cast<u32>(a.offset), a.components, d.stats, p.stats);
    // RINGOUT_SMOOTH_BLENDLOG=1: every matched mesh that is not vertex-blended.
    static const bool blendlog = std::getenv("RINGOUT_SMOOTH_BLENDLOG") != nullptr;
    if (blendlog && !blend)
    {
      const VerticesInOrderWhy why =
          VerticesInOrderExplain(d.vertices.data(), p.vertices.data(), d.num_vertices, d.stride,
                                 d.moving, static_cast<u32>(a.offset), a.components, d.stats,
                                 p.stats);
      std::fprintf(stderr,
                   "[smooth] f%llu nb #%zu verts %u tex0 %06x blend %d bytes@v%d+%d dev2 %.5f "
                   "lim2 %.5f r2 %.5f/%.5f\n",
                   static_cast<unsigned long long>(s_frames_ended), k - 1, d.num_vertices, d.tex0,
                   d.blend ? 1 : 0, why.byte_vertex, why.byte_offset, why.max_dev2, why.limit2,
                   d.stats.radius2, p.stats.radius2);
    }
  }
  // The blend clock starts where the frames rotate. Starting it at ViSwap
  // instead (the VI retrace, often most of a frame later) stepped the picture
  // backwards once a frame.
  const double t_end = NowSeconds();
  s_cost[0] += t_repair - t_align;
  ++s_cost_n[0];
  s_cost[1] += t_repaired - t_repair;
  ++s_cost_n[1];
  s_cost[2] += t_end - t_map;
  ++s_cost_n[2];
  s_pacer.OnFrame(NowSeconds());
  ++s_frames_ended;
}

const AbstractTexture* Render(float alpha, MathUtil::Rectangle<int>* out_rc)
{
  State& st = s_state;
  if (!s_recording || !st.have_pair || !g_vertex_manager)
    return nullptr;
  if (!EnsureFramebuffer(st))
    return nullptr;
  // At 1 this is the last frame as recorded. It is still the replay, not the
  // real XFB, so the picture does not change look when the blend completes.
  alpha = std::clamp(alpha, 0.0f, 1.0f);
  const double t_render = NowSeconds();

  auto& system = Core::System::GetInstance();
  auto& vsm = system.GetVertexShaderManager();
  auto& psm = system.GetPixelShaderManager();
  auto& gsm = system.GetGeometryShaderManager();
  const VertexShaderConstants saved_vs = vsm.constants;
  const PixelShaderConstants saved_ps = psm.constants;
  const GeometryShaderConstants saved_gs = gsm.constants;
  const std::span<u8> saved_custom = psm.custom_constants;
  psm.custom_constants = {};

  VertexManagerBase& vm = *g_vertex_manager;
  g_gfx->SetFramebuffer(st.framebuffer.get());
  Replayer::Clear(vm, st.framebuffer->GetRect(), true, true, true, 0xFF000000, 0xFFFFFF);

  std::vector<u8> blended;
  size_t draw_no = 0;
  for (const Event& e : st.last.events)
  {
    if (e.is_clear)
    {
      Replayer::Clear(vm, e.clear.rc, e.clear.color_enable, e.clear.alpha_enable,
                      e.clear.z_enable, e.clear.color, e.clear.z);
      continue;
    }
    const Draw& d = e.draw;
    const int partner = st.partner[draw_no++];
    if (!d.pipeline)
      continue;
    if (partner < 0)
    {
      Replayer::DrawOne(vm, d, d.vertices, d.vs);
      continue;
    }

    const Draw& p = st.before.events[partner].draw;
    VertexShaderConstants vs = d.vs;
    LerpBlock(vs.posnormalmatrix, d.vs.posnormalmatrix, p.vs.posnormalmatrix, alpha);
    LerpBlock(vs.projection, d.vs.projection, p.vs.projection, alpha);
    static_assert(sizeof(vs.texmatrices) == 8 * 12 * sizeof(float));
    for (int m = 0; m < 8; ++m)
    {
      LerpTexMatrix(&vs.texmatrices[3 * m][0], &d.vs.texmatrices[3 * m][0],
                    &p.vs.texmatrices[3 * m][0], alpha);
    }
    LerpBlock(vs.transformmatrices, d.vs.transformmatrices, p.vs.transformmatrices, alpha);
    LerpBlock(vs.normalmatrices, d.vs.normalmatrices, p.vs.normalmatrices, alpha);
    LerpBlock(vs.posttransformmatrices, d.vs.posttransformmatrices, p.vs.posttransformmatrices,
              alpha);
    for (size_t l = 0; l < vs.lights.size(); ++l)
    {
      LerpBlock(vs.lights[l].pos, d.vs.lights[l].pos, p.vs.lights[l].pos, alpha);
      LerpBlock(vs.lights[l].dir, d.vs.lights[l].dir, p.vs.lights[l].dir, alpha);
    }
    // CPU-skinned characters change their vertices every frame.
    if (st.vertex_blend[draw_no - 1])
    {
      blended = d.vertices;
      LerpAttribute(blended, d.vertices, p.vertices, d.num_vertices, d.stride, d.position, alpha);
      LerpAttribute(blended, d.vertices, p.vertices, d.num_vertices, d.stride, d.normal, alpha);
      Replayer::DrawOne(vm, d, blended, vs);
    }
    else if (p.vertices.size() == d.vertices.size() && p.stride == d.stride &&
             d.position.enable && !d.position.integer &&
             d.position.type == ComponentFormat::Float)
    {
      blended = d.vertices;
      FollowCentre(blended.data(), d.num_vertices, d.stride,
                   static_cast<u32>(d.position.offset), d.position.components, d.stats, p.stats,
                   alpha);
      Replayer::DrawOne(vm, d, blended, vs);
    }
    else
    {
      Replayer::DrawOne(vm, d, d.vertices, vs);
    }
  }

  vsm.constants = saved_vs;
  psm.constants = saved_ps;
  gsm.constants = saved_gs;
  psm.custom_constants = saved_custom;
  Replayer::Finish();
  g_gfx->SetFramebuffer(g_framebuffer_manager->GetEFBFramebuffer());
  s_cost[3] += NowSeconds() - t_render;
  ++s_cost_n[3];

  *out_rc = st.last.xfb_rc;
  ++s_blended_in_window;
  if (st.resolved)
  {
    st.resolved->ResolveFromTexture(st.color.get(), st.framebuffer->GetRect(), 0, 0);
    st.resolved->FinishedRendering();
    return st.resolved.get();
  }
  st.color->FinishedRendering();
  return st.color.get();
}

void Reset()
{
  s_state.recording = {};
  s_state.last = {};
  s_state.before = {};
  s_state.partner.clear();
  s_state.vertex_blend.clear();
  s_state.have_pair = false;
}

void Shutdown()
{
  Reset();
  s_recording = false;
  s_state.framebuffer.reset();
  s_state.resolved.reset();
  s_state.depth.reset();
  s_state.color.reset();
}

bool Active()
{
  return Config::Get(Config::GFX_SMOOTH_MOTION) && RecompDisplayRefresh::SmoothMotionOffered();
}

void StartPacer(Common::BlockingLoop* gpu_loop)
{
  if (s_pacer_thread.joinable())
    return;
  s_pacer = {};
  s_due.store(false);
  s_pacer_run.store(true);
  s_pacer_thread = std::thread(PacerMain, gpu_loop);
}

void StopPacer()
{
  s_pacer_run.store(false);
  if (s_pacer_thread.joinable())
    s_pacer_thread.join();
  s_due.store(false);
}

bool Pacing()
{
  return s_pacer_run.load(std::memory_order_relaxed) && s_recording && Active();
}


void Service()
{
  // GPU thread only.
  static double due_since = 0.0;
  if (!s_due.load(std::memory_order_acquire))
    return;
  // A present flushes the open batch, and the two halves of a split batch no
  // longer match the previous frame's -- they would be drawn unblended, a
  // piece of a model a frame ahead. Wait for the batch to close, up to 4 ms.
  if (g_vertex_manager && g_vertex_manager->HasSendableVertices())
  {
    const double now = NowSeconds();
    if (due_since == 0.0)
      due_since = now;
    if (now - due_since < 0.004)
      return;
  }
  due_since = 0.0;
  s_due.store(false, std::memory_order_relaxed);
  if (!Pacing() || !g_presenter)
    return;
  // RINGOUT_SMOOTH_ALPHA=<0..1> pins the blend (a diagnostic).
  static const float forced = [] {
    const char* v = std::getenv("RINGOUT_SMOOTH_ALPHA");
    return v ? static_cast<float>(std::atof(v)) : -1.0f;
  }();
  const float alpha = forced >= 0.0f ? forced :
                      InBetweenOnly()  ? 0.5f :
                                         static_cast<float>(s_pacer.Alpha(NowSeconds()));
  const double t_present = NowSeconds();
  g_presenter->PresentSmooth(alpha);
  s_cost[4] += NowSeconds() - t_present;
  ++s_cost_n[4];
}

void NotePresent()
{
  static const bool on = [] {
    const char* v = std::getenv("RINGOUT_SMOOTH_STATS");
    return v && v[0] == '1';
  }();
  if (!on)
    return;
  static std::chrono::steady_clock::time_point last{};
  static std::array<int, 6> bins{};  // <7, 7-9, 9-11, 11-13, 13-18, >=18 ms
  static int n = 0;
  const auto now = std::chrono::steady_clock::now();
  if (last != std::chrono::steady_clock::time_point{})
  {
    const double ms = std::chrono::duration<double, std::milli>(now - last).count();
    bins[ms < 7 ? 0 : ms < 9 ? 1 : ms < 11 ? 2 : ms < 13 ? 3 : ms < 18 ? 4 : 5]++;
    if (++n == 600)
    {
      auto C = [](int i) { return s_cost_n[i] ? s_cost[i] * 1000.0 / s_cost_n[i] : 0.0; };
      std::fprintf(stderr,
                   "[smooth] present gaps (ms) <7:%d 7-9:%d 9-11:%d 11-13:%d 13-18:%d >=18:%d "
                   "period %.2f ms blended %d; mean align %.2f repair %.2f maps %.2f render %.2f "
                   "present %.2f ms\n",
                   bins[0], bins[1], bins[2], bins[3], bins[4], bins[5],
                   s_pacer.FramePeriod() * 1000.0, s_blended_in_window, C(0), C(1), C(2), C(3), C(4));
      s_blended_in_window = 0;
      for (int i = 0; i < 5; ++i)
        s_cost[i] = 0.0, s_cost_n[i] = 0;
      bins = {};
      n = 0;
    }
  }
  last = now;
}

unsigned FramesEnded()
{
  return s_frames_ended;
}

bool InBetweenOnly()
{
  static const bool on = [] {
    const char* v = std::getenv("RINGOUT_SMOOTH_INBETWEEN");
    return v && v[0] == '1';
  }();
  return on;
}
}  // namespace RecompSmoothMotion
