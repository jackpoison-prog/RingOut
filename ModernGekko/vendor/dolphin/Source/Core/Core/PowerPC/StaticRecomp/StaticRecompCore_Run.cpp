// RecompCore: StaticRecomp CPU core - Main execution loop.
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompCore.h"
#include "Core/System.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/PowerPC/Interpreter/Interpreter.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompLockstep.h"
#include "Core/CoreTiming.h"
#include "Core/HW/CPU.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Core/NetPlay/NetPlayProto.h"
#include "Core/RecompDeterminism.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/ConfigManager.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/SystemTimers.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
// MSVC has no <unistd.h>; the POSIX names it does provide are underscored.
// The mode must be "rb" here: the pipe carries raw ARGB frames, and Windows
// text mode would expand 0x0A to 0x0D 0x0A and corrupt the video.
#define RINGOUT_POPEN _popen
#define RINGOUT_PCLOSE _pclose
#define RINGOUT_POPEN_MODE "rb"
#else
#include <unistd.h>
#define RINGOUT_POPEN popen
#define RINGOUT_PCLOSE pclose
// NOT "rb". POSIX defines the mode as "r" or "w", and glibc validates it
// strictly -- popen("...", "rb") returns NULL, which silently killed FMV
// playback (pipe=FAILED) while audio, which this hook does not touch, kept
// playing. There is no text/binary distinction to worry about here anyway.
#define RINGOUT_POPEN_MODE "r"
#endif
#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

namespace
{
constexpr u32 SYNC_EXCEPTION_MASK = ~static_cast<u32>(
    EXCEPTION_EXTERNAL_INT | EXCEPTION_DECREMENTER | EXCEPTION_PERFORMANCE_MONITOR);

// Resolve movie <fno> to an ffmpeg input inside the game's own movie.afs.
//
// The loose movie<n>.sfd files this player was written against are *verbatim
// byte ranges* of that archive, which every package already ships in
// game/files/ -- so there is nothing to extract and nothing extra to
// distribute. AFS is four fields deep: "AFS\0", a little-endian entry count,
// then (offset, size) pairs. ffmpeg's own subfile protocol takes the range, so
// the decode is byte-identical to decoding an extracted file (verified: same
// md5 over the first frames of movie2).
//
// Returns false if no archive holds this entry; out is then untouched.
bool FindMovieInAfs(int fno, std::string* out)
{
  std::string archive;
  if (const char* env = std::getenv("STATICRECOMP_FMV_AFS"))
  {
    archive = env;
  }
  else
  {
    // Both shipped packages put userdata/ and game/ side by side, so the user
    // path locates the archive without the runtime having to plumb the game
    // root down into the CPU core.
    const std::string candidates[] = {
        File::GetUserPath(D_USER_IDX) + "../game/files/movie.afs",  // shipped layout
        "game/files/movie.afs",                                     // beside the working dir
        "dist/RingOut-1.0-deck/game/files/movie.afs",               // development tree
    };
    for (const std::string& c : candidates)
    {
      if (File::Exists(c))
      {
        archive = c;
        break;
      }
    }
  }
  if (archive.empty())
    return false;

  File::IOFile f(archive, "rb");
  if (!f)
    return false;
  u8 magic[4] = {};
  u32 count = 0;
  if (!f.ReadBytes(magic, sizeof(magic)) || std::memcmp(magic, "AFS\0", 4) != 0)
    return false;
  if (!f.ReadArray(&count, 1) || fno < 0 || static_cast<u32>(fno) >= count)
    return false;
  u32 toc[2] = {};  // offset, size
  if (!f.Seek(8 + static_cast<u64>(fno) * 8, File::SeekOrigin::Begin) ||
      !f.ReadArray(toc, 2) || toc[1] == 0)
    return false;

  // end is exclusive, matching the protocol's own documented usage.
  char url[1024];
  std::snprintf(url, sizeof(url), "subfile,,start,%llu,end,%llu,,:%s",
                static_cast<unsigned long long>(toc[0]),
                static_cast<unsigned long long>(toc[0]) + toc[1], archive.c_str());
  *out = url;
  return true;
}

// --- FMV HLE native player -------------------------------------------------
// Decodes a Sofdec movie (from the game's movie.afs, or an extracted
// <dir>/movie<fno>.sfd) with ffmpeg into raw ARGB8888 frames on a reader
// thread; the CnvFrm hook takes the frame the game's own decoder is on (the
// frame number in its descriptor), so the picture keeps the game's pacing.
//
// "One frame per call" is NOT that pacing. The game converts once per frame it
// DISPLAYS, and on a 50 Hz PAL disc that is 20 per second: it keeps the movie
// in step with its audio by skipping frames of the 29.97 fps stream, so a
// player that hands out the next decoded frame per call ran PAL movies at two
// thirds speed, the picture falling ever further behind the sound (measured
// 2026-09-27: 20.0 calls/s, frame numbers stepping 0x129, 0x12B, 0x12C, 0x12E).
// ffmpeg -pix_fmt argb emits bytes A,R,G,B == GameCube big-endian ARGB8888,
// so frames copy straight into the guest destination buffer.
class FmvPlayer
{
public:
  // s_fmv is a global, and only the NEXT movie's Start() ever joined the
  // previous reader thread -- the last movie's thread stayed joinable, so
  // static destruction of the std::thread member hit std::terminate ("without
  // an active exception") and dumped core on every exit that played an FMV.
  ~FmvPlayer() { Stop(); }

  void Start(int fno)
  {
    Stop();
    m_fno = fno;
    m_open = false;
    m_paced_started = false;
  }

