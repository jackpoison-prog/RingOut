// RecompCore: StaticRecomp CPU core.
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include "Core/PowerPC/StaticRecomp/StaticRecompCore.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "Common/Config/Config.h"
#include "Common/DynamicLibrary.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/StaticRecompSettings.h"
#include "Core/Config/ConfigManager.h"
#include "Core/HW/GPFifo.h"
#include "Core/HW/Memmap.h"
#include "Core/MemTools.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompLockstep.h"
#include "Core/RecompDeterminism.h"
#include "Core/RecompScreenShape.h"
#include "Core/System.h"

#ifdef _M_X86_64
#include "Core/PowerPC/Jit64/Jit.h"
#endif
#ifdef _M_ARM_64
#include "Core/PowerPC/JitArm64/Jit.h"
#endif


StaticRecompCore* g_static_recomp_core = nullptr;

bool StaticRecompCoveredAt(u32 pc)
{
  return g_static_recomp_core && g_static_recomp_core->IsModuleActive() &&
         g_static_recomp_core->DispatchableAt(pc);
}

namespace
{
bool RangesAreSorted(const StaticRecompRange* ranges, u32 count)
{
  if (!ranges || count == 0)
    return false;
  for (u32 i = 0; i < count; ++i)
  {
    if (ranges[i].start >= ranges[i].end ||
        (i != 0 && ranges[i - 1].end > ranges[i].start))
      return false;
  }
  return true;
}

bool AddressIsCovered(const StaticRecompRange* ranges, u32 count, u32 address)
{
  for (u32 i = 0; i < count; ++i)
  {
    if (address >= ranges[i].start && address < ranges[i].end)
      return true;
  }
  return false;
}

bool ChunksTileCode(const StaticRecompModuleDesc& desc)
{
  if (!RangesAreSorted(desc.chunk_ranges, desc.num_chunk_ranges) || !desc.chunk_hashes)
    return false;
  u32 chunk = 0;
  for (u32 code = 0; code < desc.num_code_ranges; ++code)
  {
    u32 cursor = desc.code_ranges[code].start;
    while (chunk < desc.num_chunk_ranges && desc.chunk_ranges[chunk].start < desc.code_ranges[code].end)
    {
      if (desc.chunk_ranges[chunk].start != cursor ||
          desc.chunk_ranges[chunk].end > desc.code_ranges[code].end)
        return false;
      cursor = desc.chunk_ranges[chunk++].end;
    }
    if (cursor != desc.code_ranges[code].end)
      return false;
  }
  return chunk == desc.num_chunk_ranges;
}
}  // namespace

bool StaticRecompCore::IsModuleActive() const
{
  return m_module_active;
}

StaticRecompCore::StaticRecompCore(Core::System& system, StaticRecompModuleSource module_source)
    : JitBase(system), m_module_source(std::move(module_source))
{
}

StaticRecompCore::~StaticRecompCore() = default;

// GX command and vertex traffic reaches the host as ordinary guest stores to the
// write-gather pipe page, and every one of them crossed the .so boundary into
// HookExternalWrite to move a few bytes into a buffer. On a draw-call-heavy
// scene that is the busiest thing the chassis does.
//
// Hand the module the buffer instead. It writes the bytes inline and calls back
// only when the pipe fills: once per 32 bytes rather than once per word. This is
// what Dolphin's own JITs do with optimizeGatherPipe.
//
// Installed through an optional export rather than a CPUState field, so no ABI
// bump and no coupling of module and runtime deployment (the same reasoning as
// the lazy-FPRF note in the module's cpu.h). A module without the symbol keeps
// the old path and still works with this runtime.
void StaticRecompCore::InstallGatherPipeFastPath()
{
  if (!m_module)
    return;
  const auto set_gather_pipe =
      reinterpret_cast<SetGatherPipeFn>(m_library.GetSymbolAddress("ppc_set_gather_pipe"));
  if (!set_gather_pipe)
  {
    std::fprintf(stderr, "[staticrecomp] module lacks ppc_set_gather_pipe export; gather-pipe "
                         "stores stay on the external-write hook.\n");
    return;
  }

  auto& ppc_state = m_system.GetPPCState();
  // Addresses, not values: GPFifoManager::Init() may not have run yet, so the
  // module re-reads both every time rather than snapshotting a NULL here.
  //
  // m_ls_journaling is the stand-down flag. While the lockstep verifier is
  // journaling it needs every MMIO write recorded by the hook, so the module
  // must route these stores back through HookExternalWrite instead of servicing
  // them silently. bool is one byte, hence the unsigned char view.
  set_gather_pipe(&ppc_state.gather_pipe_ptr, &ppc_state.gather_pipe_base_ptr,
                  &StaticRecompCore::GatherPipeFlushTrampoline, this,
                  reinterpret_cast<const unsigned char*>(&m_lockstep_verifier->m_ls_journaling));
}

