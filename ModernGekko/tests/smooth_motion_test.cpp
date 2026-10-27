// Smooth Motion's pure parts (RecompSmoothMotionMath.h): matching a frame's
// draws against the frame before, the blend, the per-refresh pacing and when
// the option is offered at all. No Dolphin needed.

#include "RecompSmoothMotionMath.h"

#include <array>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

using namespace RecompSmoothMotion;

int g_failures = 0;

void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++g_failures;
  }
}

bool Near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

void TestAlignDraws() {
  Check(AlignDraws({1, 2, 3}, {1, 2, 3}) == std::vector<int>{0, 1, 2}, "identical frames align 1:1");
  Check(AlignDraws({1, 9, 2}, {1, 2}) == std::vector<int>{0, -1, 1}, "an inserted draw is unmatched");
  Check(AlignDraws({}, {1}).empty(), "an empty frame aligns to nothing");
  Check(AlignDraws({5, 6}, {}) == std::vector<int>{-1, -1}, "nothing to match against");
  Check(AlignDraws({1, 2, 3}, {3, 2, 1}).size() == 3, "one entry per draw of the last frame");
}

void TestShouldBlend() {
  Check(ShouldBlend(98, 100), "98% matched blends");
  Check(ShouldBlend(50, 100), "half matched blends");
  Check(!ShouldBlend(49, 100), "a scene cut shows the real frame");
  Check(!ShouldBlend(0, 0), "an empty frame does not blend");
}

void TestLerpFloats() {
  const float last[2] = {8.0f, -4.0f};
  const float before[2] = {0.0f, 4.0f};
  float out[2];
  LerpFloats(out, last, before, 2, 0.0f);
  Check(out[0] == 0.0f && out[1] == 4.0f, "alpha 0 is the frame before");
  LerpFloats(out, last, before, 2, 1.0f);
  Check(out[0] == 8.0f && out[1] == -4.0f, "alpha 1 is the last frame");
  LerpFloats(out, last, before, 2, 0.25f);
  Check(out[0] == 2.0f && out[1] == 2.0f, "alpha 0.25 is a quarter of the way");
}

void TestPacer() {
  Pacer p;
  Check(p.Alpha(0.0) == 1.0, "no frame yet shows the real frame");
  Check(Near(p.FramePeriod(), 1.0 / 59.94, 1e-9), "period starts at NTSC");
  p.OnFrame(1.0);
  Check(p.Alpha(1.0) == 0.0, "a new frame starts at the frame before");
  Check(Near(p.Alpha(1.0 + 1.0 / 119.88), 0.5, 0.01), "half a frame later is halfway");
  Check(p.Alpha(5.0) == 1.0, "a stall holds the real frame");

  Pacer slow;
  double t = 0.0;
  for (int i = 0; i < 100; ++i) {
    slow.OnFrame(t);
    t += 1.0 / 30.0;
  }
  Check(Near(slow.FramePeriod(), 1.0 / 30.0, (1.0 / 30.0) * 0.01), "period follows 30 fps");
  const double before = slow.FramePeriod();
  slow.OnFrame(t + 2.0);  // paused for two seconds
  Check(slow.FramePeriod() == before, "a pause does not change the period");
  Check(slow.Alpha(t + 2.0) == 0.0, "the frame after a pause still resets alpha");
}

void TestOffered() {
  Check(Offered(true, 99.9), "100 Hz on Vulkan is offered");
  Check(!Offered(true, 60.0), "60 Hz is not");
  Check(!Offered(true, 0.0), "an unknown refresh is not");
  Check(!Offered(false, 144.0), "other backends are not");
}

void TestRefreshFromMode() {
  Check(Near(RefreshFromMode(148500000, 2200, 1125), 60.0, 1e-6), "1080p60 mode line");
  Check(RefreshFromMode(1, 0, 1) == 0.0, "a zero total is unknown");
  Check(RefreshFromMode(1, 1, 0) == 0.0, "a zero vertical total is unknown");
}