  // Lazily opens the pipe once the frame dimensions are known (from CnvFrm).
  void EnsureOpen(u32 w, u32 h)
  {
    if (m_open || m_fno < 0)
      return;
    m_w = w;
    m_h = h;
    m_frame_bytes = static_cast<size_t>(w) * h * 4u;
    // Movie lookup. STATICRECOMP_FMV_DIR wins; otherwise try the known layouts
    // and pick the first that actually holds this movie. A single hardcoded
    // default is what broke this before: it used to be the original author's
    // absolute home path (leaked into every shipped binary), and replacing that
    // with <user dir>/fmv alone silently regressed the dev layout -- ffmpeg
    // failed to open the file, so video went black while audio, which this hook
    // does not touch, kept playing.
    std::string base;
    if (const char* dir = std::getenv("STATICRECOMP_FMV_DIR"))
    {
      base = dir;
    }
    else
    {
      const std::string candidates[] = {
          File::GetUserPath(D_USER_IDX) + "fmv",  // shipped layout
          "fmv",                                  // beside the working dir
          "work/fmv",                             // development tree
      };
      for (const std::string& c : candidates)
      {
        if (File::Exists(c + "/movie" + std::to_string(m_fno) + ".sfd"))
        {
          base = c;
          break;
        }
      }
    }

    // No loose file: read the movie in place out of the game's movie.afs. This
    // is the normal path -- extracted .sfd files are a development convenience,
    // and expecting them is why the takeover silently never engaged in any
    // shipped package or profiling run.
    std::string input;
    if (!base.empty())
    {
      input = base + "/movie" + std::to_string(m_fno) + ".sfd";
    }
    else if (!FindMovieInAfs(m_fno, &input))
    {
      ERROR_LOG_FMT(POWERPC,
                    "FMV takeover: movie{} found neither as a loose .sfd nor in a "
                    "movie.afs; set STATICRECOMP_FMV_DIR (folder of .sfd files) or "
                    "STATICRECOMP_FMV_AFS (path to movie.afs)",
                    m_fno);
      m_fno = -1;   // give the movie back to the game's own decoder
      return;
    }

    char cmd[1280];
    // cmd.exe does not treat '...' as quoting -- it would pass the quotes
    // through as part of the path -- so the argument is double-quoted there.
#ifdef _WIN32
    std::snprintf(cmd, sizeof(cmd), "ffmpeg -v error -i \"%s\" -f rawvideo -pix_fmt argb -",
                  input.c_str());
#else
    std::snprintf(cmd, sizeof(cmd), "ffmpeg -v error -i '%s' -f rawvideo -pix_fmt argb -",
                  input.c_str());
#endif
    m_pipe = RINGOUT_POPEN(cmd, RINGOUT_POPEN_MODE);
    m_stop = false;
    m_open = true;
    if (m_pipe)
      m_reader = std::thread([this] { Reader(); });
    // Print the resolved input, not just the movie number: "which file did it
    // actually open" is the one thing that was missing every time this silently
    // fell back to the game's own decoder.
    std::fprintf(stderr, "[fmv-hle] player: movie%d %ux%u pipe=%s src=%s\n", m_fno, w, h,
                 m_pipe ? "ok" : "FAILED", input.c_str());
  }

  // Copy the next decoded frame into out (m_frame_bytes). Reuses the last frame
  // if the decoder hasn't produced one yet; false only before the first frame.
  // Copy decoded frame <index> (0-based, the game's own frame number) into out,
  // dropping any earlier frames still queued. When the decoder has not reached
  // it yet, the newest decoded frame is used; false only before the first one.
  bool FrameAt(u32 index, std::vector<u8>& out)
  {
    std::lock_guard<std::mutex> l(m_mtx);
    while (!m_queue.empty() && (m_index < 0 || static_cast<s64>(index) > m_index))
    {
      m_last.swap(m_queue.front());
      m_queue.pop_front();
      ++m_index;
    }
    if (m_last.empty())
      return false;
    out = m_last;
    return true;
  }

  bool Next(std::vector<u8>& out)
  {
    std::lock_guard<std::mutex> l(m_mtx);
    if (!m_queue.empty())
    {
      out.swap(m_queue.front());
      m_queue.pop_front();
      m_last = out;
      ++m_index;
      return true;
    }
    if (!m_last.empty())
    {
      out = m_last;
      return true;
    }
    return false;
  }

  void Stop()
  {
    m_stop = true;
    if (m_reader.joinable())
      m_reader.join();
    if (m_pipe)
    {
      RINGOUT_PCLOSE(m_pipe);
      m_pipe = nullptr;
    }
    m_queue.clear();
    m_last.clear();
    m_index = -1;
    m_open = false;
    m_fno = -1;
  }

  size_t FrameBytes() const { return m_frame_bytes; }
  bool Active() const { return m_fno >= 0; }
  int Fno() const { return m_fno; }

  // Pace the takeover to the movie's real frame rate (29.97fps): a new frame is
  // "ready" only once its display interval has elapsed and a decoded frame is
  // buffered. Prevents the game from advancing as fast as it can (which would
  // play the movie too fast and waste CPU re-tiling every VI).
  bool ReadyForNextFrame()
  {
    std::lock_guard<std::mutex> l(m_mtx);
    if (m_queue.empty() && m_last.empty())
      return false;  // nothing decoded yet
    const auto now = std::chrono::steady_clock::now();
    if (!m_paced_started)
    {
      m_paced_started = true;
      m_next_due = now;
    }
    if (now < m_next_due)
      return false;
    m_next_due += std::chrono::microseconds(33367);  // 1/29.97s
    if (m_next_due < now)  // fell behind: resync
      m_next_due = now + std::chrono::microseconds(33367);
    return true;
  }

private:
  void Reader()
  {
    std::vector<u8> buf(m_frame_bytes);
    while (!m_stop)
    {
      size_t got = 0;
      while (got < m_frame_bytes && !m_stop)
      {
        size_t n = std::fread(buf.data() + got, 1, m_frame_bytes - got, m_pipe);
        if (n == 0)
          return;  // EOF / error
        got += n;
      }
      if (got < m_frame_bytes)
        return;
      {
        std::lock_guard<std::mutex> l(m_mtx);
        m_queue.push_back(buf);
      }
      // Backpressure: keep ~8 frames buffered so we don't outrun the game.
      while (!m_stop)
      {
        {
          std::lock_guard<std::mutex> l(m_mtx);
          if (m_queue.size() < 8)
            break;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(2000));
      }
    }
  }