// Called by the module only when the pipe has reached its fill mark.
// FastCheckGatherPipe re-checks the count and flushes; it is deliberately the
// Fast variant, for the reason spelled out in HookExternalWrite.
void StaticRecompCore::GatherPipeFlushTrampoline(void* user)
{
  auto* core = static_cast<StaticRecompCore*>(user);
  core->m_system.GetGPFifo().FastCheckGatherPipe();
}

void StaticRecompCore::Init()
{
  g_static_recomp_core = this;
  RefreshConfig();
  jo.enableBlocklink = false;
  jo.fastmem = false;
  jo.fastmem_arena = false;

  m_block_cache.Init();

  m_guest = CPUState{};
  m_guest.external_read = HookExternalRead;
  m_guest.external_write = HookExternalWrite;
  m_guest.external_read32 = HookExternalRead32;
  m_guest.external_write32 = HookExternalWrite32;
  m_guest.instruction_fallback = HookInstructionFallback;
  m_guest.host_call = nullptr;
  m_guest.external_user_data = this;

  std::fprintf(stderr, "[staticrecomp] core init\n");

  LoadModule();
  m_idle_pc = Config::Get(Config::MAIN_STATICRECOMP_IDLE_PC);
  // Env override for experimenting with idle-loop skipping: when the guest sits
  // in an OS scheduler spin loop waiting for an interrupt, CoreTiming::Idle()
  // fast-forwards to the next event instead of burning real wall-time spinning.
  if (const char* e = std::getenv("STATICRECOMP_IDLE_PC"))
    m_idle_pc = static_cast<u32>(std::strtoul(e, nullptr, 0));
  m_lockstep_verifier = std::make_unique<StaticRecompLockstep::StaticRecompLockstepVerifier>(*this);
  m_lockstep_verifier->Init();
  InstallGatherPipeFastPath();
  InitDeterminismWatch();
  m_gqr_log = std::getenv("STATICRECOMP_GQRLOG") != nullptr;

  // The "interpreter fallback" is really Dolphin's JIT whenever one exists, so
  // m_fallback_steps stays 0 on a run that fell back constantly -- read
  // m_fallback_jit_used instead. STATICRECOMP_NO_FALLBACK_JIT forces the true
  // interpreter, which is how you tell a module-side bug from a JIT-handoff one.
  if (std::getenv("STATICRECOMP_NO_FALLBACK_JIT") == nullptr)
  {
#ifdef _M_ARM_64
    m_fallback_jit = std::make_unique<JitArm64>(m_system);
#elif defined(_M_X86_64)
    m_fallback_jit = std::make_unique<Jit64>(m_system);
#endif
  }
  if (m_fallback_jit)
  {
    // Jit64 emits raw host loads for fastmem and depends on a SIGSEGV handler to
    // backpatch the ones that miss; it also faults deliberately for the BLR
    // optimisation. Dolphin installs that handler in Core::CpuThread, which this
    // runtime does not use -- so without this, every fastmem miss inside JIT code
    // is a fatal segfault in anonymous executable memory, with no symbols and a
    // broken frame chain. Whoever constructs the JIT owes it its handler.
    // Core::EmuThread installs the same handler for its own JIT, and both paths
    // can be live at once, so these calls are reference-counted inside EMM --
    // see MemTools.cpp. Installing unconditionally here is correct and stays
    // correct if a third owner ever appears.
    if (EMM::IsExceptionHandlerSupported())
      EMM::InstallExceptionHandler();
    m_fallback_jit->Init();
  }
}

