// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/RecompDeterminism.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Hash.h"
#include "Core/Core.h"
#include "Core/HW/Memmap.h"
#include "Core/State.h"
#include "Core/System.h"
#include "VideoCommon/Fifo.h"

#include <chrono>
#include "InputCommon/GCPadStatus.h"

namespace RecompDeterminism
{
namespace
{
// The OS globals block at the bottom of main memory: boot info, console type,
// and the RTC / time base the IPL seeds at boot. Everything above it is the
// game's own state.
constexpr u32 kOSGlobalsSize = 0x3100;

struct Config
{
  std::FILE* out = nullptr;
  u64 frames = 0;  // 0 = run until the game is stopped by hand
  bool active = false;

  // Whether to actually hash. See OnFrame: the hash IS the cost of this
  // harness, and a performance run wants the scripted route without it.
  bool hash = true;

  // RINGOUT_DETERMINISM_ASPLAYED (NOHASH runs only): the route and the frame
  // counter, with the emulation left in the player's shape. See IsAsPlayed.
  bool as_played = false;

  // Whole-RAM snapshot at one frame. A hash says two runs differ; only a dump
  // says where, and an address is what turns a divergence into a subsystem.
  std::string dump_path;
  u64 dump_frame = 0;

  // Write watch on one guest address, reported by the StaticRecomp core from
  // the module's write journal. Capped, because an address that turns out to be
  // written every frame would otherwise bury the run's output.
  u32 watch_address = 0;
  u32 watch_size = 4;
  u64 watch_max = 20;
  u64 watch_hits = 0;
  bool watch = false;

  // Per-frame poll of the watched word, alongside the journal. The journal only
  // sees stores made by recompiled guest code; DMA and anything the chassis
  // writes into guest RAM natively bypass it. Polling catches the change no
  // matter who made it, and pairing it with the store count for the same frame
  // is what distinguishes the two cases: a change with no store behind it did
  // not come from the guest CPU.
  u32 watch_last_value = 0;
  u64 watch_stores = 0;   // native stores seen since the previous frame
  u64 watch_changes = 0;  // changes reported, against the same cap

  // Scripted input: sorted by frame, each entry the pad state to hold from that
  // frame until the next one.
  struct PadFrame
  {
    u64 frame;
    u16 button;
    u8 stick_x, stick_y, substick_x, substick_y, trigger_l, trigger_r;
  };
  // One script per SI port. A port with no lines of its own is left alone and
  // falls through to the host pad, so scripting pad 1 cannot fake a pad 2.
  static constexpr int kPorts = 4;
  std::array<std::vector<PadFrame>, kPorts> input;
  std::array<std::size_t, kPorts> input_cursor{};