  FILE* m_pipe = nullptr;
  std::thread m_reader;
  std::mutex m_mtx;
  std::deque<std::vector<u8>> m_queue;
  std::vector<u8> m_last;
  s64 m_index = -1;  // frame number of m_last; -1 before the first
  std::atomic<bool> m_stop{false};
  int m_fno = -1;
  u32 m_w = 0, m_h = 0;
  size_t m_frame_bytes = 0;
  bool m_open = false;
  bool m_paced_started = false;
  std::chrono::steady_clock::time_point m_next_due;
};

FmvPlayer s_fmv;

// Coarse PC histogram: 16KB buckets across the module code range, dumped every
// N samples so we can see which function dominates during movie playback.
void FmvHistSample(u32 pc)
{
  // Full-range 1KB buckets, windowed: reset each dump so we see the CURRENT
  // hot spot (menu/paused steady-state), not cumulative with the intro movie.
  static std::unordered_map<u32, u64> hist;
  static u64 n = 0;
  hist[pc >> 10] += 1;
  if (++n % 40000000ull != 0)
    return;
  std::vector<std::pair<u32, u64>> v(hist.begin(), hist.end());
  std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
  std::fprintf(stderr, "[hist] window of %llu samples, top:\n",
               static_cast<unsigned long long>(n));
  for (size_t i = 0; i < v.size() && i < 10; ++i)
    std::fprintf(stderr, "  %08X : %.1f%%\n", v[i].first << 10, 100.0 * v[i].second / n);
  hist.clear();
  n = 0;
}
}  // namespace

// FMV HLE step 1 (observe): confirm the hook fires and that r5 is the movie
// number in movie.afs. Later steps will drive native ffmpeg playback here.
// Dump the CRI mwPly handle across consecutive exec-server calls; the field(s)
// that increment or toggle each call are the per-frame playback state.
void StaticRecompCore::OnFmvExecObserve(u32 handle)
{
  static int count = 0;
  if (count++ >= 10)
    return;
  std::fprintf(stderr, "[fmv-hle] exec#%d handle=0x%08X:", count, handle);
  for (u32 i = 0; i < 40; ++i)
    std::fprintf(stderr, " %08X", GuestRead32(handle + i * 4u));
  std::fprintf(stderr, "\n");
}

// Whether the native player may take a movie over at all.
//
// It must not when guest RAM has to match another run byte for byte. The frames
// it writes come from a host ffmpeg process, and Next() reuses the previous
// frame whenever the decoder has not produced one yet, so the pixels that land
// in guest RAM depend on host decoder progress and differ between two machines
// -- or between two runs on one machine. Under STATICRECOMP_FMV_TAKEOVER it is
// worse than cosmetic: ReadyForNextFrame() gates guest control flow on the wall
// clock, so emulated timing itself diverges.
//
// This is not hypothetical for netplay. The intro movie starts automatically a
// few seconds after boot, long before anyone reaches a match, so an ungated
// takeover desyncs every session immediately and for a reason that has nothing
// to do with the netcode. Measurement says nothing is lost by gating it: the
// HLE is a net CPU loss on every path (see .github/scripts/fmv-ab.sh).
static bool FmvTakeoverAllowed()
{
  if (NetPlay::IsNetPlayRunning())
    return false;
  if (RecompDeterminism::IsActive())
    return false;
  // Movie playback/recording compares against a recorded input stream and has
  // the same requirement.
  return true;
}

void StaticRecompCore::OnFmvStartAfs(u32 fno, u32 patid, u32 handle)
{
  if (!FmvTakeoverAllowed())
  {
    static bool logged = false;
    if (!logged)
    {
      logged = true;
      std::fprintf(stderr, "[fmv-hle] takeover disabled: guest RAM must stay reproducible "
                           "(netplay or determinism run); the game decodes its own movies\n");
    }
    // Stop rather than merely return. "Never armed -> Active() stays false" only
    // holds in a fresh process, and a netplay session started from the in-game
    // menu is NOT one: the runner rebuilds the session in place, so s_fmv is a
    // global carrying m_fno from whatever the player watched before joining.
    // EnsureOpen only checks m_fno, so that stale value reopened the pipe and
    // fed ffmpeg output back into guest RAM -- on whichever peer happened to
    // have seen a movie first. Two peers, one injecting and one not, desynced
    // at the frame the first conversion ran (frame 2220, reproducibly).
    s_fmv.Stop();   // resets m_fno to -1, so Active() really is false
    return;
  }
  std::fprintf(stderr, "[fmv-hle] mwPlyStartAfs: movie fno=%u patid=%u handle=0x%08X\n",
               fno, patid, handle);
  s_fmv.Start(static_cast<int>(fno));
}