void StaticRecompCore::InitDeterminismWatch()
{
  if (!RecompDeterminism::IsWatchArmed())
    return;
  if (!m_module)
  {
    std::fprintf(stderr, "[watch] no module loaded; there are no native stores to watch.\n");
    return;
  }
  // The module has exactly one journal slot, and the lockstep verifier claims
  // and releases it around every checked block. Sharing it would silently drop
  // whichever writes fell outside a lockstep window, so refuse instead.
  if (m_lockstep_verifier->IsEnabled())
  {
    std::fprintf(stderr, "[watch] STATICRECOMP_LOCKSTEP already owns the module's write "
                         "journal; watch DISABLED (run them separately).\n");
    return;
  }
  const auto set_journal = reinterpret_cast<StaticRecompLockstep::SetMemJournalFn>(
      m_library.GetSymbolAddress("ppc_set_mem_write_journal"));
  if (!set_journal)
  {
    std::fprintf(stderr, "[watch] module lacks ppc_set_mem_write_journal export; "
                         "watch DISABLED (rebuild the module).\n");
    return;
  }

  // Cached MEM1 only, which is what the harness's dump covers and therefore the
  // only place an address it reports can be. Rejected loudly rather than left to
  // match nothing, because a silent watch is meant to mean "nobody stored here".
  // Asked of the memory manager, not m_guest: Run() binds m_guest.ram, so its
  // size is still zero this early.
  const u32 address = RecompDeterminism::WatchGuestAddress();
  const u32 size = RecompDeterminism::WatchSize();
  const u32 offset = address - 0x80000000u;
  const u32 ram_size = m_system.GetMemory().GetRamSizeReal();
  if (ram_size == 0 || size > ram_size || offset > ram_size - size)
  {
    std::fprintf(stderr,
                 "[watch] 0x%08X + %u is outside cached MEM1 (0x80000000..0x%08X); "
                 "watch DISABLED.\n",
                 address, size, 0x80000000u + ram_size);
    return;
  }

  m_watch_offset = offset;
  m_watch_size = size;
  m_watch_armed = true;
  // Installed for the whole run, not per block: the write we are hunting for
  // happens once, and we do not know when.
  set_journal(&StaticRecompCore::DeterminismWatchTrampoline, this);
  std::fprintf(stderr, "[watch] armed on 0x%08X (%u bytes, RAM offset 0x%X)\n", address,
               m_watch_size, m_watch_offset);
}

// Called from inside the module's store fast path, before the store commits, so
// this runs on every guest RAM write while the watch is armed -- the range test
// is deliberately the entire body.
void StaticRecompCore::DeterminismWatchTrampoline(u32 offset, u32 size, void* user)
{
  auto* const core = static_cast<StaticRecompCore*>(user);
  if (offset + size <= core->m_watch_offset || offset >= core->m_watch_offset + core->m_watch_size)
    return;

  RecompDeterminism::ReportWatchWrite(core->m_watch_block_pc, core->m_watch_block_lr, size,
                                      core->GuestRead32(0x80000000u + core->m_watch_offset),
                                      core->m_guest.timebase);
}

// A GQR holds two independent settings: the store quantisation in bits 0-15 and
// the load quantisation in bits 16-31, each a type (0 = unquantised f32) plus a
// 6-bit scale exponent. 0x00000000 therefore means "plain float, no scaling"
// both ways, which is the case a specialised fast path would handle.
void StaticRecompCore::SampleGQRs()
{
  ++m_gqr_samples;
  for (int i = 0; i < 8; ++i)
    ++m_gqr_seen[i][m_guest.gqr[i]];
}

