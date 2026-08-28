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