// Decode-skip takeover: fill the frame descriptor the game's getfrm normally
// populates from the (now-skipped) MPEG decode ring, so the display path
// (gated on desc[0]!=0) proceeds to CnvFrm — which we've replaced with native
// frames. desc layout learned from the live dump: [0]=frame ptr, [2]=width,
// [3]=height, [4]=w/16, [5]=h/16, [6]=frame counter.
void StaticRecompCore::OnFmvGetFrm(u32 handle, u32 desc)
{
  const int fno = s_fmv.Fno();
  const u32 w = 640u;
  const u32 h = (fno == 0) ? 368u : 448u;
  static u32 ctr = 0;
  GuestWrite32(desc + 0u, 0x80300000u);  // nonzero frame ptr (pixels come via CnvFrm)
  GuestWrite32(desc + 4u, 3u);
  GuestWrite32(desc + 8u, w);
  GuestWrite32(desc + 12u, h);
  GuestWrite32(desc + 16u, w / 16u);
  GuestWrite32(desc + 20u, h / 16u);
  GuestWrite32(desc + 24u, ++ctr);
  GuestWrite32(desc + 28u, 0x7512u);
}

// Read a big-endian u32 from guest MEM1 (0x80000000 based); 0 if out of range.
u32 StaticRecompCore::GuestRead32(u32 addr) const
{
  const u32 off = addr - 0x80000000u;
  if (m_guest.ram == nullptr || off + 4u > m_guest.ram_size)
    return 0;
  const u8* p = m_guest.ram + off;
  return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | u32(p[3]);
}

// Write a big-endian u32 to guest MEM1 (0x80000000 based); no-op if oob.
void StaticRecompCore::GuestWrite32(u32 addr, u32 value)
{
  const u32 off = addr - 0x80000000u;
  if (m_guest.ram == nullptr || off + 4u > m_guest.ram_size)
    return;
  u8* p = m_guest.ram + off;
  p[0] = u8(value >> 24); p[1] = u8(value >> 16);
  p[2] = u8(value >> 8);  p[3] = u8(value);
}

// FMV HLE step 2b (visible injection test): fill the ARGB8888 destination
// buffer (dst = r5, dims from the descriptor) with a bright test pattern so we
// can confirm this buffer is what the game blits to screen. Replaces the guest
// YUV->ARGB conversion. dst is double-buffered; dims are 640x368 for movie0.
void StaticRecompCore::OnFmvCnvFrm(u32 handle, u32 desc, u32 dst)
{
  // This hook WRITES GUEST RAM, so it answers to the takeover gate directly
  // rather than trusting the player's armed state. Relying on m_fno alone is
  // what let a value left over from a previous session in the same process
  // reopen the pipe under netplay; the cost of getting that wrong is a desync,
  // so it is checked here as well as where the takeover is refused.
  if (!FmvTakeoverAllowed())
    return;

  const u32 w = GuestRead32(desc + 8u);   // desc[2] = width
  const u32 h = GuestRead32(desc + 12u);  // desc[3] = height
  if (w == 0 || h == 0 || w > 1024 || h > 1024)
    return;
  s_fmv.EnsureOpen(w, h);

  const size_t bytes = static_cast<size_t>(w) * h * 4u;
  const u32 off = dst - 0x80000000u;
  if (m_guest.ram == nullptr || off + bytes > m_guest.ram_size)
    return;

  // STRIDE DIAGNOSTIC: vertical grayscale gradient (row y -> brightness). A
  // correct linear buffer shows a smooth top->bottom gradient; a stride
  // mismatch shows it slanted, and a 4x4-block scramble means GX tiling.
  if (std::getenv("STATICRECOMP_FMV_DIAG"))
  {
    // Vertical white lines every 64px on dark blue. Straight => stride==w;
    // slanted => stride mismatch (drift per row measures the true stride).
    for (u32 y = 0; y < h; ++y)
      for (u32 x = 0; x < w; ++x)
      {
        const u32 argb = ((x % 64u) < 2u) ? 0xFFFFFFFFu : 0xFF000040u;
        GuestWrite32(dst + (y * w + x) * 4u, argb);
      }
    return;
  }

  // desc[8] is the frame number the game's decoder is displaying; it skips
  // frames to hold audio sync (every 3rd of 3 on a 50 Hz disc), and the picture
  // has to skip with it.
  const u32 frame_no = GuestRead32(desc + 0x20u);
  static std::vector<u8> frame;
  if (s_fmv.FrameAt(frame_no, frame) && frame.size() == bytes && (w % 4u) == 0 && (h % 4u) == 0)
  {
    // The destination is a GX RGBA8 texture: 4x4 tiles, 64 bytes each, storing
    // an AR byte-plane (32 bytes) then a GB byte-plane (32 bytes), texels in
    // row-major order within the tile. ffmpeg 'argb' gives linear A,R,G,B.
    u8* d = m_guest.ram + off;
    const u8* s = frame.data();
    const u32 tiles_x = w / 4u;
    for (u32 ty = 0; ty < h / 4u; ++ty)
      for (u32 tx = 0; tx < tiles_x; ++tx)
      {
        u8* tile = d + (static_cast<size_t>(ty) * tiles_x + tx) * 64u;
        for (u32 j = 0; j < 4u; ++j)
          for (u32 i = 0; i < 4u; ++i)
          {
            const u8* px = s + ((static_cast<size_t>(ty * 4u + j) * w) + (tx * 4u + i)) * 4u;
            const u32 t = j * 4u + i;
            tile[t * 2u + 0u] = px[0];       // A
            tile[t * 2u + 1u] = px[1];       // R
            tile[32u + t * 2u + 0u] = px[2]; // G
            tile[32u + t * 2u + 1u] = px[3]; // B
          }
      }
  }
  // else: decoder not ready yet -> leave the buffer as-is (previous frame).
}