void StaticRecompCore::ReportGQRSurvey() const
{
  std::fprintf(stderr, "[gqr] %llu samples over the run\n",
               static_cast<unsigned long long>(m_gqr_samples));
  for (int i = 0; i < 8; ++i)
  {
    std::fprintf(stderr, "[gqr] GQR%d:", i);
    for (const auto& [value, count] : m_gqr_seen[i])
    {
      const double pct = m_gqr_samples ? 100.0 * double(count) / double(m_gqr_samples) : 0.0;
      std::fprintf(stderr, "  0x%08X (ld type=%u scale=%u, st type=%u scale=%u) %.1f%%", value,
                   (value >> 16) & 7u, (value >> 24) & 0x3Fu, value & 7u, (value >> 8) & 0x3Fu,
                   pct);
    }
    std::fprintf(stderr, "\n");
  }
  std::fflush(stderr);
}

void StaticRecompCore::Shutdown()
{
  if (m_gqr_log)
    ReportGQRSurvey();
  g_static_recomp_core = nullptr;
  std::fprintf(stderr,
               "[staticrecomp] shutdown: native=%llu fallback=%llu native_exc=%llu hook_fb=%llu "
               "smc_failed=%u verifications=%llu reverify_events=%llu\n",
               (unsigned long long)m_native_dispatches, (unsigned long long)m_fallback_steps,
               (unsigned long long)m_native_exceptions,
               (unsigned long long)m_hook_fallback_instructions, m_failed_chunks,
               (unsigned long long)m_verifications, (unsigned long long)m_reverify_events);
  NOTICE_LOG_FMT(POWERPC,
                 "StaticRecomp: shutdown. native_dispatches={} fallback_steps={} "
                 "native_exceptions={} hook_fallback_instructions={} smc_failed_chunks={} "
                 "verifications={} reverify_events={}",
                 m_native_dispatches, m_fallback_steps, m_native_exceptions,
                 m_hook_fallback_instructions, m_failed_chunks, m_verifications,
                 m_reverify_events);
  if (m_dispatch_loop || m_dispatch_fwd || m_dispatch_cross)
  {
    const double total =
        double(m_dispatch_loop) + double(m_dispatch_fwd) + double(m_dispatch_cross);
    std::fprintf(stderr,
                 "[dispatch] loop-backedge=%llu (%.1f%%) same-chunk-fwd=%llu (%.1f%%) "
                 "cross-chunk=%llu (%.1f%%)\n",
                 (unsigned long long)m_dispatch_loop, 100.0 * m_dispatch_loop / total,
                 (unsigned long long)m_dispatch_fwd, 100.0 * m_dispatch_fwd / total,
                 (unsigned long long)m_dispatch_cross, 100.0 * m_dispatch_cross / total);
  }
  m_lockstep_verifier.reset();
  m_block_cache.Shutdown();
  m_module = nullptr;
  m_fmv_pcs = {};
  m_layer_hooks = {};
  m_gate_armed = nullptr;
  m_gate_bypass = nullptr;
  m_gate_factor_addr = 0;
  m_gate_on = false;
  if (m_library.IsOpen())
    m_library.Close();

  if (m_fallback_jit)
  {
    m_fallback_jit->Shutdown();
    m_fallback_jit.reset();
    if (EMM::IsExceptionHandlerSupported())
      EMM::UninstallExceptionHandler();
  }
}

