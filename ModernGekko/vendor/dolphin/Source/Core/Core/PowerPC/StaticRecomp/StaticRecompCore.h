// RecompCore: StaticRecomp CPU core.
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/DynamicLibrary.h"
#include "Core/PowerPC/JitCommon/JitBase.h"
#include "Core/PowerPC/JitCommon/JitCache.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompABI.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompFmvHooks.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompRenderLayerHook.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompModuleSource.h"
#include "Core/RecompDeterminism.h"

namespace Core
{
class System;
}

namespace PowerPC
{
struct PowerPCState;
}

namespace StaticRecompLockstep
{
class StaticRecompLockstepVerifier;
}

// Executes statically recompiled per-game native code when the PC is covered by
// a loaded module; falls back to Dolphin's interpreter for everything else.
// With no module loaded this core is exactly an interpreter loop.
class StaticRecompCore : public JitBase
{
public:
  friend class StaticRecompLockstep::StaticRecompLockstepVerifier;

  explicit StaticRecompCore(Core::System& system, StaticRecompModuleSource module_source);
  StaticRecompCore(const StaticRecompCore&) = delete;
  StaticRecompCore(StaticRecompCore&&) = delete;
  StaticRecompCore& operator=(const StaticRecompCore&) = delete;
  StaticRecompCore& operator=(StaticRecompCore&&) = delete;
  ~StaticRecompCore() override;

  void Init() override;
  void Shutdown() override;

  void Run() override;
  void SingleStep() override;
  bool IsModuleActive() const;
  bool DispatchableAt(u32 address);
  bool FastDispatchableAt(u32 address) const;

  // One bit per guest instruction slot: "this address is an entry point of a
  // VERIFIED chunk", the exact question the dispatcher asks on every dispatch.
  // It is the AND of the three arrays FastDispatchableAt used to walk, folded
  // ahead of time because chunk state changes a couple of hundred times in a
  // session and the dispatcher asks tens of millions of times.
  //
  // SIZE IS THE POINT, not the instruction count. The three-array form touched
  // an 88 MB vector<int>, a bool bitmap and a state array on every dispatch, at
  // a random index; this is 2.75 MB, which mostly fits the Deck's 4 MB L3.
  std::vector<u64> m_fast_entry_bits;
  std::size_t m_fast_entry_count = 0;

  void RebuildFastEntryBits();
  void RebuildFastEntryChunk(u32 chunk_index);
  bool IsModuleEntry(u32 address) const;

  // The loaded module's guest-timing class (its optional staticrecomp_timing_id
  // export): 1 = cross-chunk calls return through this dispatcher, 2 = the
  // module calls across chunks natively (--direct-calls), which changes guest
  // timing. A replay recorded under one class does not reproduce under the
  // other. 0 = no module loaded.
  u32 TimingId() const { return m_module ? m_timing_id : 0u; }
  u32 m_timing_id = 1;

  void ClearCache() override;
  void Jit(u32 em_address) override {}
  bool HandleFault(uintptr_t access_address, SContext* ctx) override { return false; }

  JitBaseBlockCache* GetBlockCache() override { return &m_block_cache; }
  void EraseSingleBlock(const JitBlock& block) override {}
  std::vector<MemoryStats> GetMemoryStats() const override { return {}; }
  std::size_t DisassembleNearCode(const JitBlock& block, std::ostream& stream) const override
  {
    return 0;
  }
  std::size_t DisassembleFarCode(const JitBlock& block, std::ostream& stream) const override
  {
    return 0;
  }
  const CommonAsmRoutinesBase* GetAsmRoutines() override { return nullptr; }
  const char* GetName() const override { return "StaticRecomp"; }

private:
  // JitBaseBlockCache with no generated blocks; exists so generic
  // icache-invalidation plumbing in JitInterface has a real object to talk
  // to, and to feed every invalidation into the SMC demotion guard (D4).
  class EmptyBlockCache : public JitBaseBlockCache
  {
  public:
    explicit EmptyBlockCache(StaticRecompCore& core) : JitBaseBlockCache(core), m_core(core) {}
    void WriteLinkBlock(const JitBlock::LinkData& source, const JitBlock* dest) override {}