// STATICRECOMP_DEBUG_ID, read ONCE. It was read with getenv() at every Run()
// entry and on every fallback step -- a string search of the environment, on
// the emulation thread, during play: 0.43% of that thread in a Windows fight
// profile (WPR, 2026-09-30), the same mistake the FMV switches below once made.
static const char* DebugIdEnv()
{
  static const char* const value = std::getenv("STATICRECOMP_DEBUG_ID");
  return value;
}

void StaticRecompCore::Run()
{
  // The recomp is single-thread-bound, so scheduler migration between cores
  // (cold caches, contention with the GPU/audio/host threads) costs real fps.
  // Pin this CPU-emulation thread to a dedicated core. STATICRECOMP_CPU_AFFINITY
  // overrides the core (-1 disables). Default core 2 keeps it off core 0 (IRQs).
  //
  // MEASURED ON THE DECK 2026-08-19 -- keep the pin, but do not trust the
  // reasoning this comment used to give ("away from the GPU submission thread,
  // leaving siblings free"). That assumes BLOCKED cpu numbering. The Deck is
  // INTERLEAVED: cpu2's SMT sibling is cpu3, not cpu8, so the pin leaves the
  // other half of its own physical core free rather than a whole core. The
  // conclusion survives only because nothing meaningful lands there -- cpu3
  // measured 3.5% busy in Desktop Mode, 4.8% in Game Mode.
  //
  // 12000 frames of arcade-match.txt, unthrottled, AB/BA order:
  //   Game Mode  pinned 82.17 (n=4) vs unpinned 82.23 (n=4)  -> -0.07%
  //   Desktop    pinned 80.39 (n=8) vs unpinned 80.31 (n=4)  -> +0.10%
  // THE PIN IS WORTH NOTHING EITHER WAY. It is kept because it costs nothing
  // and bounds the worst case, not because it was measured to help.
  //
  // Two earlier verdicts here were BOTH artifacts, which is why the sample is
  // this large. "-0.96%, the pin hurts" came from running 3 arms per rep with
  // the order reversed on even reps -- reversing a 3-element list leaves the
  // MIDDLE fixed, so the unpinned arm sat in the fastest position every time.
  // "+1.53%, the pin helps" came from the first two runs after a fresh session
  // restart reading 82.59 twice; eight further runs never beat 81.97, and a
  // 6-minute cooldown did not bring it back (53C -> 48C, nowhere near
  // throttling). Run-to-run spread on this fixed workload is 4.1%, so any Deck
  // A/B needs n>=8 per arm or it will manufacture effects like those two.
  //
  // Core 0 is ~0.9% slower than core 2 (4/4 reps, position-balanced) -- the one
  // part of the original reasoning that held up, and the reason for the default.
#ifdef __linux__
  {
    static bool s_pinned = false;
    if (!s_pinned)
    {
      s_pinned = true;
      const char* e = std::getenv("STATICRECOMP_CPU_AFFINITY");
      const int core = e ? std::atoi(e) : 2;
      if (core >= 0)
      {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(core, &set);
        if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0)
          std::fprintf(stderr, "[staticrecomp] CPU thread pinned to core %d\n", core);
      }
    }
  }