void StaticRecompCore::LoadModule()
{
  if (m_module_source.kind == StaticRecompModuleSource::Kind::None)
  {
    NOTICE_LOG_FMT(POWERPC, "StaticRecomp: no explicit module source; interpreter-only.");
    return;
  }

  const std::string game_id = SConfig::GetInstance().GetGameID();
  std::string path = m_module_source.path;
  const StaticRecompModuleDesc* desc = nullptr;
  if (m_module_source.kind == StaticRecompModuleSource::Kind::AttachedDescriptor)
  {
    desc = m_module_source.descriptor;
  }
  else
  {
    if (path.empty() || !File::Exists(path) || !m_library.Open(path.c_str()))
    {
      ERROR_LOG_FMT(POWERPC, "StaticRecomp: failed to open explicit module '{}'.", path);
      return;
    }
    const auto get_module = reinterpret_cast<StaticRecompGetModuleFn>(
        m_library.GetSymbolAddress(STATICRECOMP_GET_MODULE_SYMBOL));
    desc = get_module ? get_module() : nullptr;
  }

  const auto reject = [&](const std::string& why) {
    ERROR_LOG_FMT(POWERPC, "StaticRecomp: rejecting module '{}': {}. Interpreter-only.", path, why);
    m_module = nullptr;
    if (m_library.IsOpen())
      m_library.Close();
  };

  if (!desc)
    return reject("missing or null " STATICRECOMP_GET_MODULE_SYMBOL);
  // v2 is still accepted: it is the same struct minus the trailing optional
  // entry_points pair, so everything before that offset is laid out identically
  // and a v2 module keeps loading unchanged. Those two fields must not be read
  // for a v2 descriptor -- they are past the end of what it allocated.
  if (desc->abi_version != STATICRECOMP_ABI_VERSION && desc->abi_version != 2u)
    return reject(fmt::format("abi_version {} is neither {} nor 2", desc->abi_version,
                              STATICRECOMP_ABI_VERSION));
  if (desc->cpu_abi_version != GXRUNTIME_CPU_ABI_VERSION)
    return reject(fmt::format("cpu_abi_version {} != {}", desc->cpu_abi_version,
                              GXRUNTIME_CPU_ABI_VERSION));
  if (desc->cpu_state_size != sizeof(CPUState))
    return reject(fmt::format("cpu_state_size {} != sizeof(CPUState) {}", desc->cpu_state_size,
                              sizeof(CPUState)));
  if (!desc->dispatch || !desc->code_ranges || desc->num_code_ranges == 0)
    return reject("no dispatch entry or empty code ranges");
  if (!std::memchr(desc->game_id, '\0', sizeof(desc->game_id)) || desc->game_id[0] == '\0')
    return reject("invalid game_id");
  if (!RangesAreSorted(desc->code_ranges, desc->num_code_ranges))
    return reject("malformed or overlapping code ranges");
  if (desc->num_smc_ranges != 0 && !RangesAreSorted(desc->smc_ranges, desc->num_smc_ranges))
    return reject("malformed or overlapping SMC ranges");
  if (!desc->chunk_ranges || desc->num_chunk_ranges == 0 || !desc->chunk_hashes)
    return reject("no chunk ranges/hashes (required for the SMC guard)");
  if (!ChunksTileCode(*desc))
    return reject("chunk ranges do not exactly tile code ranges");
  if (!AddressIsCovered(desc->code_ranges, desc->num_code_ranges, desc->entry_point))
    return reject("entry point is not covered by the module");
  if (!game_id.empty() && game_id != desc->game_id)
    return reject(fmt::format("module game_id '{}' != running game '{}'", desc->game_id, game_id));

  m_module = desc;
  m_module_active = (desc != nullptr);
  {
    // STATICRECOMP_FMV_UNPLAYTESTED=1 also hooks discs whose row is mapped but
    // not yet playtested with the native movie player (none today).
    const bool unplaytested = std::getenv("STATICRECOMP_FMV_UNPLAYTESTED") != nullptr;
    m_fmv_pcs = StaticRecompFindFmvHooks(desc->game_id, unplaytested);
    NOTICE_LOG_FMT(POWERPC, "StaticRecomp: FMV hooks for {}: {}", desc->game_id,
                   m_fmv_pcs.start_afs != kStaticRecompNoFmvHook ? fmt::format("mwPlyStartAfs at {:#010x}",
                                                     m_fmv_pcs.start_afs) :
                                         std::string("none (the game decodes its own movies)"));
    m_layer_hooks = StaticRecompFindRenderLayerHooks(desc->game_id);
    // The gate (DolRecomp --gated-pc). Every current module exports it; only
    // one generated with the layer hooks gated tests it.
    m_gate_armed = static_cast<volatile u32*>(m_library.GetSymbolAddress("dolrecomp_gate_armed"));
    m_gate_bypass = static_cast<volatile u32*>(m_library.GetSymbolAddress("dolrecomp_gate_bypass"));
    m_gate_factor_addr = 0;
    m_gate_on = false;
    m_spawn_gate_on = false;
    m_spawn_watch = std::getenv("RINGOUT_SPAWN_WATCH") != nullptr;
    m_spawn_watch_lines = 0;
    if (const RecompScreenShape::Table* t = RecompScreenShape::FindTable(desc->game_id);
        t && m_layer_hooks.item_pc != kStaticRecompNoLayerHook && m_gate_armed && m_gate_bypass)
    {
      for (const RecompScreenShape::Constant& c : t->constants)
      {
        if (c.role == RecompScreenShape::Role::Factor)
        {
          m_gate_factor_addr = c.address;
          std::memcpy(&m_gate_factor_16x9, &c.original_bits, sizeof(float));
        }
      }
    }
    if (m_gate_armed)
      *m_gate_armed = 0u;
    if (m_gate_bypass)
      *m_gate_bypass = 0u;
  }
  {
    // Optional export; a module built before it existed has dispatcher timing.
    const auto* timing =
        static_cast<const u32*>(m_library.GetSymbolAddress("staticrecomp_timing_id"));
    m_timing_id = timing ? *timing : 1u;
  }
  {
    // Optional export (DolRecomp --dynamic-loop-budget): the budget the chunks were generated
    // with. Absent or 0 keeps the fixed budget. RINGOUT_DYN_BUDGET=<n> overrides it for A/Bs and
    // diagnosis (0 turns it off).
    const auto* budget =
        static_cast<const u32*>(m_library.GetSymbolAddress("staticrecomp_dyn_loop_budget"));
    m_dyn_budget = budget ? static_cast<s64>(*budget) : 0;
    if (const char* env = std::getenv("RINGOUT_DYN_BUDGET"))
      m_dyn_budget = std::max<s64>(0, std::strtoll(env, nullptr, 10));
    if (m_dyn_budget > 0)
      std::fprintf(stderr, "[staticrecomp] dynamic loop budget: %lld cycles\n",
                   static_cast<long long>(m_dyn_budget));
  }
  m_chunk_state.assign(desc->num_chunk_ranges, CHUNK_UNVERIFIED);
  m_failed_chunks = 0;
  m_lookup_ram_size = 0;
  m_lookup_exram_size = 0;
  m_chunk_lookup_table.clear();

  std::fprintf(stderr, "[staticrecomp] module loaded: %s entry=0x%08X\n", path.c_str(),
               desc->entry_point);
  NOTICE_LOG_FMT(POWERPC,
                 "StaticRecomp: loaded module '{}' (game_id={} entry=0x{:08X} "
                 "code_ranges={} smc_ranges={})",
                 path, desc->game_id, desc->entry_point, desc->num_code_ranges,
                 desc->num_smc_ranges);
}

void StaticRecompCore::ClearCache()
{
  // Jit64::ClearCache memsets its entire code space: measured 14-16 ms on every
  // call, and ~106 ms on the first. A savestate load calls this unconditionally
  // (JitInterface::DoState), which makes it ~70% of a rollback restore. Skipped
  // when the fallback has not run since the last clear, because then its cache
  // is already empty and the memset is pure cost. The flag is set again the
  // instant it runs, so a populated cache is still cleared exactly as before.
  if (m_fallback_jit && m_fallback_jit_used)
  {
    m_fallback_jit->ClearCache();
    m_fallback_jit_used = false;
  }

  if (!m_module)
    return;
  std::fill(m_chunk_state.begin(), m_chunk_state.end(), u8{CHUNK_UNVERIFIED});
  m_failed_chunks = 0;
  ++m_reverify_events;
}