  protected:
    void InvalidateICacheInternal(u32 physical_address, u32 address, u32 length,
                                  bool forced) override
    {
      m_core.OnICacheInvalidate(address, length);
    }

  private:
    StaticRecompCore& m_core;
  };

  void LoadModule();

  // D4 SMC guard, verify-on-entry model. Every chunk starts Unverified; the
  // first native dispatch into it hashes its guest RAM against the module's
  // recorded hash of the original text. An icache invalidation touching a
  // chunk resets it to Unverified (Dolphin invalidates while *loading* code,
  // so invalidation alone must not retire coverage); a hash mismatch (real
  // SMC) marks it Failed, interpreter-only until the next invalidation.
  enum ChunkState : u8
  {
    CHUNK_UNVERIFIED = 0,
    CHUNK_VERIFIED = 1,
    CHUNK_FAILED = 2,
  };

  void OnICacheInvalidate(u32 address, u32 length);
  int ChunkIndexOf(u32 address) const;
  void VerifyChunk(u32 index);

  static void SetPPCStateFromGuestState(const CPUState& s, PowerPC::PowerPCState& ppc);

  // D1 state residency: registers live in m_guest while native code runs;
  // full sync at every native-burst boundary.
  void SyncIn();   // Dolphin PowerPCState -> m_guest
  void SyncOut();  // m_guest -> Dolphin PowerPCState

  // FMV HLE: called when the guest invokes mwPlyStartAfs (a Sofdec movie
  // start). fno is the AFS file number = which movie in movie.afs.
  void OnFmvStartAfs(u32 fno, u32 patid, u32 handle);
  void OnFmvExecObserve(u32 handle);
  void OnFmvGetFrm(u32 handle, u32 desc);
  void OnFmvCnvFrm(u32 handle, u32 desc, u32 dst);
  u32 GuestRead32(u32 addr) const;
  void GuestWrite32(u32 addr, u32 value);
  void OnRenderLayerHook();
  void PushLayerMarker(u32 value);

  // CPUState hooks (module -> chassis environment). `cpu->external_user_data`
  // is the StaticRecompCore*.
  static u64 HookExternalRead(CPUState* cpu, u32 ea, u8 size);
  static void HookExternalWrite(CPUState* cpu, u32 ea, u64 value, u8 size);
  static u32 HookExternalRead32(CPUState* cpu, u32 ea, u8 rid);
  static void HookExternalWrite32(CPUState* cpu, u32 ea, u32 value, u8 rid);
  static void* HookExternalPointer(CPUState* cpu, u32 ea, u32 size);
  static void HookInstructionFallback(CPUState* cpu, u32 raw, u32 cia);

  // Keep Dolphin's MSR-derived state (translation mode, feature flags) in step
  // with the guest MSR before any MMU access or exception delivery.
  void PropagateGuestMSR();

  // Determinism write watch (RINGOUT_DETERMINISM_WATCH). Installs the module's
  // write journal permanently and reports the dispatching block whenever a
  // store lands in the watched range, turning a diverging address into the
  // function that wrote it. Covers native stores only: a silent watch means the
  // write did not come from recompiled guest code, which is itself the answer.
  void InitDeterminismWatch();
  static void DeterminismWatchTrampoline(u32 offset, u32 size, void* user);

  // Polls the watched word and reports a change, naming where it was noticed.
  // The cached compare keeps this to a load and a branch, so it can sit on the
  // per-dispatch path; only an actual change calls out.
  void PollDeterminismWatch(const char* site, u32 pc)
  {
    const u32 value = GuestRead32(0x80000000u + m_watch_offset);
    if (value == m_watch_last) [[likely]]
      return;
    m_watch_last = value;
    RecompDeterminism::PollWatch(site, pc, value);
  }

  std::unique_ptr<StaticRecompLockstep::StaticRecompLockstepVerifier> m_lockstep_verifier;

