// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Smooth Motion: on a display faster than 60 Hz, every refresh shows the game's
// last frame replayed with its geometry blended toward the frame before
// (RecompSmoothMotion.cpp). These are the parts with no GPU in them: matching a
// frame's draws to the previous frame's, the blend, the per-refresh pacing and
// when the option is offered.
//
// No Dolphin headers: unit-tested on their own (tests/smooth_motion_test.cpp).

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace RecompSmoothMotion
{
// For each draw of `last`, the index of its partner in `before`, or -1. Draws
// are compared by signature (primitive, counts, stride, texture, projection);
// a longest-common-subsequence alignment keeps the order, so one inserted or
// dropped draw does not shift every later match. Measured on fights: 98% of
// draws match, against 86.6% when matched by index.
inline std::vector<int> AlignDraws(const std::vector<std::uint64_t>& last,
                                   const std::vector<std::uint64_t>& before)
{
  const std::size_t n = last.size(), m = before.size();
  std::vector<int> match(n, -1);
  // The table is n*m; past ~16M cells (4000 draws a frame each side) match by
  // position instead of spending 64 MB on it.
  if (n * m > (std::size_t{1} << 24))
  {
    for (std::size_t i = 0; i < std::min(n, m); ++i)
      if (last[i] == before[i])
        match[i] = static_cast<int>(i);
    return match;
  }
  std::vector<std::uint32_t> dp((n + 1) * (m + 1), 0);
  const auto at = [&](std::size_t i, std::size_t j) -> std::uint32_t& { return dp[i * (m + 1) + j]; };
  for (std::size_t i = n; i-- > 0;)
    for (std::size_t j = m; j-- > 0;)
      at(i, j) = last[i] == before[j] ? at(i + 1, j + 1) + 1 : std::max(at(i + 1, j), at(i, j + 1));
  std::size_t i = 0, j = 0;
  while (i < n && j < m)
  {
    if (last[i] == before[j])
      match[i++] = static_cast<int>(j++);
    else if (at(i + 1, j) >= at(i, j + 1))
      ++i;
    else
      ++j;
  }
  return match;
}

// Where a draw's geometry sits on screen: the centre of its positions after
// its position matrix, and the radius around that centre.
struct Place
{
  std::uint64_t sig = 0;
  bool known = false;  // false: positions are not floats, so nothing to compare
  float center[3] = {0.0f, 0.0f, 0.0f};
  float radius = 0.0f;
};

// AlignDraws pairs look-alike draws (one signature) by their order in the
// frame. Hair strands, sparks and splashes are drawn as many look-alike pieces
// that the game re-sorts every frame, and reuses: paired by order, each piece
// blends toward another piece and the in-between pictures fly them across the
// screen. So, per signature that repeats, the order's pairing is replaced by
// nearest-first when that is far closer in total (sorted strands) -- but not
// otherwise: a pair of eyes in a fast head turn can be nearer to each other's
// old places than to their own. Then a pair further apart than kJump times
// the draw's size is a reused piece, not motion, and is unpaired; an unpaired
// draw takes the nearest free look-alike within that distance, or stays
// unpaired (drawn as it is, not blended) -- unless it moved with a draw that
// overlaps it, as an eye does with the face when the camera closes in. `match` is AlignDraws' result,
// edited in place.
constexpr float kJump = 4.0f;

inline void RepairPairs(const std::vector<Place>& last, const std::vector<Place>& before,
                        std::vector<int>& match)
{
  auto distance = [](const Place& a, const Place& b) {
    float d2 = 0.0f;
    for (int c = 0; c < 3; ++c)
      d2 += (a.center[c] - b.center[c]) * (a.center[c] - b.center[c]);
    return std::sqrt(d2);
  };
  auto is_near = [&](const Place& a, const Place& b) {
    return distance(a, b) <= kJump * std::max(a.radius, b.radius);
  };

  // Look-alike groups.
  std::unordered_map<std::uint64_t, std::vector<int>> group_last, group_before;
  for (std::size_t i = 0; i < last.size(); ++i)
  {
    if (last[i].known)
      group_last[last[i].sig].push_back(static_cast<int>(i));
  }
  std::vector<bool> held_elsewhere(before.size(), false);  // by a draw with no known place
  for (std::size_t i = 0; i < match.size(); ++i)
  {
    if (match[i] >= 0 && !last[i].known)
      held_elsewhere[match[i]] = true;
  }
  for (std::size_t j = 0; j < before.size(); ++j)
  {
    if (before[j].known && !held_elsewhere[j])
      group_before[before[j].sig].push_back(static_cast<int>(j));
  }
  constexpr std::size_t kMaxGroup = 256;  // beyond this, the order stands
  for (const auto& [sig, group] : group_last)
  {
    const auto it = group_before.find(sig);
    if (it == group_before.end())
      continue;
    const std::vector<int>& candidates = it->second;
    if ((group.size() < 2 && candidates.size() < 2) || group.size() > kMaxGroup ||
        candidates.size() > kMaxGroup)
    {
      continue;
    }
    float order_cost = 0.0f;
    std::size_t order_pairs = 0;
    for (const int i : group)
    {
      const int m = match[i];
      if (m >= 0 && before[m].known && before[m].sig == sig)
      {
        order_cost += distance(last[i], before[m]);
        ++order_pairs;
      }
    }
    struct Option
    {
      float d;
      int i, j;
    };
    std::vector<Option> options;
    options.reserve(group.size() * candidates.size());
    for (const int i : group)
      for (const int j : candidates)
        options.push_back({distance(last[i], before[j]), i, j});
    std::sort(options.begin(), options.end(),
              [](const Option& a, const Option& b) { return a.d < b.d; });
    std::unordered_map<int, int> nearest;  // last -> before
    std::vector<bool> used(before.size(), false);
    float nearest_cost = 0.0f;
    for (const Option& o : options)
    {
      if (nearest.count(o.i) || used[o.j])
        continue;
      nearest[o.i] = o.j;
      used[o.j] = true;
      nearest_cost += o.d;
    }
    if (nearest.size() >= order_pairs && nearest_cost < 0.5f * order_cost)
    {
      for (const int i : group)
      {
        const auto n = nearest.find(i);
        match[i] = n == nearest.end() ? -1 : n->second;
      }
    }
  }

  // Reused pieces, then free look-alikes nearby. A small part can move many
  // times its size when the camera closes in -- an eye in a face. It moved
  // with what is around it, so it is motion: a far pair stays when a draw that
  // overlaps it (and passed on its own) moved the same way, give or take kJump
  // times the small part's size. A reused spark does not move with the scenery
  // behind it.
  std::vector<bool> taken(before.size(), false);
  std::vector<std::size_t> far_pairs;  // not "far": windows.h defines near and far as macros
  for (std::size_t i = 0; i < match.size(); ++i)
  {
    const int m = match[i];
    if (m < 0)
      continue;
    if (!last[i].known || !before[m].known || is_near(last[i], before[m]))
      taken[m] = true;
    else
      far_pairs.push_back(i);
  }
  for (const std::size_t i : far_pairs)
  {
    const Place& a = last[i];
    const Place& b = before[match[i]];
    bool with_surroundings = false;
    for (std::size_t k = 0; k < match.size() && !with_surroundings; ++k)
    {
      const int mk = match[k];
      if (k == i || mk < 0 || !last[k].known || !before[mk].known || !is_near(last[k], before[mk]))
        continue;
      const Place& ka = last[k];
      const Place& kb = before[mk];
      if (distance(b, kb) > b.radius + kb.radius)
        continue;
      float d2 = 0.0f;
      for (int c = 0; c < 3; ++c)
      {
        const float x = (a.center[c] - b.center[c]) - (ka.center[c] - kb.center[c]);
        d2 += x * x;
      }
      with_surroundings = std::sqrt(d2) <= kJump * std::max(a.radius, b.radius);
    }
    if (with_surroundings)
      taken[match[i]] = true;
    else
      match[i] = -1;
  }
  for (std::size_t i = 0; i < match.size(); ++i)
  {
    if (match[i] >= 0 || !last[i].known)
      continue;
    const auto it = group_before.find(last[i].sig);
    if (it == group_before.end())
      continue;
    int best = -1;
    float best_d = 0.0f;
    for (const int j : it->second)
    {
      if (taken[j] || !is_near(last[i], before[j]))
        continue;
      const float d = distance(last[i], before[j]);
      if (best < 0 || d < best_d)
      {
        best = j;
        best_d = d;
      }
    }
    if (best >= 0)
    {
      match[i] = best;
      taken[best] = true;
    }
  }
}

// Bytes of a vertex that are allowed to change between frames: position,
// normals, and colours (lit on the CPU, they change every frame). Every other
// byte -- the texture coordinates, mostly -- belongs to the vertex.
struct ByteRange
{
  std::uint32_t offset = 0;
  std::uint32_t size = 0;
};

// A mesh's float positions: their centre and the squared radius around it.
struct MeshStats
{
  float center[3] = {0.0f, 0.0f, 0.0f};
  float radius2 = 0.0f;
};

inline void ReadPosition(const std::uint8_t* base, std::uint32_t v, std::uint32_t stride,
                         std::uint32_t pos_offset, int pos_components, float* out)
{
  out[0] = out[1] = out[2] = 0.0f;
  std::memcpy(out, base + static_cast<std::size_t>(v) * stride + pos_offset,
              4 * static_cast<std::size_t>(std::clamp(pos_components, 1, 3)));
}

inline MeshStats StatsOf(const std::uint8_t* base, std::uint32_t count, std::uint32_t stride,
                         std::uint32_t pos_offset, int pos_components)
{
  MeshStats s;
  if (count == 0)
    return s;
  float p[3];
  for (std::uint32_t v = 0; v < count; ++v)
  {
    ReadPosition(base, v, stride, pos_offset, pos_components, p);
    for (int i = 0; i < 3; ++i)
      s.center[i] += p[i];
  }
  for (int i = 0; i < 3; ++i)
    s.center[i] /= static_cast<float>(count);
  for (std::uint32_t v = 0; v < count; ++v)
  {
    ReadPosition(base, v, stride, pos_offset, pos_components, p);
    float d2 = 0.0f;
    for (int i = 0; i < 3; ++i)
      d2 += (p[i] - s.center[i]) * (p[i] - s.center[i]);
    s.radius2 = std::max(s.radius2, d2);
  }
  return s;
}

// Whether vertex N of the last frame blends toward vertex N of the frame
// before. The game re-sorts see-through meshes (hair) every frame: blended by
// index, each vertex moves toward another one and the in-between pictures tear
// the mesh into shards. Strands share their texture coordinates, so the
// vertices cannot be re-paired either; such a mesh is drawn as it is (it still
// follows its matrices). A mesh is re-sorted when any byte outside `moving`
// differs at the same index, or when a vertex moved further than the mesh's
// own size beyond the mesh's motion as a whole -- a small part of a fast
// fighter can move several times its size, but it moves as a whole. The stats
// come from the recorder, which reads the positions anyway: this runs for
// every mesh of every frame on the presenting thread.
inline bool VerticesInOrder(const std::uint8_t* last, const std::uint8_t* before,
                            std::uint32_t count, std::uint32_t stride,
                            const std::vector<ByteRange>& moving, std::uint32_t pos_offset,
                            int pos_components, const MeshStats& s_last, const MeshStats& s_before)
{
  // The runs of bytes that name a vertex.
  std::vector<ByteRange> named;
  for (std::uint32_t b = 0; b < stride; ++b)
  {
    bool moves = false;
    for (const ByteRange& r : moving)
      moves = moves || (b >= r.offset && b < r.offset + r.size);
    if (moves)
      continue;
    if (!named.empty() && named.back().offset + named.back().size == b)
      ++named.back().size;
    else
      named.push_back({b, 1});
  }
  for (std::uint32_t v = 0; v < count; ++v)
  {
    const std::size_t at = static_cast<std::size_t>(v) * stride;
    for (const ByteRange& r : named)
    {
      if (std::memcmp(last + at + r.offset, before + at + r.offset, r.size) != 0)
        return false;
    }
  }

  // The smaller radius: a mix-up stretches the mesh it is measured on.
  const float limit2 = std::min(s_last.radius2, s_before.radius2);
  float shift[3];
  for (int i = 0; i < 3; ++i)
    shift[i] = s_last.center[i] - s_before.center[i];
  for (std::uint32_t v = 0; v < count; ++v)
  {
    float p[3], q[3];
    ReadPosition(last, v, stride, pos_offset, pos_components, p);
    ReadPosition(before, v, stride, pos_offset, pos_components, q);
    float d2 = 0.0f;
    for (int i = 0; i < 3; ++i)
      d2 += (p[i] - q[i] - shift[i]) * (p[i] - q[i] - shift[i]);
    if (d2 > limit2)
      return false;
  }
  return true;
}

// A mesh that is not vertex-blended (VerticesInOrder said no) is drawn with
// the last frame's vertices. Drawn as they are, they sit a frame ahead of the
// blended mesh around them -- re-sorted hair cracks away from the head it is
// on. Moving them back by the part of the centre's motion the blend has not
// reached keeps the mesh with its neighbours; only its own deformation is
// left unblended. alpha 1 is `last`, 0 is `before`.
inline void FollowCentre(std::uint8_t* vertices, std::uint32_t count, std::uint32_t stride,
                         std::uint32_t pos_offset, int pos_components, const MeshStats& s_last,
                         const MeshStats& s_before, float alpha)
{
  const int n = std::clamp(pos_components, 1, 3);
  float by[3];
  for (int i = 0; i < 3; ++i)
    by[i] = (1.0f - alpha) * (s_before.center[i] - s_last.center[i]);
  for (std::uint32_t v = 0; v < count; ++v)
  {
    float p[3];
    std::uint8_t* at = vertices + static_cast<std::size_t>(v) * stride + pos_offset;
    std::memcpy(p, at, 4 * static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
      p[i] += by[i];
    std::memcpy(at, p, 4 * static_cast<std::size_t>(n));
  }
}

// Why VerticesInOrder said no (a diagnostic): the first vertex whose naming
// bytes differ (-1: none), and the worst motion beyond the mesh's own.
struct VerticesInOrderWhy
{
  int byte_vertex = -1;
  int byte_offset = -1;
  float max_dev2 = 0.0f;
  float limit2 = 0.0f;
};

inline VerticesInOrderWhy VerticesInOrderExplain(const std::uint8_t* last, const std::uint8_t* before,
                                                 std::uint32_t count, std::uint32_t stride,
                                                 const std::vector<ByteRange>& moving,
                                                 std::uint32_t pos_offset, int pos_components,
                                                 const MeshStats& s_last, const MeshStats& s_before)
{
  VerticesInOrderWhy why;
  for (std::uint32_t v = 0; v < count && why.byte_vertex < 0; ++v)
  {
    for (std::uint32_t b = 0; b < stride; ++b)
    {
      bool moves = false;
      for (const ByteRange& r : moving)
        moves = moves || (b >= r.offset && b < r.offset + r.size);
      if (!moves && last[v * stride + b] != before[v * stride + b])
      {
        why.byte_vertex = static_cast<int>(v);
        why.byte_offset = static_cast<int>(b);
        break;
      }
    }
  }
  why.limit2 = std::min(s_last.radius2, s_before.radius2);
  for (std::uint32_t v = 0; v < count; ++v)
  {
    float p[3], q[3];
    ReadPosition(last, v, stride, pos_offset, pos_components, p);
    ReadPosition(before, v, stride, pos_offset, pos_components, q);
    float d2 = 0.0f;
    for (int i = 0; i < 3; ++i)
    {
      const float x = p[i] - q[i] - (s_last.center[i] - s_before.center[i]);
      d2 += x * x;
    }
    why.max_dev2 = std::max(why.max_dev2, d2);
  }
  return why;
}

// Below half matched it is a scene cut or a loading screen: blending would
// smear two unrelated pictures, so the real frame is shown instead.
inline bool ShouldBlend(std::size_t matched, std::size_t total)
{
  return total > 0 && matched * 2 >= total;
}

// alpha 1 is `last`, 0 is `before`.
inline void LerpFloats(float* out, const float* last, const float* before, std::size_t n,
                       float alpha)
{
  for (std::size_t i = 0; i < n; ++i)
    out[i] = before[i] + (last[i] - before[i]) * alpha;
}

// One texture matrix (3 rows of 4). A matrix that picks a cell of a sheet of
// animation frames (fire: 1/8 x 1/4 cells) steps between exact fractions, and
// blending it shows the strip between two cells -- or, where it wraps from the
// last cell to the first, sweeps backwards across the whole sheet. Such a
// matrix snaps to the last frame's cell. A matrix that follows the camera
// takes any value and is blended. "Exact" is a multiple of 1/64.
inline void LerpTexMatrix(float* out, const float* last, const float* before, float alpha)
{
  auto on_grid = [](float x) { return std::nearbyint(x * 64.0f) == x * 64.0f; };
  bool changed = false, steps = true;
  for (int i = 0; i < 12; ++i)
  {
    if (last[i] == before[i])
      continue;
    changed = true;
    steps = steps && on_grid(last[i]) && on_grid(before[i]);
  }
  if (changed && steps)
  {
    std::memcpy(out, last, 12 * sizeof(float));
    return;
  }
  LerpFloats(out, last, before, 12, alpha);
}

// When to show what. A frame completed at t_N is shown fully at t_N + period;
// until then each refresh shows the frame before blended toward it, so motion
// is continuous at the cost of up to one game frame of lag (8 ms on average at
// 60 fps). The period is measured, so a game running below full speed paces
// correctly.
class Pacer
{
public:
  void OnFrame(double now_s)
  {
    if (m_have_frame)
    {
      const double gap = now_s - m_last_frame_s;
      // Pauses, stalls and bursts say nothing about the frame rate.
      if (gap >= 1.0 / 240.0 && gap <= 0.1)
        m_period_s += (gap - m_period_s) * 0.1;
    }
    m_have_frame = true;
    m_last_frame_s = now_s;
  }

  // 0..1; 1 when there is no frame yet or the next one is late.
  double Alpha(double now_s) const
  {
    if (!m_have_frame)
      return 1.0;
    return std::clamp((now_s - m_last_frame_s) / m_period_s, 0.0, 1.0);
  }

  double FramePeriod() const { return m_period_s; }

private:
  bool m_have_frame = false;
  double m_last_frame_s = 0.0;
  double m_period_s = 1.0 / 59.94;
};

// At 60 Hz there is nothing between frames to show.
inline bool Offered(bool vulkan, double refresh_hz)
{
  return vulkan && refresh_hz > 61.0;
}

// A display mode's refresh rate from its timings; 0 when they are missing.
inline double RefreshFromMode(std::uint64_t dot_clock, std::uint32_t h_total,
                              std::uint32_t v_total)
{
  if (h_total == 0 || v_total == 0)
    return 0.0;
  return static_cast<double>(dot_clock) /
         (static_cast<double>(h_total) * static_cast<double>(v_total));
}
}  // namespace RecompSmoothMotion