void TestRepairPairs() {
  // Where a draw sits: signature, centre, radius.
  auto at = [](std::uint64_t sig, float x, float r = 1.0f) {
    Place p;
    p.sig = sig;
    p.known = true;
    p.center[0] = x;
    p.radius = r;
    return p;
  };

  // Three hair strands of one signature, sorted differently in the frame
  // before: order pairs each with another strand, position puts them back.
  std::vector<int> match{0, 1, 2};
  RepairPairs({at(7, 0), at(7, 10), at(7, 20)}, {at(7, 20.2f), at(7, 0.1f), at(7, 10.3f)}, match);
  Check(match == std::vector<int>{1, 2, 0}, "re-sorted look-alike draws pair by position");

  // A recycled particle: its partner is far away and nothing else is near.
  match = {0};
  RepairPairs({at(7, 0)}, {at(7, 366)}, match);
  Check(match == std::vector<int>{-1}, "a draw that jumped more than its size is not blended");

  // Moving less than its own size is ordinary motion and stays paired.
  match = {0};
  RepairPairs({at(7, 0, 5)}, {at(7, 3, 5)}, match);
  Check(match == std::vector<int>{0}, "a draw within its own size stays paired");

  // An eye is small and a fighter is fast: a few times its size per frame is
  // still the same eye. Unpaired, it is drawn unblended and pops out of its
  // socket while the face around it blends.
  match = {0};
  RepairPairs({at(7, 0, 0.015f)}, {at(7, 0.036f, 0.015f)}, match);
  Check(match == std::vector<int>{0}, "a small part moving a few times its size stays paired");

  // Two eyes are look-alikes. Their order is right; nearest-first would swap
  // them (left eye now is nearest to where the right eye was).
  match = {0, 1};
  RepairPairs({at(7, 0, 0.02f), at(7, 0.06f, 0.02f)},
              {at(7, 0.036f, 0.02f), at(7, 0.096f, 0.02f)}, match);
  Check(match == std::vector<int>{0, 1}, "look-alikes keep their order when it is the closer one");

  // The camera closes in on a face: everything moves ~0.1 toward it in one
  // frame, past four times an eye's size. The eyes move with the head around
  // them, so they are motion, not reused pieces.
  match = {0, 1, 2};
  RepairPairs({at(9, 0.1f, 0.26f), at(7, 0.08f, 0.022f), at(7, 0.14f, 0.022f)},
              {at(9, 0.0f, 0.26f), at(7, -0.02f, 0.022f), at(7, 0.04f, 0.022f)}, match);
  Check(match == std::vector<int>{0, 1, 2}, "small parts that move with the mesh around them stay paired");

  // A recycled spark over scenery that stays put is still a jump.
  match = {0, 1};
  RepairPairs({at(9, 0.0f, 5.0f), at(7, 0.5f, 0.02f)}, {at(9, 0.0f, 5.0f), at(7, 3.0f, 0.02f)},
              match);
  Check(match == std::vector<int>{0, -1}, "a jump unlike its surroundings is still unpaired");

  // Only a draw of the same signature can stand in.
  match = {0, -1};
  RepairPairs({at(7, 0), at(8, 50)}, {at(7, 50), at(8, 0)}, match);
  Check(match == std::vector<int>{-1, -1}, "a partner is never taken from another signature");

  // An unmatched before-draw can be picked up.
  match = {-1};
  RepairPairs({at(7, 0)}, {at(7, 0.5f)}, match);
  Check(match == std::vector<int>{0}, "an unmatched draw takes a free look-alike nearby");

  // No position known (integer positions, say): order is all there is.
  Place unknown;
  unknown.sig = 7;
  match = {0};
  RepairPairs({unknown}, {at(7, 999)}, match);
  Check(match == std::vector<int>{0}, "a draw without a known position keeps its order pairing");

  // Never two draws on one partner.
  match = {0, 1};
  RepairPairs({at(7, 0), at(7, 0.2f)}, {at(7, 0.1f), at(7, 400)}, match);
  Check(match == std::vector<int>{0, -1}, "a partner is used once");
}

// A vertex of 20 bytes: position x,y,z then texture u,v (all floats).
std::vector<std::uint8_t> Mesh(const std::vector<std::array<float, 5>>& verts) {
  std::vector<std::uint8_t> out(verts.size() * 20);
  for (std::size_t i = 0; i < verts.size(); ++i)
    std::memcpy(out.data() + i * 20, verts[i].data(), 20);
  return out;
}