  EmptyBlockCache m_block_cache{*this};

  CPUState m_guest{};
  // Optional module export: hands the module the write-gather pipe so GX stores
  // are serviced inline instead of crossing the .so boundary per word.
  using SetGatherPipeFn = void (*)(u8**, u8* const*, void (*)(void*), void*, const unsigned char*);
  void InstallGatherPipeFastPath();
  static void GatherPipeFlushTrampoline(void* user);

  Common::DynamicLibrary m_library;
  StaticRecompModuleSource m_module_source;
  const StaticRecompModuleDesc* m_module = nullptr;
  bool m_module_active = false;
  // The FMV HLE hook PCs for m_module's disc; all kStaticRecompNoFmvHook (never
  // fire) for a disc with no mapped, enabled row. Set by LoadModule.
  StaticRecompFmvHookPcs m_fmv_pcs;
  // The scene-effect hooks for m_module's disc (all unset without a row), and
  // the run loop's view of them: the layer last drawn and the last marker sent.
  StaticRecompRenderLayerHooks m_layer_hooks;
  u32 m_render_layer = 0;
  bool m_scene_effect_marked = false;
  // Those hooks as DolRecomp --gated-pc (module_export.c): the module's gate,
  // armed while the screen shape in guest RAM -- its 3D aspect constant, at
  // m_gate_factor_addr -- is wider than 16:9, the only time the hooks are
  // needed. Read from RAM, not from the settings, so two netplay peers or a
  // replay arm it identically: armed, the gated PCs dispatch, and that moves
  // guest timing. Null without the exports or without a shape table.
  volatile u32* m_gate_armed = nullptr;
  volatile u32* m_gate_bypass = nullptr;
  u32 m_gate_factor_addr = 0;
  float m_gate_factor_16x9 = 0.0f;
  bool m_gate_on = false;
  // Gate group 1 is armed: Spawn is on and his tables are patched (RecompGuest::SitesArmed).
  bool m_spawn_gate_on = false;
  u32 m_spawn_sites_seen = 0;  // sites logged once each
  u32 m_spawn_hooks_seen = 0;  // character-code hooks logged once each
  // RINGOUT_SPAWN_WATCH=1: arm his sites for every fighter and log the first 64 arrivals (which
  // fighter reached which site), to show a route exercises them.
  bool m_spawn_watch = false;
  u32 m_spawn_watch_lines = 0;
  std::unique_ptr<JitBase> m_fallback_jit;
  // Whether the fallback JIT has run since its cache was last cleared. See
  // ClearCache: clearing an already-empty Jit64 code space still costs 14-16 ms.
  bool m_fallback_jit_used = false;

  u64 m_native_dispatches = 0;
  // Dispatch classification (STATICRECOMP_DISPATCHLOG), reported at shutdown.
  u64 m_dispatch_loop = 0;   // same chunk, backward target: a loop back-edge
  u64 m_dispatch_fwd = 0;    // same chunk, forward target
  u64 m_dispatch_cross = 0;  // different chunk (calls, returns, far branches)
  u64 m_fallback_steps = 0;
  u64 m_native_exceptions = 0;
  u64 m_hook_fallback_instructions = 0;
  u64 m_bursts = 0;          // SyncIn..SyncOut native runs (diagnostic)
  u64 m_charged_cycles = 0;  // cycles flushed from module charges (diagnostic)
  // Carried CPU-cycle remainder for advancing the guest Time Base at the
  // hardware rate (CPU clock / TIMER_RATIO). Reset each SyncIn.
  u64 m_tb_cycle_remainder = 0;
  // Dynamic loop budget: when the module exports its loop budget (staticrecomp_dyn_loop_budget,
  // DolRecomp --dynamic-loop-budget), each dispatch's charge accumulator starts at
  // max(0, cycles to the next CoreTiming event - budget), so a native loop's
  // `downcount <= -budget` exit fires at max(budget, cycles to the event) rather than always at
  // the budget. 0 (an older module, or RINGOUT_DYN_BUDGET=0) keeps the accumulator at 0, the old
  // behaviour. m_dc_bias is the start value, so the cycles charged are m_dc_bias - downcount.
  s64 m_dc_bias = 0;
  s64 m_dyn_budget = 0;
  void ArmCharge(int ppc_downcount)
  {
    m_dc_bias = (m_dyn_budget > 0 && ppc_downcount > m_dyn_budget) ? ppc_downcount - m_dyn_budget : 0;
    m_guest.downcount = m_dc_bias;
  }