  // Rollback probe. Save at frame `rb_at`, run `rb_len` frames, restore, replay
  // the same `rb_len` frames, and require the state to come out identical --
  // which is exactly what rollback netplay does every time it mispredicts.
  u64 rb_at = 0;
  u64 rb_len = 0;
  bool rb_on = false;
  Common::UniqueBuffer<u8> rb_buffer;
  std::size_t rb_size = 0;
  u32 rb_hash = 0;       // game-memory hash at the end of the first pass
  bool rb_replaying = false;
  u64 rb_input_offset = 0;  // frames to subtract so the replay sees the same input
};

// Button names as they appear in a script, matched case-insensitively.
struct PadName
{
  const char* name;
  u16 bit;
};
constexpr PadName kPadNames[] = {
    {"A", PAD_BUTTON_A},         {"B", PAD_BUTTON_B},
    {"X", PAD_BUTTON_X},         {"Y", PAD_BUTTON_Y},
    {"Z", PAD_TRIGGER_Z},        {"START", PAD_BUTTON_START},
    {"L", PAD_TRIGGER_L},        {"R", PAD_TRIGGER_R},
    {"UP", PAD_BUTTON_UP},       {"DOWN", PAD_BUTTON_DOWN},
    {"LEFT", PAD_BUTTON_LEFT},   {"RIGHT", PAD_BUTTON_RIGHT},
};

// One line: "[P<n>] <frame> <buttons|-> [stick_x stick_y [substick_x substick_y [l r]]]".
// Buttons are comma-separated, '-' meaning none. Blank lines and '#' comments
// are skipped. Sticks default to centred (0x80) and triggers to 0.
//
// The optional leading "P1"/"P2" selects the SI port and defaults to P1, so
// every existing single-pad script parses exactly as it did before. A second
// port is what a VS-mode route needs: character lock, health and stage select
// are each confirmed by the side that owns them, and the stage pick is 2P's.
bool ParseInputLine(const std::string& line, Config::PadFrame* out, int* pad)
{
  std::string text = line.substr(0, line.find('#'));
  std::istringstream stream(text);
  std::string frame_token, buttons;
  if (!(stream >> frame_token))
    return false;

  // A frame is digits, so a leading 'P' can only be a port selector -- the two
  // forms cannot be confused.
  *pad = 0;
  if (frame_token.size() >= 2 && (frame_token[0] == 'P' || frame_token[0] == 'p') &&
      std::isdigit(static_cast<unsigned char>(frame_token[1])))
  {
    const int port = std::atoi(frame_token.c_str() + 1) - 1;
    if (port < 0 || port >= Config::kPorts)
      return false;
    *pad = port;
    if (!(stream >> frame_token))
      return false;
  }
  if (!(stream >> buttons))
    return false;

  *out = Config::PadFrame{};
  out->frame = std::strtoull(frame_token.c_str(), nullptr, 10);
  out->stick_x = out->stick_y = out->substick_x = out->substick_y = 0x80;

  if (buttons != "-")
  {
    std::string name;
    std::istringstream names(buttons);
    while (std::getline(names, name, ','))
    {
      for (char& c : name)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      bool matched = false;
      for (const PadName& known : kPadNames)
      {
        if (name == known.name)
        {
          out->button |= known.bit;
          matched = true;
        }
      }
      // Silence here is expensive: a name this table does not know used to be
      // dropped without a word, so the line pressed NOTHING and the route ran
      // on into whatever the next press confirmed. The d-pad is "DOWN", not the
      // Pipe backend's "D_DOWN", and that one difference is easy to carry over.
      if (!matched)
      {
        std::fprintf(stderr, "[input] unknown button '%s' in: %s\n", name.c_str(),
                     line.c_str());
      }
    }
  }

  int value = 0;
  if (stream >> value) out->stick_x = static_cast<u8>(value);
  if (stream >> value) out->stick_y = static_cast<u8>(value);
  if (stream >> value) out->substick_x = static_cast<u8>(value);
  if (stream >> value) out->substick_y = static_cast<u8>(value);
  if (stream >> value) out->trigger_l = static_cast<u8>(value);
  if (stream >> value) out->trigger_r = static_cast<u8>(value);
  return true;
}

// Configured from the environment rather than the command line on purpose:
// this is a diagnostic for a question about the core, not a feature of the
// game, and it stays out of the runtime's option surface entirely.
Config& GetConfig()
{
  static Config config = [] {
    Config result;
    const char* const path = std::getenv("RINGOUT_DETERMINISM_LOG");
    if (path == nullptr || *path == '\0')
      return result;

    result.out = std::fopen(path, "w");
    if (result.out == nullptr)
      return result;

    if (const char* const frames = std::getenv("RINGOUT_DETERMINISM_FRAMES"))
      result.frames = std::strtoull(frames, nullptr, 10);

    // RINGOUT_DETERMINISM_NOHASH: keep the frame-keyed input and the frame
    // counter, drop the per-frame hash. Turns this into a PERFORMANCE harness.
    // Ignored when a dump or a watch is asked for, because both read guest RAM
    // and rely on the same quiesce the hash does.
    result.hash = std::getenv("RINGOUT_DETERMINISM_NOHASH") == nullptr;

    if (const char* const dump = std::getenv("RINGOUT_DETERMINISM_DUMP"))
    {
      result.dump_path = dump;
      if (const char* const frame = std::getenv("RINGOUT_DETERMINISM_DUMP_FRAME"))
        result.dump_frame = std::strtoull(frame, nullptr, 10);
    }

    if (const char* const watch = std::getenv("RINGOUT_DETERMINISM_WATCH"))
    {
      // Base 0, so the address can be written the way the dump reports it.
      result.watch_address = static_cast<u32>(std::strtoull(watch, nullptr, 0));
      result.watch = result.watch_address != 0;
      if (const char* const size = std::getenv("RINGOUT_DETERMINISM_WATCH_SIZE"))
        result.watch_size = static_cast<u32>(std::strtoul(size, nullptr, 0));
      if (result.watch_size == 0)
        result.watch_size = 4;
      if (const char* const max = std::getenv("RINGOUT_DETERMINISM_WATCH_MAX"))
        result.watch_max = std::strtoull(max, nullptr, 10);
    }

    if (const char* const input = std::getenv("RINGOUT_DETERMINISM_INPUT"))
    {
      std::ifstream file(input);
      if (!file)
      {
        std::fprintf(stderr, "[input] cannot open %s; running with no input\n", input);
      }
      else
      {
        std::string line;
        while (std::getline(file, line))
        {
          Config::PadFrame entry;
          int pad = 0;
          if (ParseInputLine(line, &entry, &pad))
            result.input[pad].push_back(entry);
        }
        for (int port = 0; port < Config::kPorts; ++port)
        {
          std::stable_sort(result.input[port].begin(), result.input[port].end(),
                           [](const Config::PadFrame& a, const Config::PadFrame& b) {
                             return a.frame < b.frame;
                           });
          if (!result.input[port].empty())
          {
            std::fprintf(stderr, "[input] P%d: %zu scripted pad state(s) from %s\n", port + 1,
                         result.input[port].size(), input);
          }
        }
      }
    }

    if (const char* const at = std::getenv("RINGOUT_DETERMINISM_ROLLBACK_AT"))
    {
      result.rb_at = std::strtoull(at, nullptr, 10);
      result.rb_len = 60;
      if (const char* const len = std::getenv("RINGOUT_DETERMINISM_ROLLBACK_LEN"))
        result.rb_len = std::strtoull(len, nullptr, 10);
      result.rb_on = result.rb_len != 0;
    }

    // A dump or a watch reads guest RAM and needs the same GPU quiesce the hash
    // does, so NOHASH is refused rather than silently making them racy.
    if (!result.hash && (!result.dump_path.empty() || result.watch))
    {
      std::fprintf(stderr, "[determinism] NOHASH ignored: a dump or a watch "
                           "needs the per-frame quiesce\n");
      result.hash = true;
    }
    if (!result.hash)
      std::fprintf(stderr, "[determinism] NOHASH: input and frame counter only, "
                           "no hashing -- this run cannot verify determinism\n");

    result.as_played = std::getenv("RINGOUT_DETERMINISM_ASPLAYED") != nullptr;
    if (result.as_played && result.hash)
    {
      std::fprintf(stderr, "[determinism] ASPLAYED ignored: it needs NOHASH\n");
      result.as_played = false;
    }
    if (result.as_played)
      std::fprintf(stderr, "[determinism] ASPLAYED: dual-core, GPU determinism off -- "
                           "runs differ, and the route can drift\n");

    result.active = true;
    return result;
  }();
  return config;
}

u64 s_frame = 0;
}  // namespace

bool IsActive()
{
  return GetConfig().active;
}

bool IsAsPlayed()
{
  return GetConfig().as_played;
}

bool IsWatchArmed()
{
  return GetConfig().watch;
}

u32 WatchGuestAddress()
{
  return GetConfig().watch_address;
}

u32 WatchSize()
{
  return GetConfig().watch_size;
}

void ReportWatchWrite(u32 block_pc, u32 lr, u32 size, u32 old_value, u64 timebase)
{
  Config& config = GetConfig();
  // Counted before the cap, so the per-frame attribution stays honest after the
  // reports go quiet.
  ++config.watch_stores;
  if (config.watch_hits >= config.watch_max)
    return;
  ++config.watch_hits;

  // stderr, not the hash log: the log is two numbers a script compares between
  // runs, and interleaving prose into it would break that. The frame is what
  // ties this line back to the log and the dump.
  std::fprintf(stderr,
               "[watch] frame=%llu %u-byte store to 0x%08X from block 0x%08X (lr=0x%08X) "
               "was=0x%08X tb=%llu\n",
               static_cast<unsigned long long>(s_frame), size, config.watch_address, block_pc, lr,
               old_value, static_cast<unsigned long long>(timebase));
  std::fflush(stderr);
  if (config.watch_hits == config.watch_max)
    std::fprintf(stderr, "[watch] report cap (%llu) reached; going quiet.\n",
                 static_cast<unsigned long long>(config.watch_max));
}

void PollWatch(const char* site, u32 pc, u32 value)
{
  Config& config = GetConfig();
  if (!config.watch || value == config.watch_last_value)
    return;

  if (config.watch_changes < config.watch_max)
  {
    ++config.watch_changes;
    std::fprintf(stderr,
                 "[watch] frame=%llu 0x%08X changed 0x%08X -> 0x%08X seen at %s (pc=0x%08X, "
                 "%llu native store(s) this frame)\n",
                 static_cast<unsigned long long>(s_frame), config.watch_address,
                 config.watch_last_value, value, site, pc,
                 static_cast<unsigned long long>(config.watch_stores));
    std::fflush(stderr);
  }
  config.watch_last_value = value;
}

bool ScriptedPad(int device_number, ::GCPadStatus* status)
{
  Config& config = GetConfig();
  // Only ports the script actually names are driven. Feeding one port's script
  // to every port would fake a four-player game, so an unscripted port falls
  // through to the host pad exactly as it did before ports existed here.
  if (!config.active || status == nullptr || device_number < 0 ||
      device_number >= Config::kPorts || config.input[device_number].empty())
  {
    return false;
  }

  const std::vector<Config::PadFrame>& script = config.input[device_number];
  std::size_t& cursor = config.input_cursor[device_number];

  // The script is sorted and s_frame only advances, so this walks forward once
  // over the run rather than searching per poll -- SI polls the pad several
  // times a frame.
  // During a replay the frame counter keeps climbing, so the script has to be
  // rewound by the same amount or the replay would receive DIFFERENT input and
  // 'prove' non-determinism that is really just a different button press.
  const u64 input_frame = s_frame - config.rb_input_offset;
  while (cursor + 1 < script.size() && script[cursor + 1].frame <= input_frame)
    ++cursor;
  while (cursor > 0 && script[cursor].frame > input_frame)
    --cursor;

  const Config::PadFrame& entry = script[cursor];
  // Nothing scripted yet: hold neutral rather than falling through to the host
  // pad, so a stray keypress cannot perturb a measurement run.
  const bool pending = entry.frame > input_frame;

  *status = GCPadStatus{};
  status->isConnected = true;
  status->button = static_cast<u16>((pending ? 0 : entry.button) | PAD_USE_ORIGIN);
  status->stickX = pending ? GCPadStatus::MAIN_STICK_CENTER_X : entry.stick_x;
  status->stickY = pending ? GCPadStatus::MAIN_STICK_CENTER_Y : entry.stick_y;
  status->substickX = pending ? GCPadStatus::C_STICK_CENTER_X : entry.substick_x;
  status->substickY = pending ? GCPadStatus::C_STICK_CENTER_Y : entry.substick_y;
  status->triggerLeft = pending ? 0 : entry.trigger_l;
  status->triggerRight = pending ? 0 : entry.trigger_r;
  return true;
}

void OnFrame(Core::System& system)
{
  Config& config = GetConfig();
  if (!config.active)
    return;

  // Quiesce the GPU before reading RAM. Under dual-core the GPU thread writes
  // guest RAM (EFB/XFB copies) while the CPU thread runs, so a hash taken here
  // would race it and two runs would differ whether or not the core is
  // deterministic -- which is why this harness used to force single-core, and
  // why it could not validate the configuration netplay actually ships.
  //
  // SyncGPUForRegisterAccess is the sanctioned "about to read state the GPU may
  // have touched" call. With a deterministic GPU thread it advances the GPU on
  // the CPU thread rather than waiting on a race, so the quiesce is itself
  // reproducible. It perturbs timing, but identically in every run, which is all
  // a comparison between two runs requires.
  auto& memory = system.GetMemory();
  const u8* const ram = memory.GetRAM();
  if (ram == nullptr)
    return;

  // THE QUIESCE AND THE HASH ARE THE ENTIRE COST OF THIS HARNESS: a CRC32 over
  // all 24 MB of MEM1 every frame -- ~1.7 GB/s at 72 fps -- plus a full GPU
  // sync. Correct when the question is "do two runs agree". Ruinous when the
  // question is "how fast is this", because it does not merely add CPU work, it
  // SERIALISES the CPU and GPU threads that dual-core exists to overlap, so the
  // measured shape is the harness's and not the game's.
  //
  // With NOHASH the line is still written, with zeros: the frame counter is how
  // a run is located against the route's landmarks, and it costs nothing.
  const u32 ram_size = memory.GetRamSizeReal();
  u32 low_hash = 0;
  u32 rest_hash = 0;
  u32 l1_hash = 0;
  if (config.hash)
  {

  // Hashed in two pieces, because "the RAM differs" is not a usable answer.
  //
  // The bottom of memory is the OS globals block: the boot info, the console
  // type and, critically, the real-time clock and time base the IPL seeds at
  // boot. Two runs started seconds apart differ there by construction, and
  // that is not a core defect -- it is why netplay pins the RTC across peers
  // rather than hoping. Splitting it out separates that expected difference
  // from a genuine one in the heap, where the game's actual state lives.
    system.GetFifo().SyncGPUForRegisterAccess();

    const u32 low_size = std::min<u32>(kOSGlobalsSize, ram_size);
    low_hash = Common::ComputeCRC32(ram, low_size);
    rest_hash = Common::ComputeCRC32(ram + low_size, ram_size - low_size);
    if (const u8* const l1 = memory.GetL1Cache())
      l1_hash = Common::ComputeCRC32(l1, memory.GetL1CacheSize());
  }

  std::fprintf(config.out, "%llu %08x %08x %08x\n", static_cast<unsigned long long>(s_frame),
               low_hash, rest_hash, l1_hash);
  // Flushed every frame because the interesting run is the one that diverges
  // and then crashes; a buffered tail would lose exactly the frames that matter.
  std::fflush(config.out);

  // ---- Rollback probe -----------------------------------------------------
  // Three points in one run: save, measure the state at the end of the first
  // pass, then restore and replay. If replay lands on a different hash, rollback
  // netplay cannot work no matter how fast the core gets, so this is worth
  // knowing before optimising anything else.
  if (config.rb_on)
  {
    using Clock = std::chrono::steady_clock;
    const auto ms = [](Clock::duration d) {
      return std::chrono::duration<double, std::milli>(d).count();
    };

    if (s_frame == config.rb_at)
    {
      // The first call runs with an empty buffer, so PointerWrap measures,
      // grows and then saves again -- it does the work twice plus a >100 MiB
      // allocation. Rollback would be saving into a warm buffer every frame, so
      // the second timing is the one that matters; both are reported.
      const auto start = Clock::now();
      config.rb_size = State::SaveToBuffer(system, config.rb_buffer);
      const auto cold = Clock::now() - start;
      const auto warm_start = Clock::now();
      config.rb_size = State::SaveToBuffer(system, config.rb_buffer);
      const auto elapsed = Clock::now() - warm_start;
      std::fprintf(stderr,
                   "[rollback] frame %llu: saved %.2f MiB in %.2f ms warm "
                   "(%.2f ms cold, incl. measure+alloc)\n",
                   static_cast<unsigned long long>(s_frame),
                   double(config.rb_size) / (1024.0 * 1024.0), ms(elapsed), ms(cold));
      std::fflush(stderr);
    }
    else if (!config.rb_replaying && s_frame == config.rb_at + config.rb_len)
    {
      config.rb_hash = rest_hash;
      const auto start = Clock::now();
      const bool ok = State::LoadFromBuffer(system, {config.rb_buffer.data(), config.rb_size});
      const auto elapsed = Clock::now() - start;
      config.rb_replaying = true;
      // The frame counter does not rewind, so rewind the input script instead.
      config.rb_input_offset = config.rb_len;
      std::fprintf(stderr,
                   "[rollback] frame %llu: restored in %.2f ms (%s); replaying %llu frames\n",
                   static_cast<unsigned long long>(s_frame), ms(elapsed), ok ? "ok" : "FAILED",
                   static_cast<unsigned long long>(config.rb_len));
      std::fflush(stderr);
    }
    else if (config.rb_replaying && s_frame == config.rb_at + 2 * config.rb_len)
    {
      const bool match = rest_hash == config.rb_hash;
      std::fprintf(stderr, "[rollback] replay %s: game-memory hash %08x vs %08x\n",
                   match ? "MATCHED" : "DIVERGED", rest_hash, config.rb_hash);
      std::fprintf(stderr, "[rollback] %s\n",
                   match ? "restore + replay reproduces state exactly -- rollback is viable"
                         : "restore + replay does NOT reproduce state -- rollback is blocked");
      std::fflush(stderr);
      config.rb_on = false;
    }
  }

  // Backstop poll. The core polls far more finely; this catches a change made
  // by anything that runs outside the native run loop entirely.
  if (config.watch)
  {
    const u32 offset = config.watch_address - 0x80000000u;
    if (offset + 4 <= ram_size)
    {
      const u8* const p = ram + offset;
      PollWatch("frame", 0, (u32{p[0]} << 24) | (u32{p[1]} << 16) | (u32{p[2]} << 8) | u32{p[3]});
    }
    config.watch_stores = 0;
  }

  // Dumped after the hash of the same frame, so the log line and the snapshot
  // describe identical state. Raw main RAM with no header: the file offset is
  // the offset into RAM, which makes the guest address 0x80000000 + offset and
  // lets cmp do the comparison without a tool of our own.
  if (!config.dump_path.empty() && s_frame == config.dump_frame)
  {
    if (std::FILE* const dump = std::fopen(config.dump_path.c_str(), "wb"))
    {
      std::fwrite(ram, 1, ram_size, dump);
      std::fclose(dump);
    }
    config.dump_path.clear();
  }

  ++s_frame;
  if (config.frames != 0 && s_frame >= config.frames)
  {
    std::fclose(config.out);
    config.out = nullptr;
    config.active = false;
    // Stopping here would tear the core down from inside its own frame update,
    // so it goes through the host job queue instead.
    Core::QueueHostJob([](Core::System& stopping) { Core::Stop(stopping); });
  }
}
}  // namespace RecompDeterminism