void TestVerticesInOrder() {
  const std::vector<ByteRange> moving{{0, 12}};  // the position; u,v name the vertex
  auto run = [&](const std::vector<std::uint8_t>& last, const std::vector<std::uint8_t>& before) {
    const auto n = static_cast<std::uint32_t>(last.size() / 20);
    return VerticesInOrder(last.data(), before.data(), n, 20, moving, 0, 3,
                           StatsOf(last.data(), n, 20, 0, 3), StatsOf(before.data(), n, 20, 0, 3));
  };

  const auto last = Mesh({{{0, 0, 0, 0.1f, 0}}, {{1, 0, 0, 0.2f, 0}}, {{0, 1, 0, 0.3f, 0}}});
  Check(run(last, Mesh({{{0.1f, 0, 0, 0.1f, 0}}, {{1.1f, 0, 0, 0.2f, 0}}, {{0.1f, 1, 0, 0.3f, 0}}})),
        "vertices in the same order blend");

  // Hair: the same vertices, sorted differently in the frame before. Strands
  // share their texture coordinates, so they cannot be told apart to re-pair
  // them: the mesh is not vertex-blended at all.
  Check(!run(last, Mesh({{{0.1f, 1, 0, 0.3f, 0}}, {{0.1f, 0, 0, 0.1f, 0}}, {{1.1f, 0, 0, 0.2f, 0}}})),
        "a re-sorted mesh is not vertex-blended");

  // Re-sorted by a little is re-sorted too: thin strands scramble well within
  // the size of the whole fringe.
  const auto fringe = Mesh({{{0, 0, 0, 0.1f, 0}}, {{0.1f, 0, 0, 0.2f, 0}}, {{0, 10, 0, 0.3f, 0}}});
  Check(!run(fringe, Mesh({{{0.1f, 0, 0, 0.2f, 0}}, {{0, 0, 0, 0.1f, 0}}, {{0, 10, 0, 0.3f, 0}}})),
        "a slightly re-sorted mesh is not vertex-blended");

  Check(!run(last, Mesh({{{0, 0, 0, 0.1f, 0}}, {{1, 0, 0, 0.2f, 0}}, {{0, 50, 0, 0.3f, 0}}})),
        "a vertex that jumped more than the mesh's size means no vertex blending");

  // A small mesh moving fast moves as a whole: that is motion, not a mix-up.
  Check(run(last, Mesh({{{5, 0, 0, 0.1f, 0}}, {{6, 0, 0, 0.2f, 0}}, {{5, 1, 0, 0.3f, 0}}})),
        "a mesh that moved far as a whole still blends");
}

}  // namespace

void TestFollowCentre() {
  // A re-sorted mesh (hair) is not vertex-blended, but it must move with the
  // blended head it sits on: its vertices go back by the part of the centre's
  // motion that the blend has not reached yet.
  auto last = Mesh({{{2, 0, 0, 0.1f, 0}}, {{3, 0, 0, 0.2f, 0}}, {{2, 1, 0, 0.3f, 0}}});
  const auto before = Mesh({{{0, 1, 0, 0.3f, 0}}, {{0, 0, 0, 0.1f, 0}}, {{1, 0, 0, 0.2f, 0}}});
  FollowCentre(last.data(), 3, 20, 0, 3, StatsOf(last.data(), 3, 20, 0, 3),
               StatsOf(before.data(), 3, 20, 0, 3), 0.25f);
  float p[3];
  ReadPosition(last.data(), 1, 20, 0, 3, p);
  Check(Near(p[0], 1.5, 1e-5) && Near(p[1], 0.0, 1e-5), "a re-sorted mesh follows its centre to the blend");
  float u;
  std::memcpy(&u, last.data() + 20 + 12, 4);
  Check(u == 0.2f, "only the position moves");
}

void TestLerpTexMatrix() {
  // A flame drawn from a sheet of 8x4 cells: the matrix picks one cell, and
  // the frame after it the next one -- or wraps from cell 7 back to cell 0.
  // Blended, it would show the strip between two cells (or sweep backwards
  // across the whole row); it snaps to the last frame's cell instead.
  const float sheet_last[12] = {0.125f, 0, 0, 0, 0, 0.25f, 0, 0, 0, 0, 1, 0};
  const float sheet_before[12] = {0.125f, 0, 0.875f, 0, 0, 0.25f, 0.75f, 0, 0, 0, 1, 0};
  float out[12];
  LerpTexMatrix(out, sheet_last, sheet_before, 0.5f);
  Check(out[2] == 0.0f && out[6] == 0.0f, "a matrix stepping between sheet cells is not blended");

  // A matrix that follows the camera (environment maps, scrolling water)
  // takes any value: it still blends.
  const float cam_last[12] = {0.17f, 0, 0.98f, -11.8f, 0.13f, 0.99f, -0.02f, 1.78f, 0, 0, 1, 0};
  const float cam_before[12] = {0.17f, 0, 0.98f, -11.8f, 0.13f, 0.99f, -0.02f, 1.70f, 0, 0, 1, 0};
  LerpTexMatrix(out, cam_last, cam_before, 0.5f);
  Check(Near(out[7], 1.74, 1e-5), "a matrix that moves smoothly still blends");
}

int main() {
  TestAlignDraws();
  TestShouldBlend();
  TestRepairPairs();
  TestVerticesInOrder();
  TestFollowCentre();
  TestLerpFloats();
  TestLerpTexMatrix();
  TestPacer();
  TestOffered();
  TestRefreshFromMode();
  if (g_failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }
  std::puts("smooth_motion_test: all passed");
  return 0;
}
