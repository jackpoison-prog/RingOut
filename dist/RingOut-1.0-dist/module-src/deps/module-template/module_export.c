// RecompCore per-game native module export glue (game id set at build time).
//
// Wraps the DolRecomp-generated constant-time chunk dispatcher behind the
// StaticRecomp module ABI. All environment access goes through the CPUState
// hook pointers the chassis installs; this dylib has no host dependencies.

#include "generated.h"

#include "StaticRecompABI.h"

// NOT static, deliberately. LLVM keys an internal-linkage function in a PGO
// profile as "<source path>;<symbol>", using the path as the compiler received
// it -- so a static function's counts are recorded under the TRAINING machine's
// absolute path and match nothing on anyone else's, silently. External linkage
// makes the key the bare symbol, which is the same everywhere. Visibility is
// hidden module-wide, so nothing leaves the .so either way.
int chassis_dispatch(CPUState* ctx, u32 address);
int chassis_dispatch(CPUState* ctx, u32 address)
{
    const int result = dolrecomp_call(ctx, address);
    // Lazy FPRF: classification is deferred while native code runs, so
    // materialise it here -- this is the one path every return to the run loop
    // takes, and past it the chassis may export guest state to Dolphin
    // (SyncOut) for a savestate, a rollback or the interpreter. Keeping the
    // flush on this side means none of that machinery has to know.
    ppc_fprf_flush(ctx);
    return result;
}

void chassis_on_state_loaded(CPUState* ctx);
void chassis_on_state_loaded(CPUState* ctx)
{
    // The loaded FPSCR is authoritative; anything left pending belongs to the
    // session being discarded and would overwrite it at the next flush.
    ppc_fprf_drop();
    // Re-arm host FP rounding/flush state from the freshly loaded guest FPSCR.
    ppc_fpscr_updated(ctx);
}

#include "module_tables.inc"

static const StaticRecompModuleDesc s_desc = {
    STATICRECOMP_ABI_VERSION,
    GXRUNTIME_CPU_ABI_VERSION,
    (u32)sizeof(CPUState),
    MODULE_GAME_ID,
    DOLRECOMP_ENTRY_POINT,
    chassis_dispatch,
    chassis_on_state_loaded,
    s_code_ranges,
    MODULE_CODE_RANGE_COUNT,
    s_smc_ranges,
    MODULE_SMC_RANGE_COUNT,
    s_chunk_ranges,
    MODULE_CHUNK_RANGE_COUNT,
    s_chunk_hashes,
#ifdef MODULE_ENTRY_POINTS_NONE
    /* Full per-instruction entry switch: every address in a chunk range is an
       entry, so the chassis needs no table and keeps its range behaviour. */
    NULL,
    0u,
#else
    s_entry_points,
    MODULE_ENTRY_POINT_COUNT,
#endif
};

#if defined(_WIN32)
#define RECOMP_MODULE_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define RECOMP_MODULE_EXPORT __attribute__((visibility("default")))
#else
#define RECOMP_MODULE_EXPORT
#endif

RECOMP_MODULE_EXPORT const StaticRecompModuleDesc* staticrecomp_get_module(void)
{
    return &s_desc;
}

/* Guest-timing class of this module, read by the chassis for replay
   compatibility: 2 when it was generated with --direct-calls (cross-chunk calls
   stay native, so the dispatcher's per-dispatch timing work runs less often and
   guest timing differs), 1 otherwise. A replay recorded under one class does
   not reproduce under the other. Headers generated without the flag do not
   define it. */
#ifndef DOLRECOMP_TIMING_ID
#define DOLRECOMP_TIMING_ID 1u
#endif
RECOMP_MODULE_EXPORT const u32 staticrecomp_timing_id = DOLRECOMP_TIMING_ID;

/* The loop budget, exported only by a module generated with --dynamic-loop-budget
   (0 otherwise). Non-zero tells the chassis it may start each dispatch's cycle
   charge so a loop's `downcount <= -budget` exit fires at max(budget, cycles to
   the next CoreTiming event). The value must equal the budget the chunks were
   generated with: a smaller one would let a loop overshoot an event. */
#ifndef DOLRECOMP_DYN_LOOP_BUDGET
#define DOLRECOMP_DYN_LOOP_BUDGET 0u
#endif
RECOMP_MODULE_EXPORT const u32 staticrecomp_dyn_loop_budget = DOLRECOMP_DYN_LOOP_BUDGET;

/* The --gated-pc gate (DolRecomp emitter.c). The chassis sets armed while it
   wants the gated PCs to reach the dispatcher, and names a PC in bypass to let
   its own re-entry through. Defined in every module, used only by one
   generated with --gated-pc: the chassis finding these does not mean any PC is
   gated, and setting them is then harmless. */
RECOMP_MODULE_EXPORT volatile u32 dolrecomp_gate_armed = 0u;
RECOMP_MODULE_EXPORT volatile u32 dolrecomp_gate_bypass = 0u;

/* Which gate groups this module was generated with (bit g = group g has labels). The chassis
   arms a group only when its bit is here: Spawn's group 1 exists only in a module built with
   setup.sh --spawn. A module from before this export has none, and arms no group-1 work. */
#ifndef DOLRECOMP_GATE_GROUPS
#define DOLRECOMP_GATE_GROUPS 0u
#endif
RECOMP_MODULE_EXPORT const u32 staticrecomp_gate_groups = DOLRECOMP_GATE_GROUPS;