#endif

  auto& core_timing = m_system.GetCoreTiming();
  auto& power_pc = m_system.GetPowerPC();
  auto& ppc = power_pc.GetPPCState();
  auto& interpreter = m_system.GetInterpreter();
  auto& memory = m_system.GetMemory();
  const CPU::State* state_ptr = m_system.GetCPU().GetStatePtr();

  m_guest.ram = memory.GetRAM();
  m_guest.ram_size = memory.GetRamSizeReal();
  m_guest.mem2 = memory.GetEXRAM();
  m_guest.mem2_size = memory.GetExRamSizeReal();
  InitLookupTable(m_guest.ram_size, m_guest.mem2_size);

  // The module's inlined memory fast paths bounds-check the cached-RAM window
  // against the compile-time GC_MAIN_RAM_SIZE, not m_guest.ram_size, so they are
  // in bounds only while MEM1 is at least retail size. With
  // MAIN_RAM_OVERRIDE_ENABLE and a smaller MAIN_MEM1_SIZE, a guest access the
  // fast path accepts would land past the end of the allocation -- silent host
  // heap corruption, because nothing downstream re-checks. A LARGER MEM1 is
  // safe: the constant bound is merely conservative and the out-of-line paths
  // still use ram_size. Refuse the module; the fallback JIT below runs instead.
  static_assert(static_cast<u32>(GC_MAIN_RAM_SIZE) == Memory::MEM1_SIZE_RETAIL,
                "module memory fast paths assume retail MEM1");
  const bool mem1_fits_module = m_guest.ram_size >= static_cast<u32>(GC_MAIN_RAM_SIZE);
  if (m_module && !mem1_fits_module)
  {
    ERROR_LOG_FMT(POWERPC,
                  "StaticRecomp: MEM1 is {} bytes, below the {} the module's memory fast paths "
                  "assume; module disabled (turn off the RAM override to use it).",
                  m_guest.ram_size, static_cast<u32>(GC_MAIN_RAM_SIZE));
    std::fprintf(stderr,
                 "[staticrecomp] module disabled: MEM1 %u bytes < %u assumed by the module's "
                 "memory fast paths (RAM override?)\n",
                 m_guest.ram_size, static_cast<unsigned>(GC_MAIN_RAM_SIZE));
  }

  const std::string initial_game_id = SConfig::GetInstance().GetGameID();
  m_module_active = mem1_fits_module && m_module &&
                    (initial_game_id.empty() || initial_game_id == m_module->game_id);

  if (DebugIdEnv() != nullptr)
  {
    fprintf(stderr, "[dbg] disc_game_id='%s' module_game_id='%s' module=%p active=%d ram_size=%u\n",
            initial_game_id.c_str(), m_module ? m_module->game_id : "(null)", (void*)m_module,
            (int)m_module_active, m_guest.ram_size);
    fflush(stderr);
  }

  if (!m_module_active && m_fallback_jit)
  {
    m_fallback_jit_used = true;
    m_fallback_jit->Run();
    return;
  }

  // FMV HLE env flags, read once. These were already cached in function-local
  // statics -- getenv() per dispatch had been the dominant cost, dropping a
  // movie from ~30 to ~12 fps -- but the DECLARATIONS sat inside the
  // per-dispatch loop, so each one still paid a thread-safe-initialisation
  // guard check every dispatch. Hoisting them here leaves one guard check per
  // Run() entry instead of four per dispatch.
  static const bool s_fmv_noexec = std::getenv("STATICRECOMP_FMV_NOEXEC") != nullptr;
  static const bool s_fmv_dumphndl = std::getenv("STATICRECOMP_FMV_DUMPHNDL") != nullptr;
  // Takeover: skip the game's software MPEG decode entirely and drive the
  // frame-ready state ourselves. Opt-in: measured NOT faster than letting the
  // decode run + idle-skip (~43 vs 47-60fps) because the decode was never the
  // bottleneck -- the OS idle-spin was, and idle-skip already reclaims it;
  // skipping the decode just leaves less idle to skip. Kept for
  // experimentation, off by default.
  static const bool s_fmv_takeover = std::getenv("STATICRECOMP_FMV_TAKEOVER") != nullptr;
  // PC histogram (16KB buckets) while a movie plays, to find the hot decode
  // function. Dumped by OnFmvStartAfs on the next movie / here.
  static const bool s_fmv_hist = std::getenv("STATICRECOMP_FMV_HIST") != nullptr;
  // Hoisted for the same reason as the flags above, and it was missed when they
  // were: a function-local static inside the dispatch loop costs a thread-safe
  // initialisation guard check on EVERY pass. Measured at 6.1% of Run()'s own
  // samples -- the single hottest instruction in the loop after the timebase
  // arithmetic -- for a flag that is almost always false.
  static const bool s_spinlog = std::getenv("STATICRECOMP_SPINLOG") != nullptr;
  static const bool s_dispatchlog = std::getenv("STATICRECOMP_DISPATCHLOG") != nullptr;
  // Locals, not the statics, inside the dispatch loop: the module call is
  // opaque, so after every dispatch the compiler had to re-load each static
  // from memory (perf annotate: these compares were ~6% of Run()). A local
  // whose address is never taken can stay in a register across the call.
  const bool fmv_hist_on = s_fmv_hist;
  const bool spinlog_on = s_spinlog;
  const bool dispatchlog_on = s_dispatchlog;
  // The FMV hook PCs, copied out of the object so the per-dispatch compares
  // below read registers, not memory. LoadModule (Init only) is their sole
  // writer, so they cannot change while Run() is on the stack.
  const StaticRecompFmvHookPcs fmv = m_fmv_pcs;

  u32 game_id_check = 0;
  while (*state_ptr == CPU::State::Running)
  {
    core_timing.Advance();
    if (m_gqr_log)
      SampleGQRs();
    // Advance() runs the due hardware events, which are the writers the module's
    // journal cannot see (DVD/DSP/AI DMA landing in RAM). Polling either side of
    // it separates those from anything the guest triggered.
    if (m_watch_armed)
      PollDeterminismWatch("coretiming", ppc.pc);
    // Re-checked every 64 slices, not every slice: GetGameID() takes a mutex
    // and copies a string, a few hundred times a frame, to catch a title
    // change that only happens when a new title boots. Noticing it up to 64
    // slices (~3 ms) late is harmless.
    if ((++game_id_check & 63) == 0)
    {
      const std::string current_game_id = SConfig::GetInstance().GetGameID();
      m_module_active = mem1_fits_module && m_module &&
                        (current_game_id.empty() || current_game_id == m_module->game_id);
    }

    do
    {
      // MSR.FP needs no gate here: generated FPU instructions raise the
      // FP-unavailable exception themselves (ppc_fp_available).
      if (m_module_active && DispatchableAt(ppc.pc))
      {
        SyncIn();
        ++m_bursts;
        do
        {
          // IsEnabled() is inline and ShouldCheck() is not, so calling the
          // latter unconditionally puts an out-of-line call on the dispatch
          // path of every normal run to answer "no". Measured at 0.23% of a
          // Deck profile doing nothing. ShouldCheck's own first test is this
          // same flag, so short-circuiting here is identical by construction.
          const bool do_ls = m_lockstep_verifier->IsEnabled() &&
                             m_lockstep_verifier->ShouldCheck(m_guest.pc);
          if (do_ls)
          {
            m_lockstep_verifier->Prepare(m_guest);
          }

          // --- FMV HLE hooks (fire before the guest function runs; m_guest
          // holds the PPC arg regs r3.. in gpr[3..]). The s_fmv_* env flags are
          // read once at the top of Run(); they used to be declared here, which
          // cost a static-init guard check per dispatch. ---
          if (fmv_hist_on)
            FmvHistSample(m_guest.pc);

          // Common path (no movie): just the start-detect compare. The frame /
          // decoder hooks engage only while a movie is active, so normal
          // gameplay pays almost nothing per dispatch. The PCs are per disc
          // (StaticRecompFmvHooks.h); on a disc with none they are unset and
          // never match.
          if (m_guest.pc == fmv.start_afs)  // mwPlyStartAfs(r3=handle,r4=patid,r5=fno)
            OnFmvStartAfs(m_guest.gpr[5], m_guest.gpr[4], m_guest.gpr[3]);
          if (s_fmv.Active())
          {
            if (m_guest.pc == fmv.cnv_frm)  // mwPlyFxCnvFrmARGB(r3=hnd,r4=desc,r5=dst)
            {
              // Replace the YUV->ARGB conversion: fill the destination buffer
              // natively and return to the caller, skipping the guest function.
              OnFmvCnvFrm(m_guest.gpr[3], m_guest.gpr[4], m_guest.gpr[5]);
              m_guest.gpr[3] = 0;
              m_guest.pc = m_guest.lr;
              continue;
            }
            if (s_fmv_dumphndl && m_guest.pc == fmv.exec_svr)
              OnFmvExecObserve(m_guest.gpr[3]);
            // Full decode-skip takeover: bypass the software MPEG pipeline and
            // synthesize the frame-ready state the game polls, so it advances
            // and blits our native frames without ever decoding.
            if (s_fmv_takeover)
            {
              if (m_guest.pc == fmv.exec_svr)  // mwPlyExecSvrHndl: skip MPEG decode
              {
                m_guest.gpr[3] = 0;
                m_guest.pc = m_guest.lr;
                continue;
              }
              if (m_guest.pc == fmv.next_frm_ready)  // mwPlyIsNextFrmReady: paced to 29.97fps
              {
                m_guest.gpr[3] = s_fmv.ReadyForNextFrame() ? 1u : 0u;
                m_guest.pc = m_guest.lr;
                continue;
              }
              if (m_guest.pc == fmv.rel_cur_frm)  // mwPlyRelCurFrm: nothing to release
              {
                m_guest.gpr[3] = 0;
                m_guest.pc = m_guest.lr;
                continue;
              }
              if (m_guest.pc == fmv.getfrm)  // getfrm: fabricate the frame desc
              {
                OnFmvGetFrm(m_guest.gpr[3], m_guest.gpr[4]);
                m_guest.gpr[3] = 0;
                m_guest.pc = m_guest.lr;
                continue;
              }
            }
            else if (s_fmv_noexec)
            {
              if (m_guest.pc == fmv.exec_svr)
              {
                m_guest.gpr[3] = 0;
                m_guest.pc = m_guest.lr;
                continue;
              }
            }
          }

          // --- Spawn-injection skin diagnostic (env-gated STATICRECOMP_SKINLOG).
          // 0x80126684 = per-object bone-matrix setup: r31=r3=object, reads
          // bone index at object+1, fetches matrix[bone] from the palette. The
          // injected model faults here because the caller passes a garbage
          // object pointer. Log the object ptr, its type/bone, the caller (lr),
          // and the caller's loop regs so we can see which iteration goes bad.
          static const bool s_skinlog = std::getenv("STATICRECOMP_SKINLOG") != nullptr;
          if (s_skinlog && m_guest.pc == 0x80126684u)
          {
            const u32 obj = m_guest.gpr[3];
            const bool bad = (obj < 0x80000000u) || (obj >= 0x80000000u + m_guest.ram_size);
            static int s_calls = 0, s_logged = 0;
            if (s_logged < 80 && (bad || s_calls < 40))
            {
              const u32 w0 = GuestRead32(obj);  // byte0=type, byte1=bone (BE)
              std::fprintf(stderr,
                "[skinlog] call#%d obj=0x%08x %s type=%u bone=%u lr=0x%08x "
                "r28=0x%08x r29=0x%08x r30=0x%08x r27=0x%08x\n",
                s_calls, obj, bad ? "BAD" : "ok", (w0 >> 24) & 0xFF, (w0 >> 16) & 0xFF,
                m_guest.lr, m_guest.gpr[28], m_guest.gpr[29], m_guest.gpr[30], m_guest.gpr[27]);
              ++s_logged;
            }
            ++s_calls;
          }

          // The write journal is handed a RAM offset and nothing else, so the
          // block being dispatched has to be recorded here for it to attribute
          // the store to. Two stores behind a member bool, in the shape of the
          // env-gated diagnostics above.
          if (m_watch_armed)
          {
            m_watch_block_pc = m_guest.pc;
            m_watch_block_lr = m_guest.lr;
          }

          // Why did this dispatch happen? (STATICRECOMP_DISPATCHLOG) The emitter
          // returns to the dispatcher for calls, for non-local branches, AND for
          // every local BACKWARD branch -- so each loop iteration is a full
          // round-trip. Whether that is worth attacking depends on how much of
          // the ~10.8M dispatches/sec it actually is, which is a measurement,
          // not a guess.
          const u32 dispatch_from = dispatchlog_on ? m_guest.pc : 0;

          const u32 dbg_pc_before = m_guest.pc;
          m_module->dispatch(&m_guest, m_guest.pc);
          ++m_native_dispatches;
          {
            // STATICRECOMP_SPINLOG=1: name the guest address a no-progress spin
            // is stuck on. A module can link, load and dispatch billions of
            // times while the guest never advances -- the run loop cannot tell
            // "executed a block" from "returned having done nothing", so the
            // symptom is a silent hang with a huge dispatch count and no error.
            // This found the LLVM backend's psq_load return-type mismatch in one
            // run after hours of hypotheses: it printed 0x80180BD4, a psq_l.
            if (spinlog_on)
            {
              static u64 s_same = 0;
              static u32 s_last = 0;
              if (m_guest.pc == dbg_pc_before)
              {
                if (m_guest.pc == s_last) ++s_same; else { s_same = 1; s_last = m_guest.pc; }
                if (s_same == 200000u)
                  std::fprintf(stderr, "[spin] no progress at pc=0x%08X (200k dispatches)\n",
                               m_guest.pc), std::fflush(stderr);
              }
            }
          }

          if (dispatchlog_on)
          {
            const u32 to = m_guest.pc;
            const int ci_from = ChunkIndexOf(dispatch_from);
            const int ci_to = ChunkIndexOf(to);
            if (ci_from >= 0 && ci_from == ci_to)
            {
              // Same chunk: a backward target is a loop back-edge, which is the
              // case that could have stayed native.
              if (to <= dispatch_from)
                ++m_dispatch_loop;
              else
                ++m_dispatch_fwd;
            }
            else
            {
              ++m_dispatch_cross;
            }
          }

          // A change seen here with nothing in the journal means the block's
          // MMIO side effects wrote it -- the guest asked hardware to, rather
          // than storing it itself -- and m_watch_block_pc names that block.
          if (m_watch_armed)
            PollDeterminismWatch("dispatch", m_watch_block_pc);

          if (do_ls)
          {
            m_lockstep_verifier->Verify(m_guest);
          }

          // Flush the module's per-block cycle charges into Dolphin's
          // downcount. A dispatch that charged nothing (PC-switch default,
          // pure embedded data) still costs 1 so the burst always makes
          // downcount progress; this per-dispatch flush is also the
          // dispatcher back-edge timing check — CoreTiming regains control
          // with at least CachedInterpreter's per-block frequency, so
          // external-interrupt latency matches stock.
          const s64 charge = -m_guest.downcount;
          m_guest.downcount = 0;
          const u64 charged = static_cast<u64>(charge > 0 ? charge : 1);
          ppc.downcount -= static_cast<int>(charged);
          m_charged_cycles += charged;
          // The Time Base advances at CPU clock / TIMER_RATIO (12), not once
          // per CPU cycle. Advancing it by the full cycle charge ran the guest
          // TB 12x fast, so mftb/OSGetTime-based FMV and animation pacing raced
          // ahead within a burst (movies too fast / dropped frames / A/V
          // desync). Carry the sub-tick remainder so the rate matches Dolphin's
          // GetFakeTimeBase() exactly while still letting busy-waits terminate.
          m_tb_cycle_remainder += charged;
          m_guest.timebase += m_tb_cycle_remainder / SystemTimers::TIMER_RATIO;
          m_tb_cycle_remainder %= SystemTimers::TIMER_RATIO;

          // Idle loop skipping for configured target loops (e.g. Wii Menu OSIdleThread)
          if (m_guest.pc == m_idle_pc && m_idle_pc != 0)
          {
            m_system.GetCoreTiming().Idle();
          }

          // ctx->timebase is refreshed at burst start (SyncIn), and here we
          // incrementally advance it by the exact block cycle charges to
          // prevent guest busy-wait loops from spinning on a stale timebase.
          if (m_guest.exception)
          {
            // DolRecomp's runtime already redirected pc/msr/srr to the guest
            // exception vector; the flag only signals that it happened.
            m_guest.exception = 0;
            m_guest.program_exception = 0;
            ++m_native_exceptions;
          }
          if ((ppc.Exceptions & SYNC_EXCEPTION_MASK) != 0)
            break;  // Hook-raised synchronous exception: deliver via Dolphin below.
        } while (m_module_active && FastDispatchableAt(m_guest.pc) && ppc.downcount > 0 &&
                 *state_ptr == CPU::State::Running);
        SyncOut();
        if ((ppc.Exceptions & SYNC_EXCEPTION_MASK) != 0)
          power_pc.CheckExceptions();
      }
      else
      {
        // SingleStepInner delivers synchronous exceptions itself; external
        // interrupts are delivered at slice start, as in Interpreter::Run.
        if (const char* dbg_env = DebugIdEnv())
        {
          static int dbg_fb_count = 0;
          const int dbg_cap = atoi(dbg_env) > 1 ? atoi(dbg_env) : 20;
          if (dbg_fb_count < dbg_cap)
          {
            fprintf(stderr, "[dbg] fallback #%d at guest pc=0x%08X (dispatchable=%d)\n",
                    dbg_fb_count, ppc.pc, (int)DispatchableAt(ppc.pc));
            fflush(stderr);
            ++dbg_fb_count;
          }
        }
        // Interpreter, NOT m_fallback_jit->Run(). Jit64::Run() enters `enter_code`,
        // which is Dolphin's whole-emulation dispatcher -- it runs until the CPU
        // state changes, which is why Dolphin calls it once per session. Using it
        // to cover a single non-dispatchable address hands the entire run to the
        // JIT and never gives this loop control back: no frame hashes, no FMV
        // hooks, no module dispatch ever again. It presented as a hang at 0
        // frames, and before the fault handler was installed, as a segfault in
        // JIT-generated memory. The loop below is the only form that returns as
        // soon as the guest reaches an address the module can enter.
        do
        {
          ppc.downcount -= interpreter.SingleStepInner();
          ++m_fallback_steps;
        } while (!(m_module_active && DispatchableAt(ppc.pc)) && ppc.downcount > 0 &&
                 *state_ptr == CPU::State::Running);
      }
    } while (ppc.downcount > 0 && *state_ptr == CPU::State::Running);
  }
}

void StaticRecompCore::SingleStep()
{
  // Debugger stepping runs through the interpreter; state outside Run() lives
  // in PowerPCState, so no sync is needed.
  auto& system = m_system;
  system.GetCoreTiming().Advance();
  system.GetPPCState().downcount -= system.GetInterpreter().SingleStepInner();
}