  // D4 guard state: parallel to m_module->chunk_ranges.
  std::vector<u8> m_chunk_state;
  u32 m_failed_chunks = 0;    // chunks currently failing verification (real SMC)
  u64 m_verifications = 0;    // chunk hash checks performed
  u64 m_reverify_events = 0;  // invalidations that reset a chunk to Unverified

  // Lookup table optimization for O(1) chunk searches
  std::vector<int> m_chunk_lookup_table;
  // Entry points, indexed like m_chunk_lookup_table. Empty when the module does
  // not declare any (ABI v2, or a v3 module with a full per-instruction entry
  // switch), and then every address inside a chunk range is entrable as before.
  // Bit-packed on purpose: one bit per guest instruction over RAM+EXRAM is
  // ~2.75 MB, where a byte each would be 22 MB.
  std::vector<bool> m_entry_bitmap;
  u32 m_lookup_ram_size = 0;
  u32 m_lookup_exram_size = 0;
  int GetAddressLookupIndex(u32 address) const;
  void InitLookupTable(u32 ram_size, u32 exram_size);

  // Dispatch locality: most control transfers stay inside one chunk, so the
  // last hit short-circuits the chunk binary search on the hot path.
  mutable u32 m_last_chunk_index = 0;

  u32 m_idle_pc = 0;

  // GQR quantisation survey (STATICRECOMP_GQRLOG=1). Records which values the
  // eight GQRs actually hold over a run, sampled per burst -- they change rarely
  // and a map lookup has no business on the per-dispatch path.
  //
  // READ THE OUTPUT CAREFULLY: this is what the registers CONTAIN, not how often
  // each setting is USED. The two diverge badly here, and the difference already
  // sent one optimisation down the wrong path. This survey reports GQR0 as
  // 0x00000000 for 100% of samples, and 93% of the 4555 psq call sites name
  // GQR0 -- from which a `gqr == 0` fast path looks obviously right. Counting
  // actual executions inside the module said otherwise: gqr==0 is 49.5% of
  // executed loads and only 15.3% of stores, because GQR4 is 3.5% of sites but
  // 58.7% of store executions. A register that is usually zero is not the same
  // as a hot path that is usually zero.
  //
  // So this answers "what settings does the game use", cheaply and without a
  // module rebuild. For "which settings does the hot code hit", instrument
  // ppc_psq_load/store directly instead; see work/perf-fmv/psq_dynamic_counts.log.
  void SampleGQRs();
  void ReportGQRSurvey() const;
  std::map<u32, u64> m_gqr_seen[8];
  u64 m_gqr_samples = 0;
  bool m_gqr_log = false;

  // Determinism write watch state. m_watch_block_* is refreshed before each
  // native dispatch so the journal callback -- which is handed nothing but an
  // offset -- can attribute the store to a block.
  bool m_watch_armed = false;
  u32 m_watch_offset = 0;  // RAM byte offset of the watched range
  u32 m_watch_size = 0;
  u32 m_watch_block_pc = 0;
  u32 m_watch_block_lr = 0;
  u32 m_watch_last = 0;  // last polled value, so the poll costs a compare
};

extern StaticRecompCore* g_static_recomp_core;

// True when a module is loaded/active and the given guest PC is covered by it.
// Used by the fallback JIT dispatcher to hand covered PCs back to the module
// instead of JIT-compiling them. Safe to call with no core/module (returns false).
bool StaticRecompCoveredAt(u32 pc);
