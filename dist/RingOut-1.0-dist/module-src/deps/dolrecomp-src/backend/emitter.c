// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ExpansionPak

#include "emitter.h"

/* Defined below, next to the refund state it reads. */
static void emit_exc_check_return(FILE* out, const char* indent);

#include <stdlib.h>
#include <string.h>

/* MSVC's C mode does not implement C11 atomics: <stdatomic.h> raises C1189
 * "C atomic support is not enabled" even under /std:c11, because Microsoft
 * supports <stdatomic.h> only when compiling as C++. So the counters below use
 * the Win32 Interlocked intrinsics under _MSC_VER -- the same shape cpu.c
 * already uses for ppc_memory_fence() -- and C11 atomics everywhere else.
 *
 * Diagnosed by danny199012 on the community Windows fork (danny199012/RingOut
 * a4bd759d), who established that /std:c11 cannot fix it after two attempts
 * that assumed it could. This follows their fix.
 *
 * These are --ca-liveness statistics counters, not a hot path: emit_function()
 * runs on the -jN workers, so they must be atomic, but nothing reads them per
 * instruction. */
#if defined(_MSC_VER)
#include <intrin.h>
typedef volatile __int64 dr_stat_ctr;
#define DR_STAT_LOAD(p) ((unsigned long long)_InterlockedCompareExchange64((p), 0, 0))
#define DR_STAT_INC(p)  ((void)_InterlockedExchangeAdd64((p), 1))
#define DR_THREAD_LOCAL __declspec(thread)
#else
#include <stdatomic.h>
typedef _Atomic unsigned long long dr_stat_ctr;
#define DR_STAT_LOAD(p) atomic_load(p)
#define DR_STAT_INC(p)  atomic_fetch_add((p), 1ull)
#define DR_THREAD_LOCAL _Thread_local
#endif

/* --ca-liveness: XER[CA] dead-write analysis. Reporting only for now -- see
 * compute_ca_live(). emit_function() runs on the -jN workers, so the counters
 * accumulate atomically rather than racing like the entry-point list would. */
static bool s_ca_liveness = false;
/* Opt-in ELISION, deliberately separate from --ca-liveness: that flag's --help
   promises codegen is unchanged, and test_emitter_flags asserts it.
 *
 * MEASURED 2026-08-17 AND IT IS UNSOUND. DO NOT SHIP A MODULE BUILT WITH THIS.
 * It elides 1117 of 3769 static sites (29%) and the resulting module DIVERGES:
 * frame 465 of arcade-match.txt on the Deck, 4 frames of 4000, and only in the
 * heap CRC -- OS globals and L1 match, so it is a real difference in game state,
 * not a register artefact. It diverges and re-converges, which is the worst
 * possible signature: no crash, nothing visibly wrong, and netplay peers desync.
 *
 * The likely hole is exceptions: ca_escapes() covers calls, returns, indirect
 * branches, sc/rfi and non-local branches, but ANY load or store can fault into
 * a handler that saves and restores XER, and that handler would see a stale CA.
 * A sound analysis must treat those as escapes, which can only shrink the 29%.
 *
 * And the ceiling does not justify the work: the unsound module measured
 * 0.4-0.7% faster on the Deck (123.61s -> 122.80s over 10000 fixed frames), so
 * a CORRECT version is worth less than that. Kept behind a flag because the
 * wiring is done and the analysis is here if anyone fixes it. */
static bool s_ca_elide = false;
/* Verdict for the site currently being emitted. _Thread_local because
   emit_function() runs on the -jN workers -- a plain file-static would be a
   data race across chunks, the trap record_entry() documents. */
static DR_THREAD_LOCAL int s_ca_dead_here = 0;
static dr_stat_ctr s_ca_defs;
static dr_stat_ctr s_ca_dead;
static dr_stat_ctr s_ca_uses;

void emit_set_ca_liveness(bool enable) { s_ca_liveness = enable; }
void emit_set_ca_elide(bool enable) { s_ca_elide = enable; }

void emit_report_ca_stats(void) {
    unsigned long long defs, dead, uses;
    if (!s_ca_liveness)
        return;
    defs = DR_STAT_LOAD(&s_ca_defs);
    dead = DR_STAT_LOAD(&s_ca_dead);
    uses = DR_STAT_LOAD(&s_ca_uses);
    printf("  XER[CA]: %llu write sites, %llu read sites, %llu provably dead"
           " (%.1f%% of writes)\n",
           defs, uses, dead,
           defs ? 100.0 * (double)dead / (double)defs : 0.0);
}

static u32 cr_field_shift(u8 crf) {
    return 4u * (7u - (u32)crf);
}

static u32 ppc_mask32(u8 mb, u8 me) {
    u32 mask = 0;
    u8 bit = mb;

    for (;;) {
        mask |= 0x80000000u >> bit;
        if (bit == me)
            break;
        bit = (u8)((bit + 1) & 31);
    }

    return mask;
}

static void emit_set_cr0_from_gpr(FILE* out, u8 reg) {
    fprintf(out, "        u32 cr_bits = 0;\n");
    fprintf(out, "        s32 cr_value = (s32)ctx->gpr[%u];\n", reg);
    fprintf(out, "        if (cr_value < 0)  cr_bits |= 0x8u;\n");
    fprintf(out, "        if (cr_value > 0)  cr_bits |= 0x4u;\n");
    fprintf(out, "        if (cr_value == 0) cr_bits |= 0x2u;\n");
    fprintf(out, "        cr_bits |= (ctx->xer >> 31) & 1u;\n");
    fprintf(out, "        ctx->crf[0] = (u8)cr_bits;\n");
    fprintf(out, "        PPC_CR_WRITE(0);\n");
}

static void emit_set_cr1_from_fpscr(FILE* out) {
    fprintf(out, "        ctx->crf[1] = (u8)(ctx->fpscr >> 28);\n");
    fprintf(out, "        PPC_CR_WRITE(1);\n");
}

static void emit_compare_s32(FILE* out, u8 crf, const char* lhs, const char* rhs) {
    fprintf(out, "    {\n");
    fprintf(out, "        s32 val_a = (s32)(%s);\n", lhs);
    fprintf(out, "        s32 val_b = (s32)(%s);\n", rhs);
    fprintf(out, "        u32 cr_bits = 0;\n");
    fprintf(out, "        if (val_a < val_b)  cr_bits |= 0x8u;\n");
    fprintf(out, "        if (val_a > val_b)  cr_bits |= 0x4u;\n");
    fprintf(out, "        if (val_a == val_b) cr_bits |= 0x2u;\n");
    fprintf(out, "        cr_bits |= (ctx->xer >> 31) & 1u;\n");
    fprintf(out, "        ctx->crf[%u] = (u8)cr_bits;\n", (u32)crf);
    fprintf(out, "        PPC_CR_WRITE(%u);\n", (u32)crf);
    fprintf(out, "    }\n");
}

static void emit_compare_u32(FILE* out, u8 crf, const char* lhs, const char* rhs) {
    fprintf(out, "    {\n");
    fprintf(out, "        u32 val_a = (u32)(%s);\n", lhs);
    fprintf(out, "        u32 val_b = (u32)(%s);\n", rhs);
    fprintf(out, "        u32 cr_bits = 0;\n");
    fprintf(out, "        if (val_a < val_b)  cr_bits |= 0x8u;\n");
    fprintf(out, "        if (val_a > val_b)  cr_bits |= 0x4u;\n");
    fprintf(out, "        if (val_a == val_b) cr_bits |= 0x2u;\n");
    fprintf(out, "        cr_bits |= (ctx->xer >> 31) & 1u;\n");
    fprintf(out, "        ctx->crf[%u] = (u8)cr_bits;\n", (u32)crf);
    fprintf(out, "        PPC_CR_WRITE(%u);\n", (u32)crf);
    fprintf(out, "    }\n");
}

static void emit_fcompare(FILE* out, const PPCInst* inst) {
    fprintf(out, "    {\n");
    fprintf(out, "        f64 val_a = ctx->fpr[%u];\n", inst->rA);
    fprintf(out, "        f64 val_b = ctx->fpr[%u];\n", inst->rB);
    fprintf(out, "        u32 cr_bits = 0;\n");
    fprintf(out, "        if (val_a < val_b)       cr_bits = 0x8u;\n");
    fprintf(out, "        else if (val_a > val_b)  cr_bits = 0x4u;\n");
    fprintf(out, "        else if (val_a == val_b) cr_bits = 0x2u;\n");
    fprintf(out, "        else                     cr_bits = 0x1u;\n");
    fprintf(out, "        ctx->crf[%u] = (u8)cr_bits;\n", (u32)inst->crfD);
    fprintf(out, "        PPC_CR_WRITE(%u);\n", (u32)inst->crfD);
    /* A FLOAT compare also sets FPSCR's FPCC (bits 12-15); nothing was doing
     * that, so lockstep saw the interpreter carrying compare results in FPSCR
     * that we did not. Cleared then set, per the architecture: "the FPCC field
     * is set to reflect the comparison". Integer compares must NOT touch FPSCR,
     * which is why this lives here and not in emit_compare_s32.
     *
     * This deliberately does NOT match Dolphin's interpreter, which does
     *   fpscr.FPRF = (fpscr.FPRF & ~FPCC_MASK) | compare_value;
     * where FPRF is a 5-bit BitField holding 0..31 while FPCC_MASK is 0xF << 12,
     * so the mask clears nothing and the result is OR-ed into the old value.
     * Copying that would leave stale FPCC bits the hardware clears, and the game
     * reads FPSCR (two mffs sites). Lockstep will keep reporting these; the
     * divergence is on the reference side. */
    /* FPCC occupies FPSCR bits 12-15, which sits INSIDE the 0x1F<<12 FPRF
     * mask, so a deferred FPRF flush arriving after this would wipe the
     * compare result. Land the pending value first, exactly as the eager path
     * ordered it. Found by tracing the last FPRF writer before each mffs:
     * both builds named the same writer (0x80117330) yet disagreed on the
     * value, which is only possible if something untagged wrote the range. */
    fprintf(out, "        ppc_fprf_flush(ctx);\n");
    fprintf(out, "        ctx->fpscr = (ctx->fpscr & ~(0xFu << 12)) | (cr_bits << 12);\n");
    fprintf(out, "    }\n");
}

/* --ram-bases: base registers whose D-form accesses can only reach main RAM
 * (the stack pointer, the small-data bases). Such a site opens its block with
 * `enum { dr_ram_site = 1 };`, which cpu.h turns into an unchecked access at
 * parse time; every other site is emitted exactly as before. */
static u32 s_ram_bases = 0;

/* --preserve-none: every chunk function (definition, prototype, the dispatch
 * typedef and each local declaration) carries DOLRECOMP_CHUNK_FN, which cpu.h
 * turns into __attribute__((preserve_none)) where the compiler has it. A chunk
 * then saves none of the callee-saved registers it uses: on the Deck profile
 * each of ~1.6 G chunk calls per US run pushed and popped six. All of them or
 * none -- a call through a mismatched declaration corrupts registers. */
static bool s_preserve_none = false;
void emit_set_preserve_none(bool enable) { s_preserve_none = enable; }
bool emit_preserve_none_enabled(void) { return s_preserve_none; }
const char* emit_chunk_cc(void) { return s_preserve_none ? "DOLRECOMP_CHUNK_FN " : ""; }

/* --fp-check-once: every FP instruction tests MSR.FP (ppc_fp_available) --
 * 175,848 sites on the US disc, 1.43% of a gameplay profile -- but MSR only
 * changes at mtmsr, or across something that ends the block (sc, rfi, a call,
 * a fallback, an exception, which all leave the chunk or reach a leader). So
 * within a straight-line run, once one FP instruction has passed the test the
 * rest cannot fail it.
 *
 * Every FP instruction is also a switch case (is_extra_entry: after an
 * FP-unavailable exception the OS resumes AT it), so the test cannot simply be
 * dropped: label_X keeps it for every entry, and straight-line code that has
 * already tested jumps past it to fpok_X. Known-enabled resets at each leader,
 * at every other entry (a mid-block switch case), and after mtmsr. */
static bool s_fp_check_once = false;
static DR_THREAD_LOCAL bool s_fp_ok = false;          /* MSR.FP tested in this run */
static DR_THREAD_LOCAL u32 s_fp_skip_label = 0;       /* emit fpok_<pc> after the test */
void emit_set_fp_check_once(bool enable) { s_fp_check_once = enable; }
void emit_set_ram_bases(u32 mask) { s_ram_bases = mask; }

static void emit_ea_open(FILE* out, u8 ra, bool update) {
    if ((ra != 0 || update) && (s_ram_bases >> ra & 1u))
        fprintf(out, "        enum { dr_ram_site = 1 };\n");
    fprintf(out, "        u32 ea = ");
}

static void emit_dform_ea(FILE* out, u8 ra, s16 simm, bool update) {
    if (ra == 0 && !update) {
        fprintf(out, "(u32)(s32)(%d)", (int)simm);
    } else {
        fprintf(out, "ctx->gpr[%u] + (u32)(s32)(%d)", ra, (int)simm);
    }
}

static void emit_xform_ea(FILE* out, u8 ra, u8 rb, bool update) {
    if (ra == 0 && !update) {
        fprintf(out, "ctx->gpr[%u]", rb);
    } else {
        fprintf(out, "ctx->gpr[%u] + ctx->gpr[%u]", ra, rb);
    }
}

static void emit_load(FILE* out, const PPCInst* inst, const char* read_expr,
                      bool update) {
    fprintf(out, "    {\n");
    emit_ea_open(out, inst->rA, update);
    emit_dform_ea(out, inst->rA, inst->simm, update);
    fprintf(out, ";\n");
    fprintf(out, "        ctx->gpr[%u] = %s;\n", inst->rD, read_expr);
    if (update) {
        fprintf(out, "        ctx->gpr[%u] = ea;\n", inst->rA);
    }
    fprintf(out, "    }\n");
}

static void emit_loadx(FILE* out, const PPCInst* inst, const char* read_expr,
                       bool update) {
    fprintf(out, "    {\n");
    fprintf(out, "        u32 ea = ");
    emit_xform_ea(out, inst->rA, inst->rB, update);
    fprintf(out, ";\n");
    fprintf(out, "        ctx->gpr[%u] = %s;\n", inst->rD, read_expr);
    if (update) {
        fprintf(out, "        ctx->gpr[%u] = ea;\n", inst->rA);
    }
    fprintf(out, "    }\n");
}

static void emit_store(FILE* out, const PPCInst* inst, const char* write_func,
                       const char* cast_type, bool update) {
    fprintf(out, "    {\n");
    emit_ea_open(out, inst->rA, update);
    emit_dform_ea(out, inst->rA, inst->simm, update);
    fprintf(out, ";\n");
    fprintf(out, "        %s(ctx, ea, (%s)ctx->gpr[%u]);\n",
            write_func, cast_type, inst->rS);
    if (update) {
        fprintf(out, "        ctx->gpr[%u] = ea;\n", inst->rA);
    }
    fprintf(out, "    }\n");
}

static void emit_storex(FILE* out, const PPCInst* inst, const char* write_func,
                        const char* cast_type, bool update) {
    fprintf(out, "    {\n");
    fprintf(out, "        u32 ea = ");
    emit_xform_ea(out, inst->rA, inst->rB, update);
    fprintf(out, ";\n");
    fprintf(out, "        %s(ctx, ea, (%s)ctx->gpr[%u]);\n",
            write_func, cast_type, inst->rS);
    if (update) {
        fprintf(out, "        ctx->gpr[%u] = ea;\n", inst->rA);
    }
    fprintf(out, "    }\n");
}

static void emit_fload(FILE* out, const PPCInst* inst, bool single,
                       bool update) {
    fprintf(out, "    {\n");
    emit_ea_open(out, inst->rA, update);
    emit_dform_ea(out, inst->rA, inst->simm, update);
    fprintf(out, ";\n");
    if (single) {
        fprintf(out, "        f64 value = (f64)dolrecomp_f32_from_bits(mem_read32(ctx, ea));\n");
        fprintf(out, "        ctx->fpr[%u] = value;\n", inst->rD);
        fprintf(out, "        ctx->ps1[%u] = value;\n", inst->rD);
    } else {
        fprintf(out, "        ctx->fpr[%u] = dolrecomp_f64_from_bits(mem_read64(ctx, ea));\n",
                inst->rD);
    }
    if (update) {
        fprintf(out, "        ctx->gpr[%u] = ea;\n", inst->rA);
    }
    fprintf(out, "    }\n");
}

static void emit_floadx(FILE* out, const PPCInst* inst, bool single,
                        bool update) {
    fprintf(out, "    {\n");
    fprintf(out, "        u32 ea = ");
    emit_xform_ea(out, inst->rA, inst->rB, update);
    fprintf(out, ";\n");
    if (single) {
        fprintf(out, "        f64 value = (f64)dolrecomp_f32_from_bits(mem_read32(ctx, ea));\n");
        fprintf(out, "        ctx->fpr[%u] = value;\n", inst->rD);
        fprintf(out, "        ctx->ps1[%u] = value;\n", inst->rD);
    } else {
        fprintf(out, "        ctx->fpr[%u] = dolrecomp_f64_from_bits(mem_read64(ctx, ea));\n",
                inst->rD);
    }
    if (update) {
        fprintf(out, "        ctx->gpr[%u] = ea;\n", inst->rA);
    }
    fprintf(out, "    }\n");
}

static void emit_fstore(FILE* out, const PPCInst* inst, bool single,
                        bool update) {
    fprintf(out, "    {\n");
    emit_ea_open(out, inst->rA, update);
    emit_dform_ea(out, inst->rA, inst->simm, update);
    fprintf(out, ";\n");
    if (single) {
        fprintf(out, "        mem_write32(ctx, ea, dolrecomp_store_single(ctx->fpr[%u]));\n",
                inst->rS);
    } else {
        fprintf(out, "        mem_write64(ctx, ea, dolrecomp_f64_to_bits(ctx->fpr[%u]));\n",
                inst->rS);
    }
    if (update) {
        fprintf(out, "        ctx->gpr[%u] = ea;\n", inst->rA);
    }
    fprintf(out, "    }\n");
}

static void emit_fstorex(FILE* out, const PPCInst* inst, bool single,
                         bool update) {
    fprintf(out, "    {\n");
    fprintf(out, "        u32 ea = ");
    emit_xform_ea(out, inst->rA, inst->rB, update);
    fprintf(out, ";\n");
    if (single) {
        fprintf(out, "        mem_write32(ctx, ea, dolrecomp_store_single(ctx->fpr[%u]));\n",
                inst->rS);
    } else {
        fprintf(out, "        mem_write64(ctx, ea, dolrecomp_f64_to_bits(ctx->fpr[%u]));\n",
                inst->rS);
    }
    if (update) {
        fprintf(out, "        ctx->gpr[%u] = ea;\n", inst->rA);
    }
    fprintf(out, "    }\n");
}

static void emit_psq_load(FILE* out, const PPCInst* inst, bool indexed,
                          bool update) {
    fprintf(out, "    {\n");
    fprintf(out, "        u32 ea = ");
    if (indexed) {
        emit_xform_ea(out, inst->rA, inst->rB, update);
    } else {
        emit_dform_ea(out, inst->rA, inst->simm, update);
    }
    fprintf(out, ";\n");
    fprintf(out, "        ppc_psq_load(ctx, %uu, ea, %s, %uu, %s, 0x%08Xu);\n",
            inst->rD, inst->w ? "true" : "false", inst->i,
            indexed ? "true" : "false", inst->address);
    emit_exc_check_return(out, "        ");
    if (update) {
        fprintf(out, "        ctx->gpr[%u] = ea;\n", inst->rA);
    }
    fprintf(out, "    }\n");
}

static void emit_psq_store(FILE* out, const PPCInst* inst, bool indexed,
                           bool update) {
    fprintf(out, "    {\n");
    fprintf(out, "        u32 ea = ");
    if (indexed) {
        emit_xform_ea(out, inst->rA, inst->rB, update);
    } else {
        emit_dform_ea(out, inst->rA, inst->simm, update);
    }
    fprintf(out, ";\n");
    fprintf(out, "        ppc_psq_store(ctx, %uu, ea, %s, %uu, %s, 0x%08Xu);\n",
            inst->rS, inst->w ? "true" : "false", inst->i,
            indexed ? "true" : "false", inst->address);
    emit_exc_check_return(out, "        ");
    if (update) {
        fprintf(out, "        ctx->gpr[%u] = ea;\n", inst->rA);
    }
    fprintf(out, "    }\n");
}

static void emit_dcbz(FILE* out, const PPCInst* inst) {
    fprintf(out, "    {\n");
    fprintf(out, "        u32 ea = ");
    emit_xform_ea(out, inst->rA, inst->rB, false);
    fprintf(out, ";\n");
    fprintf(out, "        ea &= ~31u;\n");
    fprintf(out, "        for (u32 i = 0; i < 32; i += 4) mem_write32(ctx, ea + i, 0);\n");
    fprintf(out, "    }\n");
}

static void emit_branch_condition(FILE* out, u8 bo, u8 bi) {
    bool ctr_ignored = (bo & 0x04) != 0;
    bool cond_ignored = (bo & 0x10) != 0;

    if (!ctr_ignored) {
        fprintf(out, "        ctx->ctr--;\n");
        fprintf(out, "        bool ctr_ok = (((ctx->ctr != 0) ? 1u : 0u) ^ %uu) != 0;\n",
                (bo >> 1) & 1u);
    } else {
        fprintf(out, "        bool ctr_ok = true;\n");
    }

    if (!cond_ignored) {
        fprintf(out, "        PPC_CR_READ(%uu);\n", 1u << (bi >> 2));
        fprintf(out, "        bool cr_ok = (((ctx->crf[%u] & 0x%Xu) != 0) == %s);\n",
                (u32)(bi >> 2), 8u >> (bi & 3u), ((bo >> 3) & 1u) ? "true" : "false");
    } else {
        fprintf(out, "        bool cr_ok = true;\n");
    }
}

/* Cycles a chunk may run natively across loop back-edges before handing control
 * back. Sized around the old per-dispatch cadence: the chassis used to regain
 * control at least once per block, and a block is a few cycles, so this is a
 * deliberate relaxation -- large enough that a hot loop stops paying dispatcher
 * overhead per iteration, small enough that interrupt latency stays in the same
 * ballpark as a CachedInterpreter block. */
#define DOLRECOMP_LOOP_BUDGET 1000

/* Back-edges to this address keep returning to the dispatcher. The chassis
 * recognises the guest's OS idle spin loop by PC and calls CoreTiming::Idle()
 * to fast-forward to the next event instead of burning real time spinning --
 * worth roughly 2x on this game. A native loop never surfaces its PC, so the
 * skip would silently stop happening: measured, the idle PC really is a
 * back-edge target here. 0 = no idle PC, every back-edge stays native. */
static u32 s_idle_pc = 0;

/* Guest PCs that must always be reached through the chassis dispatcher.
 *
 * Call chaining (below) turns a local `bl` into a native goto, which is a win
 * -- but the run loop recognises certain guest functions BY PC between
 * dispatches (the FMV HLE entry points). A call that never returns to the
 * dispatcher is a hook that never fires, so those targets are excluded by
 * address rather than by hoping they are always reached indirectly. */
/* Off by default: measured at 0.75% on the 600-frame harness -- inside the
 * noise, where native loops showed a plain -11.5% on the same instrument. Only
 * 8% of `bl` sites are local and chainable, and the matching `blr` is indirect
 * and still dispatches, so this halves the dispatches for a minority of calls
 * rather than removing a class of them. Kept because the measurement is cheap
 * to repeat on a gameplay workload, which is the case it was never tested on. */
static bool s_chain_calls = false;

void emit_set_chain_calls(bool enable) {
    s_chain_calls = enable;
}

/* --leader-cases: emit the chunk-entry switch only where control can arrive.
 *
 * One case per guest instruction makes every instruction a switch-reachable
 * join point, and the compiler generates worse code for the whole body around
 * them. Measured on the no-PGO US module, VS stage-10 fight, 14000 frames, 3
 * alternating reps: -7.96% cycles, -2.97% instructions, IPC 2.75 -> 2.90 --
 * the win is mostly tighter code, not less of it.
 *
 * THE ENTRY RULE. Block leaders are not enough; the entry set is leaders UNION:
 *  - every instruction with an FP-availability guard. The guard refunds the
 *    rest of the block and returns; the OS enables the FPU lazily and rfi's
 *    back to that same instruction. Without a case the chassis interprets up to
 *    the next entry and CHARGES those instructions, where native re-entry
 *    charged nothing, so event timing shifts: 685 heap-only frame diffs.
 *  - every direct branch target in the program (s_entry_targets). Leaders are
 *    computed per chunk, so a target reached only from another chunk is not a
 *    leader in its own.
 *  - every data word pointing into code (also s_entry_targets): switch jump
 *    tables and code pointers.
 * Leaders alone reproduced the old 2026-08-07 failure mode; this set is
 * frame-hash identical to the full switch on two routes (VS fight 14000
 * frames, arcade match 16000), with no runtime observation as input. It keeps
 * 181997 of 537792 cases on the US disc.
 *
 * Blocks are NOT split at the extra entries: leader[] still decides the pc
 * store and downcount charge, entry[] only decides the switch. Merging them
 * would move cycle charges and change guest timing. */
static bool s_leader_cases = false;

static u32* s_entry_targets = NULL;
static u32 s_entry_target_count = 0;

/* Addresses the entry switch actually has a case for, accumulated across every
 * emitted function. Only collected when the entry set is REDUCED: with the full
 * per-instruction switch every address in a chunk is an entry, which is the
 * assumption the chassis makes when no table is supplied. */
static u32* s_entries = NULL;
static u32 s_entry_count = 0;
static u32 s_entry_cap = 0;

/* MAIN THREAD ONLY. emit_function() runs on the -jN workers, so appending to a
 * shared list from there is a data race -- it corrupted the heap and aborted the
 * first time. The pipeline knows each chunk's instructions before it queues the
 * job, so the entry set is collected there instead, off the workers entirely. */
static void record_entry(u32 address) {
    if (!s_leader_cases)
        return;
    if (s_entry_count == s_entry_cap) {
        u32 cap = s_entry_cap ? s_entry_cap * 2u : 4096u;
        u32* grown = (u32*)realloc(s_entries, cap * sizeof(u32));
        if (!grown)
            return;   /* the table is optional; losing it degrades, not breaks */
        s_entries = grown;
        s_entry_cap = cap;
    }
    s_entries[s_entry_count++] = address;
}

static int compare_u32(const void* a, const void* b) {
    u32 x = *(const u32*)a, y = *(const u32*)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

int emit_write_entry_points(const char* path) {
    FILE* out;
    u32 i;
    /* No file at all when the switch covers every instruction: its absence is
     * what tells the chassis to keep treating a whole chunk range as entrable,
     * which is what every module built before this flag existed relies on. */
    if (!s_leader_cases)
        return 1;
    out = fopen(path, "w");
    if (!out)
        return 0;
    qsort(s_entries, s_entry_count, sizeof(u32), compare_u32);
    fprintf(out, "# Guest addresses the entry switch can be entered at.\n");
    fprintf(out, "# Written only for a reduced entry set (--leader-cases).\n");
    for (i = 0; i < s_entry_count; i++) {
        if (i != 0 && s_entries[i] == s_entries[i - 1u])
            continue;
        fprintf(out, "%08X\n", s_entries[i]);
    }
    fclose(out);
    return 1;
}

void emit_set_leader_cases(bool enable) {
    s_leader_cases = enable;
}

void emit_set_entry_targets(const u32* targets, u32 count) {
    free(s_entry_targets);
    s_entry_targets = NULL;
    s_entry_target_count = 0;
    if (!targets || count == 0)
        return;
    s_entry_targets = (u32*)malloc((size_t)count * sizeof(u32));
    if (!s_entry_targets)
        return;   /* degrades to leaders + FP sites; the hash gate would show it */
    memcpy(s_entry_targets, targets, (size_t)count * sizeof(u32));
    qsort(s_entry_targets, count, sizeof(u32), compare_u32);
    s_entry_target_count = count;
}

/* Is this instruction an entry beyond being a block leader? See the rule at
 * s_leader_cases. Read-only on s_entry_targets, so safe on the workers. */
static bool is_extra_entry(const PPCInst* inst) {
    u32 lo = 0, hi = s_entry_target_count;
    if (inst->embedded_data)
        return false;
    if (ppc_op_uses_fpu(inst->op))
        return true;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2u;
        if (s_entry_targets[mid] < inst->address)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return lo < s_entry_target_count && s_entry_targets[lo] == inst->address;
}

static u32 s_dispatch_pcs[32];
static u32 s_dispatch_pc_count = 0;

void emit_add_dispatch_pc(u32 pc) {
    if (pc != 0 && s_dispatch_pc_count < 32)
        s_dispatch_pcs[s_dispatch_pc_count++] = pc;
}

static bool must_reach_dispatcher(u32 pc) {
    for (u32 i = 0; i < s_dispatch_pc_count; ++i)
        if (s_dispatch_pcs[i] == pc)
            return true;
    return false;
}

/* ---- chunk overhang (--chunk-overhang) ------------------------------------
 *
 * Chunks are fixed 4096-instruction windows, so a boundary can fall anywhere,
 * including inside a loop. Measured on the US disc: the loop at 0x8000D8D0 -
 * 0x8000D96C straddles the chunk boundary at 0x8000D940, so every iteration
 * falls off one chunk and branches back out of the other -- two dispatcher
 * round trips per iteration, 13% of all chunk entries on the arcade route.
 *
 * An overhang lets a chunk carry on past its window as plain labels, up to the
 * first instruction that cannot fall through, so such a loop closes inside one
 * chunk. The overhang has no entry cases and is not in the dispatch table: the
 * next chunk still owns those addresses. It stops early at anything that must
 * be seen by the chassis: a hooked or idle PC (only BRANCHES to those are
 * forced out), and embedded data. The budget is an upper bound: an overhang
 * that would run out of it before an exit is not taken at all. */
static u32 s_overhang_max = 0;
/* End of the window of the chunk being emitted: calls INTO the overhang go to
   the owning chunk, whose switch has the case. Per worker thread. */
static DR_THREAD_LOCAL u32 s_own_end = 0;

void emit_set_chunk_overhang(u32 max_insts) { s_overhang_max = max_insts; }

static bool inst_never_falls_through(const PPCInst* inst) {
    switch (inst->op) {
    case PPC_OP_B:
        return !inst->lk;
    case PPC_OP_BCLR:
    case PPC_OP_BCCTR:
        return !inst->lk && (inst->bo & 0x14u) == 0x14u;
    case PPC_OP_RFI:
    case PPC_OP_SC:
        return true;
    default:
        return false;
    }
}

u32 emit_overhang_length(const PPCInst* insts, u32 own, u32 available,
                         const u32* stop_ranges, u32 stop_range_count) {
    u32 n;
    for (n = 0; n < available && n < s_overhang_max; n++) {
        const PPCInst* inst = &insts[own + n];
        u32 r;
        if (inst->embedded_data || must_reach_dispatcher(inst->address))
            return n;
        for (r = 0; r < stop_range_count; r++)
            if (inst->address >= stop_ranges[2 * r] && inst->address <= stop_ranges[2 * r + 1])
                return n;
        if (inst_never_falls_through(inst))
            return n + 1u;
    }
    /* Ran out of budget before an exit: an overhang that falls off its own end
       gains nothing over the dispatch it replaces. */
    return 0;
}

/* Cycles charged for the remainder of the current instruction's block: every
 * instruction after this one, up to the end of the block.
 *
 * A block charges its whole cost up front at its leader, so an exception that
 * leaves mid-block has charged CoreTiming for work that never ran -- events then
 * fire early. Measured: the FP-unavailable exit in the FP context-save prologue
 * at 0x80019F84 charged 367 cycles having executed five instructions.
 *
 * The faulting instruction itself stays charged, which matches the interpreter:
 * SingleStepInner has a single exit and returns that instruction's opinfo cycles.
 */
/* Thread-local: codegen.c emits chunks on N worker threads (-jN), so a plain
 * file-static is shared between them and one chunk's value lands in another's
 * output. That produced refunds of 1 and 7 cycles inside a 367-cycle block. */
static DR_THREAD_LOCAL u32 s_block_suffix = 0;

/* `if (ctx->exception) return;` plus the refund. */
static void emit_exc_check_return(FILE* out, const char* indent) {
    if (s_block_suffix != 0)
        fprintf(out, "%sif (ctx->exception) { DOLRECOMP_DC += %u; DOLRECOMP_RETURN; }\n",
                indent, s_block_suffix);
    else
        fprintf(out, "%sif (ctx->exception) DOLRECOMP_RETURN;\n", indent);
}

static int s_llvm_backend = 0;
void emit_set_llvm_backend(int enabled) { s_llvm_backend = enabled ? 1 : 0; }
int emit_llvm_backend_enabled(void) { return s_llvm_backend; }

void emit_set_idle_pc(u32 pc) {
    s_idle_pc = pc;
    /* The idle loop is just the first member of the must-dispatch set. */
    emit_add_dispatch_pc(pc);
}

static bool branch_target_is_local(u32 func_start, u32 func_end, u32 target) {
    return target >= func_start && target < func_end && ((target - func_start) & 3u) == 0;
}

/* --direct-calls: a `bl` into ANOTHER chunk calls that chunk's function
 * directly instead of returning to the chassis (after upstream 98f77b6).
 *
 * Measured on the US disc: 84.1% of all dispatches cross a chunk boundary
 * (793 M of 943 M over 14000 frames), and a guest call costs two of them --
 * the call and the callee's blr. A direct call removes both: the callee's
 * blr sets ctx->pc to the return address and returns from its C function, and
 * the caller resumes inline when that is the instruction after the call.
 * ANY other pc -- an exception, an exhausted loop budget, a tail call, a
 * target the callee's entry switch has no case for -- falls back to the
 * original `return`, which is always correct because ctx->pc already names
 * where the guest is.
 *
 * Constraints: calls to a PC the run loop hooks (must_reach_dispatcher: the
 * idle loop and every --dispatch-pc) keep returning, or the hook never fires;
 * guest recursion becomes host recursion, capped by DOLRECOMP_C_MAX_CALL_DEPTH;
 * and the chassis's per-dispatch work (timebase advance, downcount flush,
 * chunk verification on first dispatch) runs less often, so guest timing and
 * frame hashes legitimately change -- validate by determinism and content, not
 * by equality with the non-direct build. */
static bool s_direct_calls = false;
static u32* s_chunk_starts = NULL;
static u32 s_chunk_count = 0;

void emit_set_direct_calls(bool enable) {
    s_direct_calls = enable;
}

bool emit_direct_calls_enabled(void) {
    return s_direct_calls;
}

/* Chunk entry addresses in ascending order. MAIN THREAD ONLY, before the chunk
 * jobs run; the workers only read it. Copied. */
void emit_set_chunk_table(const u32* starts, u32 count) {
    free(s_chunk_starts);
    s_chunk_starts = NULL;
    s_chunk_count = 0;
    if (!starts || count == 0)
        return;
    s_chunk_starts = (u32*)malloc((size_t)count * sizeof(u32));
    if (!s_chunk_starts)
        return;   /* degrades to the return-to-chassis form */
    memcpy(s_chunk_starts, starts, (size_t)count * sizeof(u32));
    s_chunk_count = count;
}

/* The chunk whose func_<start>() covers addr, or 0. A target past the end of
 * the last chunk resolves to that chunk, whose entry switch has no case for it:
 * the call returns at once with ctx->pc unchanged and the caller falls back. */
static u32 chunk_start_for(u32 addr) {
    u32 lo = 0, hi = s_chunk_count;
    if (!s_chunk_starts)
        return 0;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2u;
        if (s_chunk_starts[mid] <= addr)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return lo ? s_chunk_starts[lo - 1u] : 0;
}

/* --self-calls and --tail-calls widen --direct-calls (and need it: they share its
 * chunk table, depth guard and hook rule).
 *
 * --self-calls: a `bl` to a function in the SAME chunk calls the chunk's own C
 * function, entering through its switch, instead of returning to the chassis.
 * The call and the callee's blr cost a dispatch each otherwise. (--chain-calls
 * made the call a goto, which kept the blr's dispatch; measured no win.)
 *
 * --tail-calls: a `b` into another chunk calls that chunk and then returns
 * whatever pc it left. The callee's blr lands in OUR caller's continuation, so
 * a caller that called us directly resumes inline instead of taking two
 * dispatches (the branch, then the blr back). */
static bool s_self_calls;
static bool s_tail_calls;

/* --exit-stats: count every return to the chassis by the instruction that
 * caused it. The dispatch log in the runtime can only say where control
 * ARRIVED, so it cannot tell a blr from a jump table, which is exactly what
 * decides where dispatch work is worth removing. Diagnostic only: one
 * increment per dispatch, and nothing is emitted without the flag. */
static bool s_exit_stats;
void emit_set_exit_stats(bool enable) { s_exit_stats = enable; }
bool emit_exit_stats_enabled(void) { return s_exit_stats; }

/* MEASURED DEAD, do not rebuild it: resuming on ANY entry of this chunk after a
 * native call (a label before the entry switch, re-entered when the callee came
 * back to some other pc). It collapses its target -- 54.0 M of 291.3 M
 * instrumented exits become 2.9 M -- and unprofiled it looked like a win
 * (-3.74% cycles, -9.89% instructions). WITH THE PROFILE, which is what ships,
 * it is +7.96% cycles and +10.34% instructions: the profiled build already gets
 * that work, and 25229 extra backward edges into the entry switch then wreck
 * the profile-driven layout. A no-PGO A/B is a screen, not a verdict. */

/* Keep in step with the enum emitted into generated.h. */
static void emit_exit_stat(FILE* out, const char* indent, const char* kind) {
    if (s_exit_stats)
        fprintf(out, "%sdolrecomp_exit_stat(DR_EXIT_%s);\n", indent, kind);
}
void emit_set_self_calls(bool enable) { s_self_calls = enable; }
void emit_set_tail_calls(bool enable) { s_tail_calls = enable; }
bool emit_self_calls_enabled(void) { return s_self_calls; }
bool emit_tail_calls_enabled(void) { return s_tail_calls; }

static bool emit_cross_chunk_tail(FILE* out, const PPCInst* inst, u32 func_start) {
    u32 target_chunk;
    if (!s_tail_calls || !s_direct_calls || must_reach_dispatcher(inst->branch_target))
        return false;
    target_chunk = chunk_start_for(inst->branch_target);
    if (!target_chunk || target_chunk == func_start)
        return false;
    fprintf(out, "            ctx->pc = 0x%08Xu;\n", inst->branch_target);
    fprintf(out, "            if (dolrecomp_call_enter()) {\n");
    fprintf(out, "                %svoid func_%08X(CPUState* ctx);\n", emit_chunk_cc(), target_chunk);
    fprintf(out, "                DOLRECOMP_DC_FLUSH(ctx);\n");
    fprintf(out, "                func_%08X(ctx);\n", target_chunk);
    fprintf(out, "                DOLRECOMP_DC_RELOAD(ctx);\n");
    fprintf(out, "                dolrecomp_call_leave();\n");
    fprintf(out, "            }\n");
    fprintf(out, "            DOLRECOMP_RETURN;\n");
    return true;
}

static bool emit_cross_chunk_call(FILE* out, const PPCInst* inst, u32 func_start, u32 func_end) {
    u32 continuation = inst->address + 4u;
    u32 target_chunk;
    if (!s_direct_calls || must_reach_dispatcher(inst->branch_target))
        return false;
    if (branch_target_is_local(func_start, s_own_end ? s_own_end : func_end, inst->branch_target))
        target_chunk = s_self_calls ? func_start : 0u;
    else
        target_chunk = chunk_start_for(inst->branch_target);
    if (!target_chunk || (target_chunk == func_start && !s_self_calls))
        return false;
    /* Nothing to resume into if the continuation is in another chunk. */
    if (!branch_target_is_local(func_start, func_end, continuation))
        return false;
    fprintf(out, "            ctx->pc = 0x%08Xu;\n", inst->branch_target);
    fprintf(out, "            if (dolrecomp_call_enter()) {\n");
    fprintf(out, "                %svoid func_%08X(CPUState* ctx);\n", emit_chunk_cc(), target_chunk);
    fprintf(out, "                DOLRECOMP_DC_FLUSH(ctx);\n");
    fprintf(out, "                func_%08X(ctx);\n", target_chunk);
    fprintf(out, "                DOLRECOMP_DC_RELOAD(ctx);\n");
    fprintf(out, "                dolrecomp_call_leave();\n");
    fprintf(out, "                if (ctx->pc == 0x%08Xu) goto label_%08X;\n", continuation, continuation);
    fprintf(out, "            }\n");
    emit_exit_stat(out, "            ", "CALL_MISS");
    fprintf(out, "            DOLRECOMP_RETURN;\n");
    return true;
}

static void emit_direct_branch(FILE* out, const PPCInst* inst, bool local_target,
                               u32 func_start, u32 func_end) {
    bool local_backward = local_target && inst->branch_target <= inst->address;

    if (inst->lk) {
        fprintf(out, "            ctx->lr = 0x%08Xu;\n", inst->address + 4);
        /* Call chaining. A local `bl` used to cost a full dispatcher round trip
         * on the way IN; the matching `blr` is indirect and still dispatches, so
         * this halves the dispatches per local call rather than removing them.
         * A backward local call is also a loop shape, so it takes the same cycle
         * budget as a back-edge -- without one the module could spin arbitrarily
         * long before CoreTiming regains control. */
        if (s_chain_calls && local_target &&
            !must_reach_dispatcher(inst->branch_target)) {
            if (local_backward) {
                fprintf(out, "            if (DOLRECOMP_DC <= -%d) {\n", DOLRECOMP_LOOP_BUDGET);
                fprintf(out, "                ctx->pc = 0x%08Xu;\n", inst->branch_target);
                fprintf(out, "                DOLRECOMP_RETURN;\n");
                fprintf(out, "            }\n");
            }
            fprintf(out, "            goto label_%08X;\n", inst->branch_target);
            return;
        }
        if ((!local_target || s_self_calls) && emit_cross_chunk_call(out, inst, func_start, func_end))
            return;
        emit_exit_stat(out, "            ", local_target ? "BL_LOCAL" : "BL_CROSS");
        fprintf(out, "            ctx->pc = 0x%08Xu;\n", inst->branch_target);
        fprintf(out, "            DOLRECOMP_RETURN;\n");
        return;
    }
    /* The idle loop must keep reaching the chassis, or idle-skip dies with it. */
    if (local_backward && must_reach_dispatcher(inst->branch_target)) {
        emit_exit_stat(out, "            ", "IDLE");
        fprintf(out, "            ctx->pc = 0x%08Xu;\n", inst->branch_target);
        fprintf(out, "            DOLRECOMP_RETURN;\n");
        return;
    }
    if (local_backward) {
        /* Loop back-edge. Returning to the dispatcher here is what made every
         * loop iteration a full round-trip: measured, 52.8% of all dispatches
         * (73.3M of 138.8M in a 1200-frame run) are exactly this.
         *
         * Staying native needs a bound, or the module could spin arbitrarily long
         * without CoreTiming regaining control and external interrupts would be
         * delivered late. The branch target is a local branch target, so it is a
         * leader and charges ctx->downcount every iteration -- the counter always
         * moves, and the budget is reached in bounded time. Past the budget we
         * hand control back exactly as before, so this only ever shortens the
         * time between dispatches, never lengthens it beyond DOLRECOMP_LOOP_BUDGET
         * cycles. */
        fprintf(out, "            if (DOLRECOMP_DC <= -%d) {\n", DOLRECOMP_LOOP_BUDGET);
        emit_exit_stat(out, "                ", "LOOP_BUDGET");
        fprintf(out, "                ctx->pc = 0x%08Xu;\n", inst->branch_target);
        fprintf(out, "                DOLRECOMP_RETURN;\n");
        fprintf(out, "            }\n");
        fprintf(out, "            goto label_%08X;\n", inst->branch_target);
    } else if (local_target) {
        fprintf(out, "            goto label_%08X;\n", inst->branch_target);
    } else if (!emit_cross_chunk_tail(out, inst, func_start)) {
        emit_exit_stat(out, "            ", "B_CROSS");
        fprintf(out, "            ctx->pc = 0x%08Xu;\n", inst->branch_target);
        fprintf(out, "            DOLRECOMP_RETURN;\n");
    }
}

static void emit_dynamic_branch(FILE* out, const PPCInst* inst,
                                const char* target_expr) {
    fprintf(out, "    {\n");
    fprintf(out, "        u32 target = %s;\n", target_expr);
    emit_branch_condition(out, inst->bo, inst->bi);
    fprintf(out, "        if (ctr_ok && cr_ok) {\n");
    if (inst->lk) {
        fprintf(out, "            ctx->lr = 0x%08Xu;\n", inst->address + 4);
    }
    emit_exit_stat(out, "            ",
                   strstr(target_expr, "lr") ? (inst->lk ? "BLRL" : "BLR")
                                             : (inst->lk ? "BCTRL" : "BCTR"));
    fprintf(out, "            ctx->pc = target;\n");
    fprintf(out, "            DOLRECOMP_RETURN;\n");
    fprintf(out, "        }\n");
    fprintf(out, "    }\n");
}

static void emit_cr_logical(FILE* out, const PPCInst* inst, const char* expr) {
    fprintf(out, "    {\n");
    fprintf(out, "        PPC_CR_READ(%uu);\n",
            (1u << (inst->rA >> 2)) | (1u << (inst->rB >> 2)));
    fprintf(out, "        u32 a = (ctx->crf[%u] >> %uu) & 1u;\n", inst->rA >> 2, 3u - (inst->rA & 3u));
    fprintf(out, "        u32 b = (ctx->crf[%u] >> %uu) & 1u;\n", inst->rB >> 2, 3u - (inst->rB & 3u));
    fprintf(out, "        u32 mask = 0x%Xu;\n", 8u >> (inst->rD & 3u));
    fprintf(out, "        u32 value = (%s) & 1u;\n", expr);
    fprintf(out, "        ctx->crf[%u] = (u8)((ctx->crf[%u] & ~mask) | (value ? mask : 0u));\n",
            inst->rD >> 2, inst->rD >> 2);
    fprintf(out, "        PPC_CR_WRITE(%u);\n", (u32)(inst->rD >> 2));
    fprintf(out, "    }\n");
}

static void emit_record_if_needed(FILE* out, const PPCInst* inst, u8 reg) {
    if (inst->rc) {
        emit_set_cr0_from_gpr(out, reg);
    }
}

static const char* emit_cpu_macro(DolRecompCPU cpu) {
    switch (cpu) {
    case DOLRECOMP_CPU_BROADWAY:
        return "BROADWAY";
    case DOLRECOMP_CPU_ESPRESSO:
        return "ESPRESSO";
    case DOLRECOMP_CPU_GEKKO:
    default:
        return "GEKKO";
    }
}

static const char* emit_cpu_label(DolRecompCPU cpu) {
    switch (cpu) {
    case DOLRECOMP_CPU_BROADWAY:
        return "broadway";
    case DOLRECOMP_CPU_ESPRESSO:
        return "espresso";
    case DOLRECOMP_CPU_GEKKO:
    default:
        return "gekko";
    }
}

/* Set by the DOL pre-pass when no text section contains lwarx/stwcx; written
   into generated.h ahead of cpu.h so a DOLRECOMP_MEM_FAST module can drop the
   reservation test. Nothing is written otherwise, so other output is unchanged. */
static bool s_no_reservation;
void emit_set_no_reservation(bool none) { s_no_reservation = none; }

void emit_header_for_cpu(FILE* out, DolRecompCPU cpu) {
    fprintf(out,
        "// DolRecomp output\n"
        "// cpu: %s\n"
        "\n"
        "#ifndef RECOMP_GENERATED_H\n"
        "#define RECOMP_GENERATED_H\n"
        "\n"
        "#define DOLRECOMP_CPU_%s 1\n"
        "#define DOLRECOMP_CPU_NAME \"%s\"\n"
        "%s"
        "\n"
        "#include <string.h>\n"
        "#include <math.h>\n"
        "#include \"cpu/cpu.h\"\n"
        "\n"
        "static inline u32 dolrecomp_rotl32(u32 value, u32 sh) {\n"
        "    sh &= 31u;\n"
        "    return sh ? ((value << sh) | (value >> (32u - sh))) : value;\n"
        "}\n"
        "\n"
        "static inline f32 dolrecomp_f32_from_bits(u32 bits) {\n"
        "    f32 value;\n"
        "    memcpy(&value, &bits, sizeof(value));\n"
        "    return value;\n"
        "}\n"
        "\n"
        "static inline u32 dolrecomp_f32_to_bits(f32 value) {\n"
        "    u32 bits;\n"
        "    memcpy(&bits, &value, sizeof(bits));\n"
        "    return bits;\n"
        "}\n"
        "\n"
        "static inline f64 dolrecomp_f64_from_bits(u64 bits) {\n"
        "    f64 value;\n"
        "    memcpy(&value, &bits, sizeof(value));\n"
        "    return value;\n"
        "}\n"
        "\n"
        "static inline u64 dolrecomp_f64_to_bits(f64 value) {\n"
        "    u64 bits;\n"
        "    memcpy(&bits, &value, sizeof(bits));\n"
        "    return bits;\n"
        "}\n"
        "\n"
        /* PPC_PS_LANE expands to nothing unless -DRECOMP_CR_STATS, so the
           shipped generation is unchanged. Hooked HERE rather than in each
           ps_* case because every paired-single lane operation routes through
           this round and nothing else does -- the scalar single forms
           (fadds/fsubs/...) cast inline and never call it. Counts LANES, so
           halve it for operations; includes ps_merge*, which is a shuffle
           rather than arithmetic. */
        "static inline f64 dolrecomp_ps_round(f64 value) {\n"
        "    PPC_PS_LANE();\n"
        "    return (f64)(f32)value;\n"
        "}\n"
        "\n"
        /* The Gekko rounds a single-precision multiply's RIGHT operand to a
           25-bit mantissa first (Dolphin: Force25Bit). A no-op on any value
           that is already single precision -- its mantissa ends 29 bits above
           the bottom -- so it only moves a result when a double feeds a single
           multiply. Branchless: a branch here would add a block to every chunk
           that multiplies, and the chunk's PGO profile would stop matching.
           Subnormals round at the normal bit instead of being renormalised,
           which is what Jit64 does too. */
        "static inline f64 dolrecomp_force25(f64 value) {\n"
        "    u64 bits = dolrecomp_f64_to_bits(value);\n"
        "    return dolrecomp_f64_from_bits((bits & 0xFFFFFFFFF8000000ull) + (bits & 0x0000000008000000ull));\n"
        "}\n"
        "\n"
        "static inline f64 dolrecomp_ps_from_bits(u32 bits) {\n"
        "    return (f64)dolrecomp_f32_from_bits(bits);\n"
        "}\n"
        "\n"
        "static inline u32 dolrecomp_ps_to_bits(f64 value) {\n"
        "    return dolrecomp_f32_to_bits((f32)value);\n"
        "}\n"
        "\n"
        "#if defined(__GNUC__) || defined(__clang__)\n"
        "#define DOLRECOMP_FPRF_FI static inline __attribute__((always_inline))\n"
        "#else\n"
        "#define DOLRECOMP_FPRF_FI static inline\n"
        "#endif\n"
        "\n"
        // FPRF (FPSCR bits 12-16: class, <, >, =, ?) after an arithmetic result.
        // always_inline, not plain inline: these are a handful of bit tests, but
        // the compiler declines to inline them into 16 KB chunk functions and a
        // profile of real play showed dolrecomp_classify_s as an out-of-line call
        // costing ~3% of the CPU thread. The harness workload exercises FP far
        // less and reported the cost as noise -- measure on the workload you care
        // about.
        // The plain arithmetic below is emitted as C rather than routed through a
        // runtime helper, so nothing was maintaining these bits -- lockstep caught
        // native holding fpscr=0x4 where the interpreter had 0x4004/0xc004. The
        // game reads FPSCR (two mffs sites), so it is observable, not cosmetic.
        // ppc_fma / fres / frsqrte / ps_res / ps_rsqrte already set it themselves.
        // Classification mirrors classify_f64 / classify_f32 in cpu.c exactly.
        "DOLRECOMP_FPRF_FI u32 dolrecomp_classify_d(f64 value) {\n"
        "    u64 bits = dolrecomp_f64_to_bits(value);\n"
        "    u64 sign = bits >> 63;\n"
        "    u64 exponent = bits & 0x7FF0000000000000ull;\n"
        "    u64 fraction = bits & 0x000FFFFFFFFFFFFFull;\n"
        "    if (exponent == 0x7FF0000000000000ull)\n"
        "        return fraction ? 0x11u : (sign ? 0x09u : 0x05u);\n"
        "    if (exponent == 0)\n"
        "        return fraction ? (sign ? 0x18u : 0x14u) : (sign ? 0x12u : 0x02u);\n"
        "    return sign ? 0x08u : 0x04u;\n"
        "}\n"
        "\n"
        "DOLRECOMP_FPRF_FI u32 dolrecomp_classify_s(f32 value) {\n"
        "    u32 bits = dolrecomp_f32_to_bits(value);\n"
        "    u32 sign = bits >> 31;\n"
        "    u32 exponent = bits & 0x7F800000u;\n"
        "    u32 fraction = bits & 0x007FFFFFu;\n"
        "    if (exponent == 0x7F800000u)\n"
        "        return fraction ? 0x11u : (sign ? 0x09u : 0x05u);\n"
        "    if (exponent == 0)\n"
        "        return fraction ? (sign ? 0x18u : 0x14u) : (sign ? 0x12u : 0x02u);\n"
        "    return sign ? 0x08u : 0x04u;\n"
        "}\n"
        "\n"
        "#ifdef RECOMP_NO_FPRF\n"
        "DOLRECOMP_FPRF_FI void dolrecomp_fprf_d(CPUState* ctx, f64 value) { (void)ctx; (void)value; }\n"
        "DOLRECOMP_FPRF_FI void dolrecomp_fprf_s(CPUState* ctx, f32 value) { (void)ctx; (void)value; }\n"
        "#elif defined(RECOMP_EAGER_FPRF)\n"
        "DOLRECOMP_FPRF_FI void dolrecomp_fprf_d(CPUState* ctx, f64 value) {\n"
        "    PPC_FPRF_WRITE();\n"
        "    ctx->fpscr = (ctx->fpscr & ~(0x1Fu << 12)) | (dolrecomp_classify_d(value) << 12);\n"
        "}\n"
        "DOLRECOMP_FPRF_FI void dolrecomp_fprf_s(CPUState* ctx, f32 value) {\n"
        "    PPC_FPRF_WRITE();\n"
        "    ctx->fpscr = (ctx->fpscr & ~(0x1Fu << 12)) | (dolrecomp_classify_s(value) << 12);\n"
        "}\n"
        "#else\n"
        "/* Lazy: remember the result, classify only when FPSCR can be seen. */\n"
        "DOLRECOMP_FPRF_FI void dolrecomp_fprf_d(CPUState* ctx, f64 value) {\n"
        "    PPC_FPRF_WRITE(); (void)ctx;\n"
        "    g_fprf_value = value; g_fprf_kind = 2u;\n"
        "}\n"
        "DOLRECOMP_FPRF_FI void dolrecomp_fprf_s(CPUState* ctx, f32 value) {\n"
        "    PPC_FPRF_WRITE(); (void)ctx;\n"
        "    g_fprf_value = (f64)value; g_fprf_kind = 1u;\n"
        "}\n"
        "#endif\n"
        "\n"
        ,
        emit_cpu_label(cpu),
        emit_cpu_macro(cpu),
        emit_cpu_label(cpu),
        s_no_reservation ? "#define DOLRECOMP_DOL_NO_RESERVATION 1\n" : "");

    /* Cross-chunk direct calls (--direct-calls) turn guest recursion into host
       recursion; past the cap the call site returns to the chassis, which is
       always correct. One counter for all chunks: it is defined in cpu.c,
       which every module links. Not atomic: one CPU thread. Written only under
       the flag, so a default generation stays byte-identical. */
    if (s_direct_calls) {
        fprintf(out,
            "\n"
            "#ifndef DOLRECOMP_C_MAX_CALL_DEPTH\n"
            "#define DOLRECOMP_C_MAX_CALL_DEPTH 24\n"
            "#endif\n"
            "extern unsigned dolrecomp_call_depth;\n"
            "static inline int dolrecomp_call_enter(void) {\n"
            "    if (dolrecomp_call_depth >= (unsigned)DOLRECOMP_C_MAX_CALL_DEPTH)\n"
            "        return 0;\n"
            "    dolrecomp_call_depth++;\n"
            "    return 1;\n"
            "}\n"
            "static inline void dolrecomp_call_leave(void) {\n"
            "    if (dolrecomp_call_depth)\n"
            "        dolrecomp_call_depth--;\n"
            "}\n");
    }

    /* --exit-stats counters. The order here IS the order of the names printed
       by the destructor in generated.c; keep both in step with emit_exit_stat. */
    if (s_exit_stats) {
        fprintf(out,
            "\n"
            "#define DOLRECOMP_EXIT_STATS 1\n"
            "enum {\n"
            "    DR_EXIT_BLR, DR_EXIT_BLRL, DR_EXIT_BCTR, DR_EXIT_BCTRL,\n"
            "    DR_EXIT_BL_LOCAL, DR_EXIT_BL_CROSS, DR_EXIT_B_CROSS,\n"
            "    DR_EXIT_LOOP_BUDGET, DR_EXIT_IDLE, DR_EXIT_CALL_MISS,\n"
            "    DR_EXIT_SWITCH_MISS,\n"
            "    DR_EXIT_KINDS\n"
            "};\n"
            /* Weak, and defined right here in the header: the counters must
               live in a TU the module actually COMPILES. Putting them in the
               manifest generated.c left dolrecomp_exit_counts undefined, the
               module failed to load and the chassis rejected it. Every chunk
               includes this header, so weak linkage collapses the 132 copies
               into one array and one destructor -- which therefore prints the
               table once, when the module unloads. */
            "#include <stdio.h>\n"
            "__attribute__((weak)) unsigned long long dolrecomp_exit_counts[DR_EXIT_KINDS];\n"
            /* Count only returns that actually reach the chassis. Inside a
               native call the same `return` hands control to the CALLER's C
               frame, which is not a dispatch: counting those said 694 M exits
               where the chassis had done 396 M dispatches, and would have
               ranked the levers by the wrong denominator. */
            /* dolrecomp_call_depth exists only under --direct-calls; without it
               every return already goes to the chassis, so the test is 1. */
            "#if defined(DOLRECOMP_C_MAX_CALL_DEPTH)\n"
            "#define DOLRECOMP_EXIT_AT_TOP (dolrecomp_call_depth == 0u)\n"
            "#else\n"
            "#define DOLRECOMP_EXIT_AT_TOP 1\n"
            "#endif\n"
            "static inline void dolrecomp_exit_stat(int kind) {\n"
            "    if (DOLRECOMP_EXIT_AT_TOP)\n"
            "        dolrecomp_exit_counts[kind]++;\n"
            "}\n"
            "__attribute__((weak, destructor)) void dolrecomp_exit_stats_dump(void) {\n"
            "    static int done = 0;\n"
            "    if (done) return;\n"
            "    done = 1;\n"
            "    static const char* const names[DR_EXIT_KINDS] = {\n"
            "        \"blr (return)\", \"blrl\", \"bctr (jump table)\", \"bctrl (fn pointer)\",\n"
            "        \"bl same chunk\", \"bl cross chunk\", \"b cross chunk\",\n"
            "        \"loop budget\", \"idle loop\", \"direct call, pc not the return\",\n"
            "        \"entry switch: pc not in this chunk\"\n"
            "    };\n"
            "    unsigned long long total = 0;\n"
            "    for (int i = 0; i < DR_EXIT_KINDS; i++) total += dolrecomp_exit_counts[i];\n"
            "    if (!total) return;\n"
            "    fprintf(stderr, \"[exit-stats] %%llu dispatches from the module\\n\", total);\n"
            "    for (int i = 0; i < DR_EXIT_KINDS; i++)\n"
            "        if (dolrecomp_exit_counts[i])\n"
            "            fprintf(stderr, \"[exit-stats] %%-32s %%12llu  %%5.1f%%%%\\n\", names[i],\n"
            "                    dolrecomp_exit_counts[i],\n"
            "                    100.0 * (double)dolrecomp_exit_counts[i] / (double)total);\n"
            "}\n");
    }
}

void emit_header(FILE* out) {
    emit_header_for_cpu(out, DOLRECOMP_CPU_GEKKO);
}

void emit_footer(FILE* out) {
    fprintf(out, "\n#endif /* RECOMP_GENERATED_H */\n\n// end\n");
}

static void emit_instruction_with_range(FILE* out, const PPCInst* inst,
                                        u32 func_start, u32 func_end) {
    char disasm[64];
    ppc_disasm(disasm, sizeof(disasm), inst);
    fprintf(out, "    // %08X: %s\n", inst->address, disasm);

    if (inst->embedded_data) {
        fprintf(out, "    // embedded data\n\n");
        return;
    }

    if (ppc_op_uses_fpu(inst->op)) {
        if (s_block_suffix != 0) {
            fprintf(out,
                    "    if (!ppc_fp_available(ctx, 0x%08Xu)) { DOLRECOMP_DC += %u; DOLRECOMP_RETURN; }\n",
                    inst->address, s_block_suffix);
        } else {
            fprintf(out, "    if (!ppc_fp_available(ctx, 0x%08Xu)) DOLRECOMP_RETURN;\n", inst->address);
        }
        if (s_fp_skip_label == inst->address)
            fprintf(out, "fpok_%08X:\n", inst->address);
    }

    switch (inst->op) {
    case PPC_OP_MULLI:
        fprintf(out, "    ctx->gpr[%u] = (u32)((s64)(s32)ctx->gpr[%u] * (s64)(s32)%d);\n",
                inst->rD, inst->rA, (int)inst->simm);
        break;

    case PPC_OP_SUBFIC:
        fprintf(out, "    {\n");
        fprintf(out, "        u64 res = (u64)(u32)(s32)(%d) + (u64)(~ctx->gpr[%u]) + 1u;\n",
                (int)inst->simm, inst->rA);
        fprintf(out, "        ctx->gpr[%u] = (u32)res;\n", inst->rD);
        if (!s_ca_dead_here) {
            fprintf(out, "#ifndef RECOMP_NO_CA\n");
            fprintf(out, "        ctx->xer = (ctx->xer & ~0x20000000u) | (((u32)(res >> 32) & 1u) << 29);\n");
            fprintf(out, "#endif\n");
        }
        fprintf(out, "        PPC_CA_WRITE();\n");
        fprintf(out, "    }\n");
        break;

    case PPC_OP_ADDI:
        if (inst->rA == 0) {
            fprintf(out, "    ctx->gpr[%u] = (u32)(s32)(%d);\n",
                    inst->rD, (int)inst->simm);
        } else {
            fprintf(out, "    ctx->gpr[%u] = ctx->gpr[%u] + (u32)(s32)(%d);\n",
                    inst->rD, inst->rA, (int)inst->simm);
        }
        break;

    case PPC_OP_ADDIC:
    case PPC_OP_ADDIC_DOT:
        fprintf(out, "    {\n");
        fprintf(out, "        u64 a = ctx->gpr[%u];\n", inst->rA);
        fprintf(out, "        u64 b = (u32)(s32)(%d);\n", (int)inst->simm);
        fprintf(out, "        u64 res = a + b;\n");
        fprintf(out, "        ctx->gpr[%u] = (u32)res;\n", inst->rD);
        if (!s_ca_dead_here) {
            fprintf(out, "#ifndef RECOMP_NO_CA\n");
            fprintf(out, "        ctx->xer = (ctx->xer & ~0x20000000u) | (((u32)(res >> 32) & 1u) << 29);\n");
            fprintf(out, "#endif\n");
        }
        fprintf(out, "        PPC_CA_WRITE();\n");
        if (inst->op == PPC_OP_ADDIC_DOT) {
            emit_set_cr0_from_gpr(out, inst->rD);
        }
        fprintf(out, "    }\n");
        break;

    case PPC_OP_ADDIS:
        if (inst->rA == 0) {
            fprintf(out, "    ctx->gpr[%u] = ((u32)(s32)(%d) << 16);\n",
                    inst->rD, (int)inst->simm);
        } else {
            fprintf(out, "    ctx->gpr[%u] = ctx->gpr[%u] + ((u32)(s32)(%d) << 16);\n",
                    inst->rD, inst->rA, (int)inst->simm);
        }
        break;

    case PPC_OP_CMPI:
        {
            char rhs[32];
            snprintf(rhs, sizeof(rhs), "%d", (int)inst->simm);
            char lhs[32];
            snprintf(lhs, sizeof(lhs), "ctx->gpr[%u]", inst->rA);
            emit_compare_s32(out, inst->crfD, lhs, rhs);
        }
        break;

    case PPC_OP_CMPLI:
        {
            char rhs[32];
            snprintf(rhs, sizeof(rhs), "0x%04Xu", inst->uimm);
            char lhs[32];
            snprintf(lhs, sizeof(lhs), "ctx->gpr[%u]", inst->rA);
            emit_compare_u32(out, inst->crfD, lhs, rhs);
        }
        break;

    case PPC_OP_CMP:
        {
            char lhs[32], rhs[32];
            snprintf(lhs, sizeof(lhs), "ctx->gpr[%u]", inst->rA);
            snprintf(rhs, sizeof(rhs), "ctx->gpr[%u]", inst->rB);
            emit_compare_s32(out, inst->crfD, lhs, rhs);
        }
        break;

    case PPC_OP_CMPL:
        {
            char lhs[32], rhs[32];
            snprintf(lhs, sizeof(lhs), "ctx->gpr[%u]", inst->rA);
            snprintf(rhs, sizeof(rhs), "ctx->gpr[%u]", inst->rB);
            emit_compare_u32(out, inst->crfD, lhs, rhs);
        }
        break;

    case PPC_OP_ORI:
        if (inst->rS == 0 && inst->rA == 0 && inst->uimm == 0) {
            fprintf(out, "    // nop\n");
        } else {
            fprintf(out, "    ctx->gpr[%u] = ctx->gpr[%u] | 0x%04Xu;\n",
                    inst->rA, inst->rS, inst->uimm);
        }
        break;

    case PPC_OP_ORIS:
        fprintf(out, "    ctx->gpr[%u] = ctx->gpr[%u] | (0x%04Xu << 16);\n",
                inst->rA, inst->rS, inst->uimm);
        break;

    case PPC_OP_XORI:
        fprintf(out, "    ctx->gpr[%u] = ctx->gpr[%u] ^ 0x%04Xu;\n",
                inst->rA, inst->rS, inst->uimm);
        break;

    case PPC_OP_XORIS:
        fprintf(out, "    ctx->gpr[%u] = ctx->gpr[%u] ^ (0x%04Xu << 16);\n",
                inst->rA, inst->rS, inst->uimm);
        break;

    case PPC_OP_ANDI:
        fprintf(out, "    {\n");
        fprintf(out, "        ctx->gpr[%u] = ctx->gpr[%u] & 0x%04Xu;\n",
                inst->rA, inst->rS, inst->uimm);
        emit_set_cr0_from_gpr(out, inst->rA);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_ANDIS:
        fprintf(out, "    {\n");
        fprintf(out, "        ctx->gpr[%u] = ctx->gpr[%u] & (0x%04Xu << 16);\n",
                inst->rA, inst->rS, inst->uimm);
        emit_set_cr0_from_gpr(out, inst->rA);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_ADD:
    case PPC_OP_ADDO:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 a = ctx->gpr[%u];\n", inst->rA);
        fprintf(out, "        u32 b = ctx->gpr[%u];\n", inst->rB);
        fprintf(out, "        u32 res = a + b;\n");
        fprintf(out, "        ctx->gpr[%u] = res;\n", inst->rD);
        if (inst->oe)
            fprintf(out, "        ppc_set_xer_ov(ctx, ppc_add_overflowed(a, b, res));\n");
        emit_record_if_needed(out, inst, inst->rD);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_ADDC:
    case PPC_OP_ADDCO:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 a = ctx->gpr[%u];\n", inst->rA);
        fprintf(out, "        u32 b = ctx->gpr[%u];\n", inst->rB);
        fprintf(out, "        u64 wide = (u64)a + (u64)b;\n");
        fprintf(out, "        u32 res = (u32)wide;\n");
        fprintf(out, "        ctx->gpr[%u] = res;\n", inst->rD);
        if (!s_ca_dead_here) {
            fprintf(out, "#ifndef RECOMP_NO_CA\n");
            fprintf(out, "        ctx->xer = (ctx->xer & ~0x20000000u) | (((u32)(wide >> 32) & 1u) << 29);\n");
            fprintf(out, "#endif\n");
        }
        fprintf(out, "        PPC_CA_WRITE();\n");
        if (inst->oe)
            fprintf(out, "        ppc_set_xer_ov(ctx, ppc_add_overflowed(a, b, res));\n");
        emit_record_if_needed(out, inst, inst->rD);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_ADDE:
    case PPC_OP_ADDEO:
        fprintf(out, "    {\n");
        fprintf(out, "        PPC_CA_READ();\n");
        fprintf(out, "        u32 carry = (ctx->xer >> 29) & 1u;\n");
        fprintf(out, "        u32 a = ctx->gpr[%u];\n", inst->rA);
        fprintf(out, "        u32 b = ctx->gpr[%u];\n", inst->rB);
        fprintf(out, "        u64 wide = (u64)a + (u64)b + carry;\n");
        fprintf(out, "        u32 res = (u32)wide;\n");
        fprintf(out, "        ctx->gpr[%u] = res;\n", inst->rD);
        if (!s_ca_dead_here) {
            fprintf(out, "#ifndef RECOMP_NO_CA\n");
            fprintf(out, "        ctx->xer = (ctx->xer & ~0x20000000u) | (((u32)(wide >> 32) & 1u) << 29);\n");
            fprintf(out, "#endif\n");
        }
        fprintf(out, "        PPC_CA_WRITE();\n");
        if (inst->oe)
            fprintf(out, "        ppc_set_xer_ov(ctx, ppc_add_overflowed(a, b, res));\n");
        emit_record_if_needed(out, inst, inst->rD);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_ADDME:
    case PPC_OP_ADDMEO:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 input = ctx->gpr[%u];\n", inst->rA);
        fprintf(out, "        PPC_CA_READ();\n");
        fprintf(out, "        u32 carry = (ctx->xer >> 29) & 1u;\n");
        fprintf(out, "        u64 res = (u64)input + 0xFFFFFFFFull + carry;\n");
        fprintf(out, "        ctx->gpr[%u] = (u32)res;\n", inst->rD);
        if (!s_ca_dead_here) {
            fprintf(out, "#ifndef RECOMP_NO_CA\n");
            fprintf(out, "        ctx->xer = (ctx->xer & ~0x20000000u) | ((res >> 32) ? 0x20000000u : 0u);\n");
            fprintf(out, "#endif\n");
        }
        fprintf(out, "        PPC_CA_WRITE();\n");
        if (inst->oe)
            fprintf(out, "        ppc_set_xer_ov(ctx, ppc_add_overflowed(input, 0xFFFFFFFFu, (u32)res));\n");
        emit_record_if_needed(out, inst, inst->rD);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_ADDZE:
    case PPC_OP_ADDZEO:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 a = ctx->gpr[%u];\n", inst->rA);
        fprintf(out, "        PPC_CA_READ();\n");
        fprintf(out, "        u64 wide = (u64)a + ((ctx->xer >> 29) & 1u);\n");
        fprintf(out, "        u32 res = (u32)wide;\n");
        fprintf(out, "        ctx->gpr[%u] = res;\n", inst->rD);
        if (!s_ca_dead_here) {
            fprintf(out, "#ifndef RECOMP_NO_CA\n");
            fprintf(out, "        ctx->xer = (ctx->xer & ~0x20000000u) | (((u32)(wide >> 32) & 1u) << 29);\n");
            fprintf(out, "#endif\n");
        }
        fprintf(out, "        PPC_CA_WRITE();\n");
        if (inst->oe)
            fprintf(out, "        ppc_set_xer_ov(ctx, ppc_add_overflowed(a, 0u, res));\n");
        emit_record_if_needed(out, inst, inst->rD);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_SUBF:
    case PPC_OP_SUBFO:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 a = ~ctx->gpr[%u];\n", inst->rA);
        fprintf(out, "        u32 b = ctx->gpr[%u];\n", inst->rB);
        fprintf(out, "        u32 res = a + b + 1u;\n");
        fprintf(out, "        ctx->gpr[%u] = res;\n", inst->rD);
        if (inst->oe)
            fprintf(out, "        ppc_set_xer_ov(ctx, ppc_add_overflowed(a, b, res));\n");
        emit_record_if_needed(out, inst, inst->rD);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_SUBFC:
    case PPC_OP_SUBFCO:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 a = ~ctx->gpr[%u];\n", inst->rA);
        fprintf(out, "        u32 b = ctx->gpr[%u];\n", inst->rB);
        fprintf(out, "        u64 wide = (u64)b + (u64)a + 1u;\n");
        fprintf(out, "        u32 res = (u32)wide;\n");
        fprintf(out, "        ctx->gpr[%u] = res;\n", inst->rD);
        if (!s_ca_dead_here) {
            fprintf(out, "#ifndef RECOMP_NO_CA\n");
            fprintf(out, "        ctx->xer = (ctx->xer & ~0x20000000u) | (((u32)(wide >> 32) & 1u) << 29);\n");
            fprintf(out, "#endif\n");
        }
        fprintf(out, "        PPC_CA_WRITE();\n");
        if (inst->oe)
            fprintf(out, "        ppc_set_xer_ov(ctx, ppc_add_overflowed(a, b, res));\n");
        emit_record_if_needed(out, inst, inst->rD);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_SUBFE:
    case PPC_OP_SUBFEO:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 a = ~ctx->gpr[%u];\n", inst->rA);
        fprintf(out, "        u32 b = ctx->gpr[%u];\n", inst->rB);
        fprintf(out, "        PPC_CA_READ();\n");
        fprintf(out, "        u32 carry = (ctx->xer >> 29) & 1u;\n");
        fprintf(out, "        u64 wide = (u64)a + (u64)b + carry;\n");
        fprintf(out, "        u32 res = (u32)wide;\n");
        fprintf(out, "        ctx->gpr[%u] = res;\n", inst->rD);
        if (!s_ca_dead_here) {
            fprintf(out, "#ifndef RECOMP_NO_CA\n");
            fprintf(out, "        ctx->xer = (ctx->xer & ~0x20000000u) | (((u32)(wide >> 32) & 1u) << 29);\n");
            fprintf(out, "#endif\n");
        }
        fprintf(out, "        PPC_CA_WRITE();\n");
        if (inst->oe)
            fprintf(out, "        ppc_set_xer_ov(ctx, ppc_add_overflowed(a, b, res));\n");
        emit_record_if_needed(out, inst, inst->rD);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_SUBFME:
    case PPC_OP_SUBFMEO:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 input = ~ctx->gpr[%u];\n", inst->rA);
        fprintf(out, "        PPC_CA_READ();\n");
        fprintf(out, "        u32 carry = (ctx->xer >> 29) & 1u;\n");
        fprintf(out, "        u64 res = (u64)input + 0xFFFFFFFFull + carry;\n");
        fprintf(out, "        ctx->gpr[%u] = (u32)res;\n", inst->rD);
        if (!s_ca_dead_here) {
            fprintf(out, "#ifndef RECOMP_NO_CA\n");
            fprintf(out, "        ctx->xer = (ctx->xer & ~0x20000000u) | ((res >> 32) ? 0x20000000u : 0u);\n");
            fprintf(out, "#endif\n");
        }
        fprintf(out, "        PPC_CA_WRITE();\n");
        if (inst->oe)
            fprintf(out, "        ppc_set_xer_ov(ctx, ppc_add_overflowed(input, 0xFFFFFFFFu, (u32)res));\n");
        emit_record_if_needed(out, inst, inst->rD);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_SUBFZE:
    case PPC_OP_SUBFZEO:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 a = ~ctx->gpr[%u];\n", inst->rA);
        fprintf(out, "        PPC_CA_READ();\n");
        fprintf(out, "        u64 wide = (u64)a + ((ctx->xer >> 29) & 1u);\n");
        fprintf(out, "        u32 res = (u32)wide;\n");
        fprintf(out, "        ctx->gpr[%u] = res;\n", inst->rD);
        if (!s_ca_dead_here) {
            fprintf(out, "#ifndef RECOMP_NO_CA\n");
            fprintf(out, "        ctx->xer = (ctx->xer & ~0x20000000u) | (((u32)(wide >> 32) & 1u) << 29);\n");
            fprintf(out, "#endif\n");
        }
        fprintf(out, "        PPC_CA_WRITE();\n");
        if (inst->oe)
            fprintf(out, "        ppc_set_xer_ov(ctx, ppc_add_overflowed(a, 0u, res));\n");
        emit_record_if_needed(out, inst, inst->rD);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_NEG:
    case PPC_OP_NEGO:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 a = ctx->gpr[%u];\n", inst->rA);
        fprintf(out, "        ctx->gpr[%u] = (~a) + 1u;\n", inst->rD);
        if (inst->oe)
            fprintf(out, "        ppc_set_xer_ov(ctx, a == 0x80000000u);\n");
        emit_record_if_needed(out, inst, inst->rD);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_MULLW:
    case PPC_OP_MULLWO:
        fprintf(out, "    {\n");
        fprintf(out, "        s64 product = (s64)(s32)ctx->gpr[%u] * (s64)(s32)ctx->gpr[%u];\n",
                inst->rA, inst->rB);
        fprintf(out, "        ctx->gpr[%u] = (u32)product;\n", inst->rD);
        if (inst->oe)
            fprintf(out, "        ppc_set_xer_ov(ctx, product < -0x80000000ll || product > 0x7fffffffll);\n");
        emit_record_if_needed(out, inst, inst->rD);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_MULHW:
        fprintf(out, "    {\n");
        fprintf(out, "        s64 product = (s64)(s32)ctx->gpr[%u] * (s64)(s32)ctx->gpr[%u];\n",
                inst->rA, inst->rB);
        fprintf(out, "        ctx->gpr[%u] = (u32)(product >> 32);\n", inst->rD);
        emit_record_if_needed(out, inst, inst->rD);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_MULHWU:
        fprintf(out, "    {\n");
        fprintf(out, "        u64 product = (u64)ctx->gpr[%u] * (u64)ctx->gpr[%u];\n",
                inst->rA, inst->rB);
        fprintf(out, "        ctx->gpr[%u] = (u32)(product >> 32);\n", inst->rD);
        emit_record_if_needed(out, inst, inst->rD);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_DIVW:
    case PPC_OP_DIVWO:
        fprintf(out, "    {\n");
        fprintf(out, "        s32 dividend = (s32)ctx->gpr[%u];\n", inst->rA);
        fprintf(out, "        s32 divisor = (s32)ctx->gpr[%u];\n", inst->rB);
        fprintf(out, "        bool ov = divisor == 0 || ((u32)dividend == 0x80000000u && divisor == -1);\n");
        fprintf(out, "        ctx->gpr[%u] = ov ? ((dividend < 0) ? 0xFFFFFFFFu : 0u) : (u32)(dividend / divisor);\n",
                inst->rD);
        if (inst->oe)
            fprintf(out, "        ppc_set_xer_ov(ctx, ov);\n");
        emit_record_if_needed(out, inst, inst->rD);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_DIVWU:
    case PPC_OP_DIVWUO:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 divisor = ctx->gpr[%u];\n", inst->rB);
        fprintf(out, "        ctx->gpr[%u] = divisor == 0 ? 0u : ctx->gpr[%u] / divisor;\n",
                inst->rD, inst->rA);
        if (inst->oe)
            fprintf(out, "        ppc_set_xer_ov(ctx, divisor == 0);\n");
        emit_record_if_needed(out, inst, inst->rD);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_AND:
    case PPC_OP_ANDC:
    case PPC_OP_OR:
    case PPC_OP_ORC:
    case PPC_OP_XOR:
    case PPC_OP_NAND:
    case PPC_OP_NOR:
    case PPC_OP_EQV: {
        const char* expr = NULL;
        switch (inst->op) {
        case PPC_OP_AND:  expr = "ctx->gpr[%u] & ctx->gpr[%u]"; break;
        case PPC_OP_ANDC: expr = "ctx->gpr[%u] & ~ctx->gpr[%u]"; break;
        case PPC_OP_OR:   expr = "ctx->gpr[%u] | ctx->gpr[%u]"; break;
        case PPC_OP_ORC:  expr = "ctx->gpr[%u] | ~ctx->gpr[%u]"; break;
        case PPC_OP_XOR:  expr = "ctx->gpr[%u] ^ ctx->gpr[%u]"; break;
        case PPC_OP_NAND: expr = "~(ctx->gpr[%u] & ctx->gpr[%u])"; break;
        case PPC_OP_NOR:  expr = "~(ctx->gpr[%u] | ctx->gpr[%u])"; break;
        default:          expr = "~(ctx->gpr[%u] ^ ctx->gpr[%u])"; break;
        }
        fprintf(out, "    {\n");
        fprintf(out, "        ctx->gpr[%u] = ", inst->rA);
        fprintf(out, expr, inst->rS, inst->rB);
        fprintf(out, ";\n");
        emit_record_if_needed(out, inst, inst->rA);
        fprintf(out, "    }\n");
        break;
    }

    case PPC_OP_CNTLZW:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 v = ctx->gpr[%u];\n", inst->rS);
        fprintf(out, "        u32 n = 0;\n");
        fprintf(out, "        while (n < 32 && ((v & (0x80000000u >> n)) == 0)) n++;\n");
        fprintf(out, "        ctx->gpr[%u] = n;\n", inst->rA);
        emit_record_if_needed(out, inst, inst->rA);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_EXTSB:
        fprintf(out, "    {\n");
        fprintf(out, "        ctx->gpr[%u] = (u32)(s32)(s8)ctx->gpr[%u];\n",
                inst->rA, inst->rS);
        emit_record_if_needed(out, inst, inst->rA);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_EXTSH:
        fprintf(out, "    {\n");
        fprintf(out, "        ctx->gpr[%u] = (u32)(s32)(s16)ctx->gpr[%u];\n",
                inst->rA, inst->rS);
        emit_record_if_needed(out, inst, inst->rA);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_SLW:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 sh = ctx->gpr[%u] & 0x3Fu;\n", inst->rB);
        fprintf(out, "        ctx->gpr[%u] = sh > 31 ? 0u : (ctx->gpr[%u] << sh);\n",
                inst->rA, inst->rS);
        emit_record_if_needed(out, inst, inst->rA);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_SRW:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 sh = ctx->gpr[%u] & 0x3Fu;\n", inst->rB);
        fprintf(out, "        ctx->gpr[%u] = sh > 31 ? 0u : (ctx->gpr[%u] >> sh);\n",
                inst->rA, inst->rS);
        emit_record_if_needed(out, inst, inst->rA);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_SRAW:
    case PPC_OP_SRAWI:
        fprintf(out, "    {\n");
        if (inst->op == PPC_OP_SRAWI) {
            fprintf(out, "        u32 sh = %uu;\n", inst->sh);
        } else {
            fprintf(out, "        u32 sh = ctx->gpr[%u] & 0x3Fu;\n", inst->rB);
        }
        fprintf(out, "        u32 value = ctx->gpr[%u];\n", inst->rS);
        fprintf(out, "        bool ca = false;\n");
        fprintf(out, "        if (sh == 0) {\n");
        fprintf(out, "            ctx->gpr[%u] = value;\n", inst->rA);
        fprintf(out, "        } else if (sh > 31) {\n");
        fprintf(out, "            ctx->gpr[%u] = (value & 0x80000000u) ? 0xFFFFFFFFu : 0u;\n", inst->rA);
        fprintf(out, "            ca = (value & 0x80000000u) != 0;\n");
        fprintf(out, "        } else {\n");
        fprintf(out, "            ctx->gpr[%u] = (u32)((s32)value >> sh);\n", inst->rA);
        fprintf(out, "            ca = (value & 0x80000000u) && ((value << (32u - sh)) != 0);\n");
        fprintf(out, "        }\n");
        if (!s_ca_dead_here) {
            fprintf(out, "#ifndef RECOMP_NO_CA\n");
            fprintf(out, "        ctx->xer = (ctx->xer & ~0x20000000u) | (ca ? 0x20000000u : 0u);\n");
            fprintf(out, "#endif\n");
        }
        fprintf(out, "        PPC_CA_WRITE();\n");
        emit_record_if_needed(out, inst, inst->rA);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_RLWINM:
        {
            u32 mask = ppc_mask32(inst->mb, inst->me);
            fprintf(out, "    {\n");
            fprintf(out, "        ctx->gpr[%u] = dolrecomp_rotl32(ctx->gpr[%u], %uu) & 0x%08Xu;\n",
                    inst->rA, inst->rS, inst->sh, mask);
            emit_record_if_needed(out, inst, inst->rA);
            fprintf(out, "    }\n");
        }
        break;

    case PPC_OP_RLWNM:
        {
            u32 mask = ppc_mask32(inst->mb, inst->me);
            fprintf(out, "    {\n");
            fprintf(out, "        ctx->gpr[%u] = dolrecomp_rotl32(ctx->gpr[%u], ctx->gpr[%u]) & 0x%08Xu;\n",
                    inst->rA, inst->rS, inst->rB, mask);
            emit_record_if_needed(out, inst, inst->rA);
            fprintf(out, "    }\n");
        }
        break;

    case PPC_OP_RLWIMI:
        {
            u32 mask = ppc_mask32(inst->mb, inst->me);
            fprintf(out, "    {\n");
            fprintf(out, "        u32 rot = dolrecomp_rotl32(ctx->gpr[%u], %uu);\n",
                    inst->rS, inst->sh);
            fprintf(out, "        ctx->gpr[%u] = (ctx->gpr[%u] & ~0x%08Xu) | (rot & 0x%08Xu);\n",
                    inst->rA, inst->rA, mask, mask);
            emit_record_if_needed(out, inst, inst->rA);
            fprintf(out, "    }\n");
        }
        break;

    case PPC_OP_FADDS:
        // Gekko single-precision scalar arithmetic broadcasts the result to
        // BOTH paired-single lanes (Dolphin: ps[FD].Fill(result)). Mirror ps1
        // so a later psq_st reads the correct second lane (fixes warped geometry).
        fprintf(out, "    ctx->fpr[%u] = ctx->ps1[%u] = (f64)(f32)(ctx->fpr[%u] + ctx->fpr[%u]);\n",
                inst->rD, inst->rD, inst->rA, inst->rB);
        fprintf(out, "    PPC_FPRF_TAG(0x%08Xu);\n", inst->address);
        fprintf(out, "    dolrecomp_fprf_s(ctx, (f32)ctx->fpr[%u]);\n", inst->rD);
        break;

    case PPC_OP_FSUBS:
        fprintf(out, "    ctx->fpr[%u] = ctx->ps1[%u] = (f64)(f32)(ctx->fpr[%u] - ctx->fpr[%u]);\n",
                inst->rD, inst->rD, inst->rA, inst->rB);
        fprintf(out, "    PPC_FPRF_TAG(0x%08Xu);\n", inst->address);
        fprintf(out, "    dolrecomp_fprf_s(ctx, (f32)ctx->fpr[%u]);\n", inst->rD);
        break;

    case PPC_OP_FMULS:
        fprintf(out, "    ctx->fpr[%u] = ctx->ps1[%u] = (f64)(f32)(ctx->fpr[%u] * dolrecomp_force25(ctx->fpr[%u]));\n",
                inst->rD, inst->rD, inst->rA, inst->rC);
        fprintf(out, "    PPC_FPRF_TAG(0x%08Xu);\n", inst->address);
        fprintf(out, "    dolrecomp_fprf_s(ctx, (f32)ctx->fpr[%u]);\n", inst->rD);
        break;

    case PPC_OP_FDIVS:
        fprintf(out, "    ctx->fpr[%u] = ctx->ps1[%u] = (f64)(f32)(ctx->fpr[%u] / ctx->fpr[%u]);\n",
                inst->rD, inst->rD, inst->rA, inst->rB);
        fprintf(out, "    PPC_FPRF_TAG(0x%08Xu);\n", inst->address);
        fprintf(out, "    dolrecomp_fprf_s(ctx, (f32)ctx->fpr[%u]);\n", inst->rD);
        break;

    case PPC_OP_FRES:
        fprintf(out, "    { f64 result; if (ppc_fres(ctx, ctx->fpr[%u], &result)) ctx->fpr[%u] = ctx->ps1[%u] = result; }\n",
                inst->rB, inst->rD, inst->rD);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_FMADDS:
    case PPC_OP_FMSUBS:
    case PPC_OP_FNMADDS:
    case PPC_OP_FNMSUBS: {
        const bool sub = inst->op == PPC_OP_FMSUBS || inst->op == PPC_OP_FNMSUBS;
        const bool neg = inst->op == PPC_OP_FNMADDS || inst->op == PPC_OP_FNMSUBS;
        fprintf(out, "    {\n");
        fprintf(out, "        f64 result;\n");
        fprintf(out, "        if (ppc_fma(ctx, ctx->fpr[%u], ctx->fpr[%u], ctx->fpr[%u], true, %s, %s, &result))\n",
                inst->rA, inst->rC, inst->rB, sub ? "true" : "false", neg ? "true" : "false");
        fprintf(out, "            ctx->fpr[%u] = ctx->ps1[%u] = result;\n", inst->rD, inst->rD);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        fprintf(out, "    }\n");
        break;
    }

    case PPC_OP_FADD:
        fprintf(out, "    ctx->fpr[%u] = ctx->fpr[%u] + ctx->fpr[%u];\n",
                inst->rD, inst->rA, inst->rB);
        fprintf(out, "    PPC_FPRF_TAG(0x%08Xu);\n", inst->address);
        fprintf(out, "    dolrecomp_fprf_d(ctx, ctx->fpr[%u]);\n", inst->rD);
        break;

    case PPC_OP_FSUB:
        fprintf(out, "    ctx->fpr[%u] = ctx->fpr[%u] - ctx->fpr[%u];\n",
                inst->rD, inst->rA, inst->rB);
        fprintf(out, "    PPC_FPRF_TAG(0x%08Xu);\n", inst->address);
        fprintf(out, "    dolrecomp_fprf_d(ctx, ctx->fpr[%u]);\n", inst->rD);
        break;

    case PPC_OP_FMUL:
        fprintf(out, "    ctx->fpr[%u] = ctx->fpr[%u] * ctx->fpr[%u];\n",
                inst->rD, inst->rA, inst->rC);
        fprintf(out, "    PPC_FPRF_TAG(0x%08Xu);\n", inst->address);
        fprintf(out, "    dolrecomp_fprf_d(ctx, ctx->fpr[%u]);\n", inst->rD);
        break;

    case PPC_OP_FDIV:
        fprintf(out, "    ctx->fpr[%u] = ctx->fpr[%u] / ctx->fpr[%u];\n",
                inst->rD, inst->rA, inst->rB);
        fprintf(out, "    PPC_FPRF_TAG(0x%08Xu);\n", inst->address);
        fprintf(out, "    dolrecomp_fprf_d(ctx, ctx->fpr[%u]);\n", inst->rD);
        break;

    case PPC_OP_FRSQRTE:
        fprintf(out, "    { f64 result; if (ppc_frsqrte(ctx, ctx->fpr[%u], &result)) ctx->fpr[%u] = result; }\n",
                inst->rB, inst->rD);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_FMADD:
    case PPC_OP_FMSUB:
    case PPC_OP_FNMADD:
    case PPC_OP_FNMSUB: {
        const bool sub = inst->op == PPC_OP_FMSUB || inst->op == PPC_OP_FNMSUB;
        const bool neg = inst->op == PPC_OP_FNMADD || inst->op == PPC_OP_FNMSUB;
        fprintf(out, "    {\n");
        fprintf(out, "        f64 result;\n");
        fprintf(out, "        if (ppc_fma(ctx, ctx->fpr[%u], ctx->fpr[%u], ctx->fpr[%u], false, %s, %s, &result))\n",
                inst->rA, inst->rC, inst->rB, sub ? "true" : "false", neg ? "true" : "false");
        fprintf(out, "            ctx->fpr[%u] = result;\n", inst->rD);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        fprintf(out, "    }\n");
        break;
    }

    case PPC_OP_FCTIW:
    case PPC_OP_FCTIWZ:
        fprintf(out, "    { u64 result; if (ppc_fctiw(ctx, ctx->fpr[%u], %s, &result)) ctx->fpr[%u] = dolrecomp_f64_from_bits(result); }\n",
                inst->rB, inst->op == PPC_OP_FCTIWZ ? "true" : "false", inst->rD);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_FMR:
        fprintf(out, "    ctx->fpr[%u] = ctx->fpr[%u];\n", inst->rD, inst->rB);
        break;

    case PPC_OP_FNEG:
        fprintf(out, "    ctx->fpr[%u] = dolrecomp_f64_from_bits(dolrecomp_f64_to_bits(ctx->fpr[%u]) ^ 0x8000000000000000ull);\n",
                inst->rD, inst->rB);
        break;

    case PPC_OP_FABS:
        fprintf(out, "    ctx->fpr[%u] = dolrecomp_f64_from_bits(dolrecomp_f64_to_bits(ctx->fpr[%u]) & 0x7FFFFFFFFFFFFFFFull);\n",
                inst->rD, inst->rB);
        break;

    case PPC_OP_FNABS:
        fprintf(out, "    ctx->fpr[%u] = dolrecomp_f64_from_bits(dolrecomp_f64_to_bits(ctx->fpr[%u]) | 0x8000000000000000ull);\n",
                inst->rD, inst->rB);
        break;

    case PPC_OP_FRSP:
        // frsp broadcasts to both lanes too (Dolphin: ps[FD].Fill(rounded)).
        fprintf(out, "    ctx->fpr[%u] = ctx->ps1[%u] = (f64)(f32)ctx->fpr[%u];\n",
                inst->rD, inst->rD, inst->rB);
        fprintf(out, "    PPC_FPRF_TAG(0x%08Xu);\n", inst->address);
        fprintf(out, "    dolrecomp_fprf_s(ctx, (f32)ctx->fpr[%u]);\n", inst->rD);
        break;

    case PPC_OP_FSEL:
        fprintf(out, "    {\n");
        fprintf(out, "        ctx->fpr[%u] = (ctx->fpr[%u] >= 0.0) ? ctx->fpr[%u] : ctx->fpr[%u];\n",
                inst->rD, inst->rA, inst->rC, inst->rB);
        if (inst->rc) {
            emit_set_cr1_from_fpscr(out);
        }
        fprintf(out, "    }\n");
        break;

    case PPC_OP_MTFSB0:
    case PPC_OP_MTFSB1:
        fprintf(out, "    {\n");
        /* Only PPC bits 15-19 land in FPRF (u32 bits 12-16). rD is known here,
         * so drop the pending classification for exactly those and leave it
         * alone for every other bit. */
        if (inst->rD >= 15u && inst->rD <= 19u)
            fprintf(out, "        ppc_fprf_drop();\n");
        fprintf(out, "        u32 mask = 0x80000000u >> %u;\n", inst->rD);
        if (inst->op == PPC_OP_MTFSB0) {
            fprintf(out, "        if (%u != 1 && %u != 2) ctx->fpscr &= ~mask;\n",
                    inst->rD, inst->rD);
        } else {
            fprintf(out, "        if (%u != 1 && %u != 2) ctx->fpscr |= mask;\n",
                    inst->rD, inst->rD);
        }
        if (inst->rc) {
            emit_set_cr1_from_fpscr(out);
        }
        fprintf(out, "    }\n");
        break;

    case PPC_OP_MFFS:
        fprintf(out, "    PPC_FPRF_READ();\n");
        fprintf(out, "    ppc_fprf_flush(ctx);\n");
        fprintf(out, "    PPC_FPRF_TRACE_MFFS(ctx);\n");
        fprintf(out, "    ctx->fpr[%u] = dolrecomp_f64_from_bits(0xFFF8000000000000ull | ctx->fpscr);\n", inst->rD);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_MCRFS: {
        u32 shift = cr_field_shift(inst->crfS);
        fprintf(out, "    {\n");
        fprintf(out, "        PPC_FPRF_READ();\n");
        fprintf(out, "        ppc_fprf_flush(ctx);\n");
        fprintf(out, "        u32 field = (ctx->fpscr >> %u) & 0xFu;\n", shift);
        fprintf(out, "        ctx->fpscr &= ~((0xFu << %u) & 0x83F80700u);\n", shift);
        fprintf(out, "        ppc_fpscr_updated(ctx);\n");
        fprintf(out, "        ctx->crf[%u] = (u8)field;\n", (u32)inst->crfD);
        fprintf(out, "        PPC_CR_WRITE(%u);\n", (u32)inst->crfD);
        fprintf(out, "    }\n");
        break;
    }

    case PPC_OP_MTFSFI: {
        u32 shift = cr_field_shift(inst->crfD);
        fprintf(out, "    ppc_fprf_drop();\n");
        fprintf(out, "    ctx->fpscr = (ctx->fpscr & ~(0xFu << %u)) | (0x%Xu << %u);\n",
                shift, inst->imm, shift);
        fprintf(out, "    ppc_fpscr_updated(ctx);\n");
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;
    }

    case PPC_OP_MTFSF:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 mask = 0;\n");
        fprintf(out, "        for (u32 i = 0; i < 8; i++) if (0x%02Xu & (1u << i)) mask |= 0xFu << (i * 4);\n", inst->fm);
        fprintf(out, "        u32 source = (u32)dolrecomp_f64_to_bits(ctx->fpr[%u]);\n", inst->rB);
        fprintf(out, "        ppc_fprf_drop();\n");
        fprintf(out, "        ctx->fpscr = (ctx->fpscr & ~mask) | (source & mask);\n");
        fprintf(out, "        ppc_fpscr_updated(ctx);\n");
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_PS_ADD:
        fprintf(out, "    {\n");
        fprintf(out, "        ctx->fpr[%u] = dolrecomp_ps_round((f32)ctx->fpr[%u] + (f32)ctx->fpr[%u]);\n",
                inst->rD, inst->rA, inst->rB);
        fprintf(out, "        ctx->ps1[%u] = dolrecomp_ps_round((f32)ctx->ps1[%u] + (f32)ctx->ps1[%u]);\n",
                inst->rD, inst->rA, inst->rB);
        fprintf(out, "        PPC_FPRF_TAG(0x%08Xu);\n", inst->address);
        fprintf(out, "        dolrecomp_fprf_s(ctx, (f32)ctx->fpr[%u]);\n", inst->rD);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_PS_SUB:
        fprintf(out, "    {\n");
        fprintf(out, "        ctx->fpr[%u] = dolrecomp_ps_round((f32)ctx->fpr[%u] - (f32)ctx->fpr[%u]);\n",
                inst->rD, inst->rA, inst->rB);
        fprintf(out, "        ctx->ps1[%u] = dolrecomp_ps_round((f32)ctx->ps1[%u] - (f32)ctx->ps1[%u]);\n",
                inst->rD, inst->rA, inst->rB);
        fprintf(out, "        PPC_FPRF_TAG(0x%08Xu);\n", inst->address);
        fprintf(out, "        dolrecomp_fprf_s(ctx, (f32)ctx->fpr[%u]);\n", inst->rD);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_PS_MUL:
        fprintf(out, "    {\n");
        fprintf(out, "        ctx->fpr[%u] = dolrecomp_ps_round(ctx->fpr[%u] * dolrecomp_force25(ctx->fpr[%u]));\n",
                inst->rD, inst->rA, inst->rC);
        fprintf(out, "        ctx->ps1[%u] = dolrecomp_ps_round(ctx->ps1[%u] * dolrecomp_force25(ctx->ps1[%u]));\n",
                inst->rD, inst->rA, inst->rC);
        fprintf(out, "        PPC_FPRF_TAG(0x%08Xu);\n", inst->address);
        fprintf(out, "        dolrecomp_fprf_s(ctx, (f32)ctx->fpr[%u]);\n", inst->rD);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_PS_DIV:
        fprintf(out, "    {\n");
        fprintf(out, "        ctx->fpr[%u] = dolrecomp_ps_round((f32)ctx->fpr[%u] / (f32)ctx->fpr[%u]);\n",
                inst->rD, inst->rA, inst->rB);
        fprintf(out, "        ctx->ps1[%u] = dolrecomp_ps_round((f32)ctx->ps1[%u] / (f32)ctx->ps1[%u]);\n",
                inst->rD, inst->rA, inst->rB);
        fprintf(out, "        PPC_FPRF_TAG(0x%08Xu);\n", inst->address);
        fprintf(out, "        dolrecomp_fprf_s(ctx, (f32)ctx->fpr[%u]);\n", inst->rD);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_PS_RES:
        fprintf(out, "    { f64 a, b; ppc_ps_res(ctx, ctx->fpr[%u], ctx->ps1[%u], &a, &b); ctx->fpr[%u] = dolrecomp_ps_round(a); ctx->ps1[%u] = dolrecomp_ps_round(b); }\n",
                inst->rB, inst->rB, inst->rD, inst->rD);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_RSQRTE:
        fprintf(out, "    { f64 a, b; ppc_ps_rsqrte(ctx, ctx->fpr[%u], ctx->ps1[%u], &a, &b); ctx->fpr[%u] = dolrecomp_ps_round(a); ctx->ps1[%u] = dolrecomp_ps_round(b); }\n",
                inst->rB, inst->rB, inst->rD, inst->rD);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_MADD:
    case PPC_OP_PS_MSUB:
    case PPC_OP_PS_NMADD:
    case PPC_OP_PS_NMSUB:
        fprintf(out, "    {\n");
        fprintf(out, "        f32 ps0 = (f32)ctx->fpr[%u] * (f32)ctx->fpr[%u];\n",
                inst->rA, inst->rC);
        fprintf(out, "        f32 ps1 = (f32)ctx->ps1[%u] * (f32)ctx->ps1[%u];\n",
                inst->rA, inst->rC);
        if (inst->op == PPC_OP_PS_MADD || inst->op == PPC_OP_PS_NMADD) {
            fprintf(out, "        ps0 += (f32)ctx->fpr[%u];\n", inst->rB);
            fprintf(out, "        ps1 += (f32)ctx->ps1[%u];\n", inst->rB);
        } else {
            fprintf(out, "        ps0 -= (f32)ctx->fpr[%u];\n", inst->rB);
            fprintf(out, "        ps1 -= (f32)ctx->ps1[%u];\n", inst->rB);
        }
        if (inst->op == PPC_OP_PS_NMADD || inst->op == PPC_OP_PS_NMSUB) {
            fprintf(out, "        ps0 = -ps0;\n");
            fprintf(out, "        ps1 = -ps1;\n");
        }
        fprintf(out, "        ctx->fpr[%u] = dolrecomp_ps_round(ps0);\n", inst->rD);
        fprintf(out, "        ctx->ps1[%u] = dolrecomp_ps_round(ps1);\n", inst->rD);
        fprintf(out, "        PPC_FPRF_TAG(0x%08Xu);\n", inst->address);
        fprintf(out, "        dolrecomp_fprf_s(ctx, (f32)ctx->fpr[%u]);\n", inst->rD);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_PS_NEG:
        fprintf(out, "    ctx->fpr[%u] = dolrecomp_ps_from_bits(dolrecomp_ps_to_bits(ctx->fpr[%u]) ^ 0x80000000u);\n",
                inst->rD, inst->rB);
        fprintf(out, "    ctx->ps1[%u] = dolrecomp_ps_from_bits(dolrecomp_ps_to_bits(ctx->ps1[%u]) ^ 0x80000000u);\n",
                inst->rD, inst->rB);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_ABS:
        fprintf(out, "    ctx->fpr[%u] = dolrecomp_ps_from_bits(dolrecomp_ps_to_bits(ctx->fpr[%u]) & 0x7FFFFFFFu);\n",
                inst->rD, inst->rB);
        fprintf(out, "    ctx->ps1[%u] = dolrecomp_ps_from_bits(dolrecomp_ps_to_bits(ctx->ps1[%u]) & 0x7FFFFFFFu);\n",
                inst->rD, inst->rB);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_NABS:
        fprintf(out, "    ctx->fpr[%u] = dolrecomp_ps_from_bits(dolrecomp_ps_to_bits(ctx->fpr[%u]) | 0x80000000u);\n",
                inst->rD, inst->rB);
        fprintf(out, "    ctx->ps1[%u] = dolrecomp_ps_from_bits(dolrecomp_ps_to_bits(ctx->ps1[%u]) | 0x80000000u);\n",
                inst->rD, inst->rB);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_MR:
        fprintf(out, "    ctx->fpr[%u] = ctx->fpr[%u];\n", inst->rD, inst->rB);
        fprintf(out, "    ctx->ps1[%u] = ctx->ps1[%u];\n", inst->rD, inst->rB);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    // Cross-lane paired-single ops read one register's ps0 lane while writing
    // another's, so when the destination aliases a source the two sequential
    // stores would clobber a lane before it is read. Stage both results in
    // temporaries first (matrix-transform idioms routinely reuse D==C / D==A).
    case PPC_OP_PS_SUM0:
        fprintf(out, "    { f64 d0 = dolrecomp_ps_round((f32)ctx->fpr[%u] + (f32)ctx->ps1[%u]);\n",
                inst->rA, inst->rB);
        fprintf(out, "      f64 d1 = dolrecomp_ps_round(ctx->ps1[%u]);\n", inst->rC);
        fprintf(out, "      ctx->fpr[%u] = d0; ctx->ps1[%u] = d1; }\n", inst->rD, inst->rD);
        fprintf(out, "        PPC_FPRF_TAG(0x%08Xu);\n", inst->address);
        fprintf(out, "        dolrecomp_fprf_s(ctx, (f32)ctx->fpr[%u]);\n", inst->rD);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_SUM1:
        fprintf(out, "    { f64 d0 = dolrecomp_ps_round(ctx->fpr[%u]);\n", inst->rC);
        fprintf(out, "      f64 d1 = dolrecomp_ps_round((f32)ctx->fpr[%u] + (f32)ctx->ps1[%u]);\n",
                inst->rA, inst->rB);
        fprintf(out, "      ctx->fpr[%u] = d0; ctx->ps1[%u] = d1; }\n", inst->rD, inst->rD);
        fprintf(out, "        PPC_FPRF_TAG(0x%08Xu);\n", inst->address);
        fprintf(out, "        dolrecomp_fprf_s(ctx, (f32)ctx->fpr[%u]);\n", inst->rD);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_MULS0:
        fprintf(out, "    { f64 d0 = dolrecomp_ps_round(ctx->fpr[%u] * dolrecomp_force25(ctx->fpr[%u]));\n",
                inst->rA, inst->rC);
        fprintf(out, "      f64 d1 = dolrecomp_ps_round(ctx->ps1[%u] * dolrecomp_force25(ctx->fpr[%u]));\n",
                inst->rA, inst->rC);
        fprintf(out, "      ctx->fpr[%u] = d0; ctx->ps1[%u] = d1; }\n", inst->rD, inst->rD);
        fprintf(out, "        PPC_FPRF_TAG(0x%08Xu);\n", inst->address);
        fprintf(out, "        dolrecomp_fprf_s(ctx, (f32)ctx->fpr[%u]);\n", inst->rD);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_MULS1:
        fprintf(out, "    { f64 d0 = dolrecomp_ps_round(ctx->fpr[%u] * dolrecomp_force25(ctx->ps1[%u]));\n",
                inst->rA, inst->rC);
        fprintf(out, "      f64 d1 = dolrecomp_ps_round(ctx->ps1[%u] * dolrecomp_force25(ctx->ps1[%u]));\n",
                inst->rA, inst->rC);
        fprintf(out, "      ctx->fpr[%u] = d0; ctx->ps1[%u] = d1; }\n", inst->rD, inst->rD);
        fprintf(out, "        PPC_FPRF_TAG(0x%08Xu);\n", inst->address);
        fprintf(out, "        dolrecomp_fprf_s(ctx, (f32)ctx->fpr[%u]);\n", inst->rD);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_MADDS0:
        fprintf(out, "    { f64 d0 = dolrecomp_ps_round((f32)ctx->fpr[%u] * (f32)ctx->fpr[%u] + (f32)ctx->fpr[%u]);\n",
                inst->rA, inst->rC, inst->rB);
        fprintf(out, "      f64 d1 = dolrecomp_ps_round((f32)ctx->ps1[%u] * (f32)ctx->fpr[%u] + (f32)ctx->ps1[%u]);\n",
                inst->rA, inst->rC, inst->rB);
        fprintf(out, "      ctx->fpr[%u] = d0; ctx->ps1[%u] = d1; }\n", inst->rD, inst->rD);
        fprintf(out, "        PPC_FPRF_TAG(0x%08Xu);\n", inst->address);
        fprintf(out, "        dolrecomp_fprf_s(ctx, (f32)ctx->fpr[%u]);\n", inst->rD);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_MADDS1:
        fprintf(out, "    { f64 d0 = dolrecomp_ps_round((f32)ctx->fpr[%u] * (f32)ctx->ps1[%u] + (f32)ctx->fpr[%u]);\n",
                inst->rA, inst->rC, inst->rB);
        fprintf(out, "      f64 d1 = dolrecomp_ps_round((f32)ctx->ps1[%u] * (f32)ctx->ps1[%u] + (f32)ctx->ps1[%u]);\n",
                inst->rA, inst->rC, inst->rB);
        fprintf(out, "      ctx->fpr[%u] = d0; ctx->ps1[%u] = d1; }\n", inst->rD, inst->rD);
        fprintf(out, "        PPC_FPRF_TAG(0x%08Xu);\n", inst->address);
        fprintf(out, "        dolrecomp_fprf_s(ctx, (f32)ctx->fpr[%u]);\n", inst->rD);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_MERGE00:
        fprintf(out, "    { f64 d0 = dolrecomp_ps_round(ctx->fpr[%u]);\n", inst->rA);
        fprintf(out, "      f64 d1 = dolrecomp_ps_round(ctx->fpr[%u]);\n", inst->rB);
        fprintf(out, "      ctx->fpr[%u] = d0; ctx->ps1[%u] = d1; }\n", inst->rD, inst->rD);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_MERGE01:
        fprintf(out, "    { f64 d0 = dolrecomp_ps_round(ctx->fpr[%u]);\n", inst->rA);
        fprintf(out, "      f64 d1 = dolrecomp_ps_round(ctx->ps1[%u]);\n", inst->rB);
        fprintf(out, "      ctx->fpr[%u] = d0; ctx->ps1[%u] = d1; }\n", inst->rD, inst->rD);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_MERGE10:
        fprintf(out, "    { f64 d0 = dolrecomp_ps_round(ctx->ps1[%u]);\n", inst->rA);
        fprintf(out, "      f64 d1 = dolrecomp_ps_round(ctx->fpr[%u]);\n", inst->rB);
        fprintf(out, "      ctx->fpr[%u] = d0; ctx->ps1[%u] = d1; }\n", inst->rD, inst->rD);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_MERGE11:
        fprintf(out, "    { f64 d0 = dolrecomp_ps_round(ctx->ps1[%u]);\n", inst->rA);
        fprintf(out, "      f64 d1 = dolrecomp_ps_round(ctx->ps1[%u]);\n", inst->rB);
        fprintf(out, "      ctx->fpr[%u] = d0; ctx->ps1[%u] = d1; }\n", inst->rD, inst->rD);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_CMPU0:
    case PPC_OP_PS_CMPO0:
    case PPC_OP_PS_CMPU1:
    case PPC_OP_PS_CMPO1:
        fprintf(out, "    {\n");
        if (inst->op == PPC_OP_PS_CMPU0 || inst->op == PPC_OP_PS_CMPO0) {
            fprintf(out, "        f32 val_a = (f32)ctx->fpr[%u];\n", inst->rA);
            fprintf(out, "        f32 val_b = (f32)ctx->fpr[%u];\n", inst->rB);
        } else {
            fprintf(out, "        f32 val_a = (f32)ctx->ps1[%u];\n", inst->rA);
            fprintf(out, "        f32 val_b = (f32)ctx->ps1[%u];\n", inst->rB);
        }
        fprintf(out, "        u32 cr_bits = 0;\n");
        fprintf(out, "        if (val_a < val_b)       cr_bits = 0x8u;\n");
        fprintf(out, "        else if (val_a > val_b)  cr_bits = 0x4u;\n");
        fprintf(out, "        else if (val_a == val_b) cr_bits = 0x2u;\n");
        fprintf(out, "        else                     cr_bits = 0x1u;\n");
        fprintf(out, "        ctx->crf[%u] = (u8)cr_bits;\n", (u32)inst->crfD);
        fprintf(out, "        PPC_CR_WRITE(%u);\n", (u32)inst->crfD);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_PS_SEL:
        fprintf(out, "    ctx->fpr[%u] = ((f32)ctx->fpr[%u] >= 0.0f) ? ctx->fpr[%u] : ctx->fpr[%u];\n",
                inst->rD, inst->rA, inst->rC, inst->rB);
        fprintf(out, "    ctx->ps1[%u] = ((f32)ctx->ps1[%u] >= 0.0f) ? ctx->ps1[%u] : ctx->ps1[%u];\n",
                inst->rD, inst->rA, inst->rC, inst->rB);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_FCMPU:
    case PPC_OP_FCMPO:
        emit_fcompare(out, inst);
        break;

    case PPC_OP_LWZ:  emit_load(out, inst, "mem_read32(ctx, ea)", false); break;
    case PPC_OP_LWZU: emit_load(out, inst, "mem_read32(ctx, ea)", true); break;
    case PPC_OP_LBZ:  emit_load(out, inst, "mem_read8(ctx, ea)", false); break;
    case PPC_OP_LBZU: emit_load(out, inst, "mem_read8(ctx, ea)", true); break;
    case PPC_OP_LHZ:  emit_load(out, inst, "mem_read16(ctx, ea)", false); break;
    case PPC_OP_LHZU: emit_load(out, inst, "mem_read16(ctx, ea)", true); break;
    case PPC_OP_LHA:  emit_load(out, inst, "(u32)(s32)(s16)mem_read16(ctx, ea)", false); break;
    case PPC_OP_LHAU: emit_load(out, inst, "(u32)(s32)(s16)mem_read16(ctx, ea)", true); break;

    case PPC_OP_LWZX:  emit_loadx(out, inst, "mem_read32(ctx, ea)", false); break;
    case PPC_OP_LWZUX: emit_loadx(out, inst, "mem_read32(ctx, ea)", true); break;
    case PPC_OP_LBZX:  emit_loadx(out, inst, "mem_read8(ctx, ea)", false); break;
    case PPC_OP_LBZUX: emit_loadx(out, inst, "mem_read8(ctx, ea)", true); break;
    case PPC_OP_LHZX:  emit_loadx(out, inst, "mem_read16(ctx, ea)", false); break;
    case PPC_OP_LHZUX: emit_loadx(out, inst, "mem_read16(ctx, ea)", true); break;
    case PPC_OP_LHAX:  emit_loadx(out, inst, "(u32)(s32)(s16)mem_read16(ctx, ea)", false); break;
    case PPC_OP_LHAUX: emit_loadx(out, inst, "(u32)(s32)(s16)mem_read16(ctx, ea)", true); break;
    case PPC_OP_LWBRX: emit_loadx(out, inst, "bswap32(mem_read32(ctx, ea))", false); break;
    case PPC_OP_LHBRX: emit_loadx(out, inst, "bswap16(mem_read16(ctx, ea))", false); break;

    case PPC_OP_LFS:   emit_fload(out, inst, true,  false); break;
    case PPC_OP_LFSU:  emit_fload(out, inst, true,  true); break;
    case PPC_OP_LFD:   emit_fload(out, inst, false, false); break;
    case PPC_OP_LFDU:  emit_fload(out, inst, false, true); break;

    case PPC_OP_LFSX:  emit_floadx(out, inst, true,  false); break;
    case PPC_OP_LFSUX: emit_floadx(out, inst, true,  true); break;
    case PPC_OP_LFDX:  emit_floadx(out, inst, false, false); break;
    case PPC_OP_LFDUX: emit_floadx(out, inst, false, true); break;

    case PPC_OP_PSQ_L:   emit_psq_load(out, inst, false, false); break;
    case PPC_OP_PSQ_LU:  emit_psq_load(out, inst, false, true); break;
    case PPC_OP_PSQ_LX:  emit_psq_load(out, inst, true,  false); break;
    case PPC_OP_PSQ_LUX: emit_psq_load(out, inst, true,  true); break;

    case PPC_OP_STW:  emit_store(out, inst, "mem_write32", "u32", false); break;
    case PPC_OP_STWU: emit_store(out, inst, "mem_write32", "u32", true); break;
    case PPC_OP_STB:  emit_store(out, inst, "mem_write8", "u8", false); break;
    case PPC_OP_STBU: emit_store(out, inst, "mem_write8", "u8", true); break;
    case PPC_OP_STH:  emit_store(out, inst, "mem_write16", "u16", false); break;
    case PPC_OP_STHU: emit_store(out, inst, "mem_write16", "u16", true); break;

    case PPC_OP_STWX:  emit_storex(out, inst, "mem_write32", "u32", false); break;
    case PPC_OP_STWUX: emit_storex(out, inst, "mem_write32", "u32", true); break;
    case PPC_OP_STBX:  emit_storex(out, inst, "mem_write8", "u8", false); break;
    case PPC_OP_STBUX: emit_storex(out, inst, "mem_write8", "u8", true); break;
    case PPC_OP_STHX:  emit_storex(out, inst, "mem_write16", "u16", false); break;
    case PPC_OP_STHUX: emit_storex(out, inst, "mem_write16", "u16", true); break;

    case PPC_OP_STFS:   emit_fstore(out, inst, true,  false); break;
    case PPC_OP_STFSU:  emit_fstore(out, inst, true,  true); break;
    case PPC_OP_STFD:   emit_fstore(out, inst, false, false); break;
    case PPC_OP_STFDU:  emit_fstore(out, inst, false, true); break;

    case PPC_OP_STFSX:  emit_fstorex(out, inst, true,  false); break;
    case PPC_OP_STFSUX: emit_fstorex(out, inst, true,  true); break;
    case PPC_OP_STFDX:  emit_fstorex(out, inst, false, false); break;
    case PPC_OP_STFDUX: emit_fstorex(out, inst, false, true); break;

    case PPC_OP_PSQ_ST:   emit_psq_store(out, inst, false, false); break;
    case PPC_OP_PSQ_STU:  emit_psq_store(out, inst, false, true); break;
    case PPC_OP_PSQ_STX:  emit_psq_store(out, inst, true,  false); break;
    case PPC_OP_PSQ_STUX: emit_psq_store(out, inst, true,  true); break;

    case PPC_OP_STWBRX:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 ea = ");
        emit_xform_ea(out, inst->rA, inst->rB, false);
        fprintf(out, ";\n");
        fprintf(out, "        mem_write32(ctx, ea, bswap32(ctx->gpr[%u]));\n", inst->rS);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_STHBRX:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 ea = ");
        emit_xform_ea(out, inst->rA, inst->rB, false);
        fprintf(out, ";\n");
        fprintf(out, "        mem_write16(ctx, ea, bswap16((u16)ctx->gpr[%u]));\n", inst->rS);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_LSWI:
    case PPC_OP_LSWX: {
        u32 count = inst->op == PPC_OP_LSWI ? (inst->nb ? inst->nb : 32u) : 0u;
        fprintf(out, "    {\n");
        if (inst->op == PPC_OP_LSWX) {
            fprintf(out, "        u32 ea = ctx->gpr[%u];\n", inst->rB);
            if (inst->rA)
                fprintf(out, "        ea += ctx->gpr[%u];\n", inst->rA);
            fprintf(out, "        u32 count = ctx->xer & 0x7Fu;\n");
            fprintf(out, "        u32 reg_count = (count + 3u) / 4u;\n");
            fprintf(out, "        for (u32 r = 0; r < reg_count; r++) {\n");
            fprintf(out, "            u32 reg = (%uu + r) & 31u;\n", inst->rD);
            fprintf(out, "            if (reg == %uu || reg == %uu) {\n", inst->rA, inst->rB);
            fprintf(out, "                ppc_program_exception(ctx, PPC_PROGRAM_ILLEGAL, 0x%08Xu);\n",
                    inst->address);
            fprintf(out, "                DOLRECOMP_RETURN;\n");
            fprintf(out, "            }\n");
            fprintf(out, "        }\n");
        } else {
            if (inst->rA) fprintf(out, "        u32 ea = ctx->gpr[%u];\n", inst->rA);
            else fprintf(out, "        u32 ea = 0u;\n");
            fprintf(out, "        u32 count = %uu;\n", count);
        }
        fprintf(out, "        for (u32 n = 0; n < count; n++) {\n");
        fprintf(out, "            u32 reg = (%uu + n / 4u) & 31u;\n", inst->rD);
        fprintf(out, "            if ((n & 3u) == 0) ctx->gpr[reg] = 0;\n");
        fprintf(out, "            ctx->gpr[reg] |= (u32)mem_read8(ctx, ea + n) << (24u - 8u * (n & 3u));\n");
        fprintf(out, "        }\n");
        fprintf(out, "    }\n");
        break;
    }

    case PPC_OP_STSWI:
    case PPC_OP_STSWX: {
        u32 count = inst->op == PPC_OP_STSWI ? (inst->nb ? inst->nb : 32u) : 0u;
        fprintf(out, "    {\n");
        if (inst->op == PPC_OP_STSWX) {
            fprintf(out, "        u32 ea = ctx->gpr[%u]", inst->rB);
            if (inst->rA) fprintf(out, " + ctx->gpr[%u]", inst->rA);
            fprintf(out, ";\n        u32 count = ctx->xer & 0x7Fu;\n");
        } else {
            if (inst->rA) fprintf(out, "        u32 ea = ctx->gpr[%u];\n", inst->rA);
            else fprintf(out, "        u32 ea = 0u;\n");
            fprintf(out, "        u32 count = %uu;\n", count);
        }
        fprintf(out, "        for (u32 n = 0; n < count; n++) {\n");
        fprintf(out, "            u32 reg = (%uu + n / 4u) & 31u;\n", inst->rS);
        fprintf(out, "            u8 value = (u8)(ctx->gpr[reg] >> (24u - 8u * (n & 3u)));\n");
        fprintf(out, "            mem_write8(ctx, ea + n, value);\n");
        fprintf(out, "        }\n");
        fprintf(out, "    }\n");
        break;
    }

    case PPC_OP_LWARX:
        fprintf(out, "    {\n        u32 ea = ");
        emit_xform_ea(out, inst->rA, inst->rB, false);
        fprintf(out, ";\n        ctx->gpr[%u] = mem_read32(ctx, ea);\n", inst->rD);
        fprintf(out, "        ctx->reserve_addr = ea;\n        ctx->reserve_valid = true;\n    }\n");
        break;

    case PPC_OP_STWCX:
        fprintf(out, "    {\n        u32 ea = ");
        emit_xform_ea(out, inst->rA, inst->rB, false);
        fprintf(out, ";\n        bool success = ctx->reserve_valid;\n");
        fprintf(out, "        ctx->reserve_valid = false;\n");
        fprintf(out, "        if (success) mem_write32(ctx, ea, ctx->gpr[%u]);\n", inst->rS);
        fprintf(out, "        ctx->crf[0] = (u8)((success ? 2u : 0u) | (ctx->xer >> 31));\n");
        fprintf(out, "        PPC_CR_WRITE(0);\n");
        fprintf(out, "    }\n");
        break;

    case PPC_OP_STFIWX:
        fprintf(out, "    {\n        u32 ea = ");
        emit_xform_ea(out, inst->rA, inst->rB, false);
        fprintf(out, ";\n        mem_write32(ctx, ea, (u32)dolrecomp_f64_to_bits(ctx->fpr[%u]));\n    }\n", inst->rS);
        break;

    case PPC_OP_DCBZ:
        emit_dcbz(out, inst);
        break;

    case PPC_OP_DCBZ_L:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 ea = ");
        emit_xform_ea(out, inst->rA, inst->rB, false);
        fprintf(out, ";\n");
        fprintf(out, "        ppc_dcbz_l(ctx, ea, 0x%08Xu);\n", inst->address);
        emit_exc_check_return(out, "        ");
        fprintf(out, "    }\n");
        break;

    case PPC_OP_DCBST:
    case PPC_OP_DCBF:
    case PPC_OP_DCBI:
        // Data-cache management ops have no effect in the flat-memory model
        // (stores go straight to RAM; there is no data cache to flush/invalidate).
        // Emit as a no-op instead of a per-instruction fallback round-trip:
        // streaming code runs these thousands of times per frame, and the old
        // fallback path also ended the block (extra dispatcher churn). SMC
        // coherency is unaffected — instruction-cache invalidation is icbi's job
        // (below), and the chassis re-verifies chunk hashes on invalidation.
        break;

    case PPC_OP_ICBI:
        fprintf(out, "    DOLRECOMP_DC_FLUSH(ctx);\n");
        fprintf(out, "    ppc_fallback_instruction(ctx, 0x%08Xu, 0x%08Xu);\n",
                inst->raw, inst->address);
        fprintf(out, "    DOLRECOMP_DC_RELOAD(ctx);\n");
        fprintf(out, "    DOLRECOMP_RETURN;\n");
        break;

    case PPC_OP_DCBTST:
    case PPC_OP_DCBT:
        fprintf(out, "    (void)ctx;\n");
        break;

    case PPC_OP_LMW:
        fprintf(out, "    {\n");
        emit_ea_open(out, inst->rA, false);
        emit_dform_ea(out, inst->rA, inst->simm, false);
        fprintf(out, ";\n");
        fprintf(out, "        for (u32 r = %u; r < 32; r++, ea += 4) ctx->gpr[r] = mem_read32(ctx, ea);\n",
                inst->rD);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_STMW:
        fprintf(out, "    {\n");
        emit_ea_open(out, inst->rA, false);
        emit_dform_ea(out, inst->rA, inst->simm, false);
        fprintf(out, ";\n");
        fprintf(out, "        for (u32 r = %u; r < 32; r++, ea += 4) mem_write32(ctx, ea, ctx->gpr[r]);\n",
                inst->rS);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_B:
        fprintf(out, "    {\n");
        emit_direct_branch(out, inst,
                           branch_target_is_local(func_start, func_end, inst->branch_target),
                           func_start, func_end);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_BC:
        fprintf(out, "    {\n");
        emit_branch_condition(out, inst->bo, inst->bi);
        fprintf(out, "        if (ctr_ok && cr_ok) {\n");
        emit_direct_branch(out, inst,
                           branch_target_is_local(func_start, func_end, inst->branch_target),
                           func_start, func_end);
        fprintf(out, "        }\n");
        fprintf(out, "    }\n");
        break;

    case PPC_OP_BCLR:
        emit_dynamic_branch(out, inst, "ctx->lr & ~3u");
        break;

    case PPC_OP_BCCTR:
        emit_dynamic_branch(out, inst, "ctx->ctr & ~3u");
        break;

    case PPC_OP_TWI:
        fprintf(out, "    if (ppc_trap_condition(%uu, ctx->gpr[%u], (u32)(s32)%d)) {\n",
                inst->to, inst->rA, (int)inst->simm);
        fprintf(out, "        ppc_program_exception(ctx, PPC_PROGRAM_TRAP, 0x%08Xu);\n", inst->address);
        fprintf(out, "        DOLRECOMP_RETURN;\n");
        fprintf(out, "    }\n");
        break;

    case PPC_OP_TW:
        fprintf(out, "    if (ppc_trap_condition(%uu, ctx->gpr[%u], ctx->gpr[%u])) {\n",
                inst->to, inst->rA, inst->rB);
        fprintf(out, "        ppc_program_exception(ctx, PPC_PROGRAM_TRAP, 0x%08Xu);\n", inst->address);
        fprintf(out, "        DOLRECOMP_RETURN;\n");
        fprintf(out, "    }\n");
        break;

    case PPC_OP_SC:
        fprintf(out, "    ppc_system_call_exception(ctx, 0x%08Xu);\n", inst->address);
        fprintf(out, "    DOLRECOMP_RETURN;\n");
        break;

    case PPC_OP_RFI:
        fprintf(out, "    ppc_rfi(ctx, 0x%08Xu);\n", inst->address);
        fprintf(out, "    DOLRECOMP_RETURN;\n");
        break;

    case PPC_OP_CRAND:  emit_cr_logical(out, inst, "a & b"); break;
    case PPC_OP_CRANDC: emit_cr_logical(out, inst, "a & ~b"); break;
    case PPC_OP_CREQV:  emit_cr_logical(out, inst, "~(a ^ b)"); break;
    case PPC_OP_CRNAND: emit_cr_logical(out, inst, "~(a & b)"); break;
    case PPC_OP_CRNOR:  emit_cr_logical(out, inst, "~(a | b)"); break;
    case PPC_OP_CROR:   emit_cr_logical(out, inst, "a | b"); break;
    case PPC_OP_CRORC:  emit_cr_logical(out, inst, "a | ~b"); break;
    case PPC_OP_CRXOR:  emit_cr_logical(out, inst, "a ^ b"); break;

    case PPC_OP_MCRF: {
        fprintf(out, "    {\n");
        fprintf(out, "        PPC_CR_READ(%uu);\n", 1u << inst->crfS);
        fprintf(out, "        ctx->crf[%u] = ctx->crf[%u];\n", (u32)inst->crfD, (u32)inst->crfS);
        fprintf(out, "        PPC_CR_WRITE(%u);\n", (u32)inst->crfD);
        fprintf(out, "    }\n");
        break;
    }

    case PPC_OP_MCRXR: {
        fprintf(out, "    {\n");
        fprintf(out, "        PPC_CA_READ();\n");
        fprintf(out, "        ctx->crf[%u] = (u8)(ctx->xer >> 28);\n", (u32)inst->crfD);
        fprintf(out, "        PPC_CR_WRITE(%u);\n", (u32)inst->crfD);
        fprintf(out, "        ctx->xer &= ~0xE0000000u;\n");
        fprintf(out, "    }\n");
        break;
    }

    case PPC_OP_MFCR:
        fprintf(out, "    PPC_CR_READ(0xFFu);\n");
        fprintf(out, "    ctx->gpr[%u] = cpu_cr_get(ctx);\n", inst->rD);
        break;

    case PPC_OP_MTCRF: {
        if (inst->crm & 0xFFu) {
            for (u32 crf = 0; crf < 8; crf++)
                if (inst->crm & (0x80u >> crf))
                    fprintf(out, "    ctx->crf[%u] = (u8)((ctx->gpr[%u] >> %u) & 0xFu);\n",
                            crf, inst->rS, cr_field_shift((u8)crf));
            for (u32 crf = 0; crf < 8; crf++)
                if (inst->crm & (0x80u >> crf))
                    fprintf(out, "    PPC_CR_WRITE(%u);\n", crf);
        } else {
            fprintf(out, "    // mtcrf mask selects no CR fields\n");
        }
        break;
    }

    case PPC_OP_MFMSR:
        fprintf(out, "    ctx->gpr[%u] = ctx->msr;\n", inst->rD);
        break;

    case PPC_OP_MTMSR:
        fprintf(out, "    ctx->msr = ctx->gpr[%u];\n", inst->rS);
        break;

    case PPC_OP_MFSR:
        fprintf(out, "    ctx->gpr[%u] = ctx->sr[%u];\n", inst->rD, inst->sr);
        break;

    case PPC_OP_MFSRIN:
        fprintf(out, "    ctx->gpr[%u] = ctx->sr[(ctx->gpr[%u] >> 28) & 0xFu];\n",
                inst->rD, inst->rB);
        break;

    case PPC_OP_MTSR:
        fprintf(out, "    ctx->sr[%u] = ctx->gpr[%u];\n", inst->sr, inst->rS);
        break;

    case PPC_OP_MTSRIN:
        fprintf(out, "    ctx->sr[(ctx->gpr[%u] >> 28) & 0xFu] = ctx->gpr[%u];\n",
                inst->rB, inst->rS);
        break;

    case PPC_OP_MFTB:
        fprintf(out, "    ctx->gpr[%u] = ppc_mftb(ctx, %uu, 0x%08Xu);\n",
                inst->rD, inst->spr, inst->address);
        emit_exc_check_return(out, "    ");
        break;

    case PPC_OP_MFSPR:
        switch (inst->spr) {
        case 1:
            fprintf(out, "    PPC_CA_READ();\n");
            fprintf(out, "    ctx->gpr[%u] = ctx->xer;\n", inst->rD);
            break;
        case 8: fprintf(out, "    ctx->gpr[%u] = ctx->lr;\n", inst->rD); break;
        case 9: fprintf(out, "    ctx->gpr[%u] = ctx->ctr;\n", inst->rD); break;
        case 26: fprintf(out, "    ctx->gpr[%u] = ctx->srr0;\n", inst->rD); break;
        case 27: fprintf(out, "    ctx->gpr[%u] = ctx->srr1;\n", inst->rD); break;
        case 268:
        case 269:
            fprintf(out, "    ctx->gpr[%u] = ppc_mftb(ctx, %uu, 0x%08Xu);\n",
                    inst->rD, inst->spr, inst->address);
            emit_exc_check_return(out, "    ");
            break;
        case 912: fprintf(out, "    ctx->gpr[%u] = ctx->gqr[0];\n", inst->rD); break;
        case 913: fprintf(out, "    ctx->gpr[%u] = ctx->gqr[1];\n", inst->rD); break;
        case 914: fprintf(out, "    ctx->gpr[%u] = ctx->gqr[2];\n", inst->rD); break;
        case 915: fprintf(out, "    ctx->gpr[%u] = ctx->gqr[3];\n", inst->rD); break;
        case 916: fprintf(out, "    ctx->gpr[%u] = ctx->gqr[4];\n", inst->rD); break;
        case 917: fprintf(out, "    ctx->gpr[%u] = ctx->gqr[5];\n", inst->rD); break;
        case 918: fprintf(out, "    ctx->gpr[%u] = ctx->gqr[6];\n", inst->rD); break;
        case 919: fprintf(out, "    ctx->gpr[%u] = ctx->gqr[7];\n", inst->rD); break;
        case 282: fprintf(out, "    ctx->gpr[%u] = ctx->ear;\n", inst->rD); break;
        case 920: fprintf(out, "    ctx->gpr[%u] = ctx->hid2;\n", inst->rD); break;
        default:
            fprintf(out, "    DOLRECOMP_DC_FLUSH(ctx);\n");
            fprintf(out, "    ppc_fallback_instruction(ctx, 0x%08Xu, 0x%08Xu);\n",
                    inst->raw, inst->address);
            fprintf(out, "    DOLRECOMP_DC_RELOAD(ctx);\n");
            fprintf(out, "    DOLRECOMP_RETURN;\n");
            break;
        }
        break;

    case PPC_OP_MTSPR:
        switch (inst->spr) {
        case 1: fprintf(out, "    ctx->xer = ctx->gpr[%u];\n", inst->rS); break;
        case 8: fprintf(out, "    ctx->lr = ctx->gpr[%u];\n", inst->rS); break;
        case 9: fprintf(out, "    ctx->ctr = ctx->gpr[%u];\n", inst->rS); break;
        case 26: fprintf(out, "    ctx->srr0 = ctx->gpr[%u];\n", inst->rS); break;
        case 27: fprintf(out, "    ctx->srr1 = ctx->gpr[%u];\n", inst->rS); break;
        case 282: fprintf(out, "    ctx->ear = ctx->gpr[%u];\n", inst->rS); break;
        case 912: fprintf(out, "    ctx->gqr[0] = ctx->gpr[%u];\n", inst->rS); break;
        case 913: fprintf(out, "    ctx->gqr[1] = ctx->gpr[%u];\n", inst->rS); break;
        case 914: fprintf(out, "    ctx->gqr[2] = ctx->gpr[%u];\n", inst->rS); break;
        case 915: fprintf(out, "    ctx->gqr[3] = ctx->gpr[%u];\n", inst->rS); break;
        case 916: fprintf(out, "    ctx->gqr[4] = ctx->gpr[%u];\n", inst->rS); break;
        case 917: fprintf(out, "    ctx->gqr[5] = ctx->gpr[%u];\n", inst->rS); break;
        case 918: fprintf(out, "    ctx->gqr[6] = ctx->gpr[%u];\n", inst->rS); break;
        case 919: fprintf(out, "    ctx->gqr[7] = ctx->gpr[%u];\n", inst->rS); break;
        case 920: fprintf(out, "    ctx->hid2 = ctx->gpr[%u];\n", inst->rS); break;
        default:
            fprintf(out, "    DOLRECOMP_DC_FLUSH(ctx);\n");
            fprintf(out, "    ppc_fallback_instruction(ctx, 0x%08Xu, 0x%08Xu);\n",
                    inst->raw, inst->address);
            fprintf(out, "    DOLRECOMP_DC_RELOAD(ctx);\n");
            fprintf(out, "    DOLRECOMP_RETURN;\n");
            break;
        }
        break;

    case PPC_OP_TLBIE:
        fprintf(out, "    ppc_tlbie(ctx, ctx->gpr[%u], 0x%08Xu);\n", inst->rB, inst->address);
        emit_exc_check_return(out, "    ");
        break;

    case PPC_OP_SYNC:
    case PPC_OP_EIEIO:
    case PPC_OP_ISYNC:
    case PPC_OP_TLBSYNC:
        fprintf(out, "    ppc_memory_fence();\n");
        break;

    case PPC_OP_ECIWX:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 ea = ");
        emit_xform_ea(out, inst->rA, inst->rB, false);
        fprintf(out, ";\n");
        fprintf(out, "        u32 value = ppc_eciwx(ctx, ea, 0x%08Xu);\n", inst->address);
        emit_exc_check_return(out, "        ");
        fprintf(out, "        ctx->gpr[%u] = value;\n", inst->rD);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_ECOWX:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 ea = ");
        emit_xform_ea(out, inst->rA, inst->rB, false);
        fprintf(out, ";\n");
        fprintf(out, "        ppc_ecowx(ctx, ea, ctx->gpr[%u], 0x%08Xu);\n",
                inst->rS, inst->address);
        emit_exc_check_return(out, "        ");
        fprintf(out, "    }\n");
        break;

    default:
        fprintf(out, "    DOLRECOMP_DC_FLUSH(ctx);\n");
        fprintf(out, "    ppc_fallback_instruction(ctx, 0x%08Xu, 0x%08Xu);\n",
                inst->raw, inst->address);
        fprintf(out, "    DOLRECOMP_DC_RELOAD(ctx);\n");
        fprintf(out, "    DOLRECOMP_RETURN;\n");
        break;
    }

    fprintf(out, "\n");
}

void emit_instruction(FILE* out, const PPCInst* inst) {
    s_ca_dead_here = 0;   /* no liveness context outside emit_function() */
    emit_instruction_with_range(out, inst, 0, (u32)-1);
}

static bool mfspr_is_modeled(u16 spr) {
    switch (spr) {
    case 1: case 8: case 9: case 26: case 27:
    case 268: case 269: case 282:
    case 912: case 913: case 914: case 915:
    case 916: case 917: case 918: case 919: case 920:
        return true;
    default:
        return false;
    }
}

static bool mtspr_is_modeled(u16 spr) {
    switch (spr) {
    case 1: case 8: case 9: case 26: case 27: case 282:
    case 912: case 913: case 914: case 915:
    case 916: case 917: case 918: case 919: case 920:
        return true;
    default:
        return false;
    }
}

static bool inst_routes_to_fallback(const PPCInst* inst) {
    switch (inst->op) {
    // dcbst/dcbf/dcbi are emitted inline as no-ops (flat memory) — not fallback.
    case PPC_OP_ICBI:
    case PPC_OP_UNKNOWN:
        return true;
    case PPC_OP_MFSPR:
        return !mfspr_is_modeled(inst->spr);
    case PPC_OP_MTSPR:
        return !mtspr_is_modeled(inst->spr);
    default:
        return false;
    }
}

static bool inst_ends_block(const PPCInst* inst) {
    switch (inst->op) {
    case PPC_OP_B:
    case PPC_OP_BC:
    case PPC_OP_BCLR:
    case PPC_OP_BCCTR:
    case PPC_OP_SC:
    case PPC_OP_RFI:
        return true;
    default:
        return inst_routes_to_fallback(inst);
    }
}

/* ---- XER[CA] liveness ---------------------------------------------------
 *
 * XER[CA] is written 1.255 G times per gameplay run and read 20.2 M -- 98.4%
 * of the writes are dead. Unlike FPRF it cannot simply be stubbed out (a
 * RECOMP_NO_CA build produces zero frames: some early multi-word arithmetic
 * loop depends on the carry to terminate), and unlike FPRF it cannot be made
 * lazy for free either, because six instructions consume it as an INPUT. So
 * the only way to skip the computation is to prove, per site, that nothing
 * reads the value before it is overwritten.
 *
 * This is a backward dataflow over one function's blocks, and it is
 * deliberately SOUND rather than aggressive: any point where control leaves
 * the function -- a call, a return, an indirect branch, a non-local branch, a
 * fallback to the interpreter, sc/rfi -- is treated as a reader, because the
 * code on the other side is not being analysed. Interprocedural propagation
 * would capture more, but a wrong answer here is silent guest-state
 * corruption, and the measurement below decides whether that is worth it.
 *
 * MEASURE THE CAPTURE RATE BEFORE WIRING ANY ELISION. The 98.4%-dead figure
 * is DYNAMIC; what this can prove STATICALLY under the escape rule is a
 * different and probably much smaller number, and if it is small the whole
 * idea is dead for ~1% of headroom.
 */
static bool inst_defs_ca(const PPCInst* inst) {
    switch (inst->op) {
    case PPC_OP_SUBFIC:
    case PPC_OP_ADDIC:   case PPC_OP_ADDIC_DOT:
    case PPC_OP_ADDC:    case PPC_OP_ADDCO:
    case PPC_OP_ADDE:    case PPC_OP_ADDEO:
    case PPC_OP_ADDME:   case PPC_OP_ADDMEO:
    case PPC_OP_ADDZE:   case PPC_OP_ADDZEO:
    case PPC_OP_SUBFC:   case PPC_OP_SUBFCO:
    case PPC_OP_SUBFE:   case PPC_OP_SUBFEO:
    case PPC_OP_SUBFME:  case PPC_OP_SUBFMEO:
    case PPC_OP_SUBFZE:  case PPC_OP_SUBFZEO:
    case PPC_OP_SRAW:    case PPC_OP_SRAWI:
        return true;
    case PPC_OP_MTSPR:
        return inst->spr == 1u;      /* mtxer rewrites CA wholesale */
    default:
        return false;
    }
}

static bool inst_uses_ca(const PPCInst* inst) {
    switch (inst->op) {
    case PPC_OP_ADDE:    case PPC_OP_ADDEO:
    case PPC_OP_ADDME:   case PPC_OP_ADDMEO:
    case PPC_OP_ADDZE:   case PPC_OP_ADDZEO:
    case PPC_OP_SUBFE:   case PPC_OP_SUBFEO:
    case PPC_OP_SUBFME:  case PPC_OP_SUBFMEO:
    case PPC_OP_SUBFZE:  case PPC_OP_SUBFZEO:
    case PPC_OP_MCRXR:
        return true;
    case PPC_OP_MFSPR:
        return inst->spr == 1u;      /* mfxer exposes CA to the guest */
    default:
        return false;
    }
}

/* Does control leave the analysed function here? Then CA must be assumed read. */
static bool ca_escapes(const PPCInst* inst, u32 func_addr, u32 func_end) {
    if (inst_routes_to_fallback(inst))
        return true;
    switch (inst->op) {
    case PPC_OP_SC:
    case PPC_OP_RFI:
    case PPC_OP_BCLR:
    case PPC_OP_BCCTR:
        return true;
    case PPC_OP_B:
    case PPC_OP_BC:
        if (inst->lk)
            return true;             /* a call: the callee may read CA */
        return !branch_target_is_local(func_addr, func_end, inst->branch_target);
    default:
        return false;
    }
}

#define CA_LIVE_IN(k) \
    (inst_uses_ca(&insts[k]) ? 1u \
     : (inst_defs_ca(&insts[k]) ? 0u : live_out[k]))

/* live_out[i] = is CA live immediately AFTER instruction i? */
static void compute_ca_live(const PPCInst* insts, u32 count, u32 func_addr,
                            u8* live_out) {
    u32 func_end = func_addr + count * 4u;
    bool changed = true;
    u32 guard = 0;

    memset(live_out, 0, count);
    /* Monotone (liveness only ever turns on), so this terminates; the guard is
     * belt-and-braces against a malformed branch target making it oscillate. */
    while (changed && guard++ < 256u) {
        u32 k;
        changed = false;
        for (k = count; k-- > 0;) {
            const PPCInst* inst = &insts[k];
            u8 out;

            if (inst->embedded_data) {
                out = 1u;            /* not really code; assume the worst */
            } else if (k + 1u >= count) {
                out = 1u;            /* falls off the end into the unknown */
            } else if (!inst_ends_block(inst)) {
                out = (u8)CA_LIVE_IN(k + 1u);
            } else if (ca_escapes(inst, func_addr, func_end)) {
                out = 1u;
            } else {
                out = 0u;
                if (inst->op == PPC_OP_B || inst->op == PPC_OP_BC) {
                    u32 t = (inst->branch_target - func_addr) / 4u;
                    if (t < count)
                        out |= (u8)CA_LIVE_IN(t);
                }
                /* A conditional bc also falls through. bo bit pattern 1z1zz is
                 * "branch always", which does not. */
                if (inst->op == PPC_OP_BC && (inst->bo & 0x14u) != 0x14u)
                    out |= (u8)CA_LIVE_IN(k + 1u);
            }

            if (out != live_out[k]) {
                live_out[k] = out;
                changed = true;
            }
        }
    }
}

static u32 inst_cycle_cost(const PPCInst* inst) {
    if (inst->embedded_data || inst_routes_to_fallback(inst))
        return 0;

    switch (inst->op) {
    case PPC_OP_MULLI:
        return 3;
    case PPC_OP_SC:
    case PPC_OP_RFI:
    case PPC_OP_TW:
        return 2;
    case PPC_OP_LMW:
    case PPC_OP_STMW:
        return 11;
    case PPC_OP_MULLW:
    case PPC_OP_MULLWO:
    case PPC_OP_MULHW:
    case PPC_OP_MULHWU:
        return 5;
    case PPC_OP_DIVW:
    case PPC_OP_DIVWO:
    case PPC_OP_DIVWU:
    case PPC_OP_DIVWUO:
        return 40;
    case PPC_OP_DCBZ:
        return 5;
    case PPC_OP_DCBTST:
    case PPC_OP_DCBT:
        return 2;
    case PPC_OP_MFSR:
    case PPC_OP_MFSRIN:
        return 3;
    case PPC_OP_MTSPR:
        return 2;
    case PPC_OP_SYNC:
        return 3;
    case PPC_OP_MTFSB0:
    case PPC_OP_MTFSB1:
    case PPC_OP_MTFSF:
    case PPC_OP_MTFSFI:
        return 3;
    case PPC_OP_FDIVS:
        return 17;
    case PPC_OP_FDIV:
        return 31;
    case PPC_OP_PS_DIV:
        return 17;
    case PPC_OP_PS_RSQRTE:
        return 2;
    default:
        return 1;
    }
}

/* Block leaders: the function entry, whatever follows a block-ending
 * instruction, and every local branch target. Shared by the emitter and the
 * entry-point collector so the switch and the table can never disagree -- if
 * they did, the chassis would dispatch into a chunk at an address the switch
 * has no case for. */
static void compute_leaders(const PPCInst* insts, u32 count, u32 func_addr, u8* leader) {
    u32 func_end = func_addr + count * 4u;
    u32 i;
    if (count)
        leader[0] = 1;
    for (i = 0; i < count; i++) {
        const PPCInst* inst = &insts[i];
        if (inst->embedded_data)
            continue;
        if (inst_ends_block(inst) && i + 1u < count)
            leader[i + 1u] = 1;
        if ((inst->op == PPC_OP_B || inst->op == PPC_OP_BC) &&
            branch_target_is_local(func_addr, func_end, inst->branch_target)) {
            leader[(inst->branch_target - func_addr) / 4u] = 1;
        }
    }
}

void emit_collect_entry_points(const PPCInst* insts, u32 count, u32 func_addr) {
    u8* leader;
    u32 i;
    if (!s_leader_cases || count == 0)
        return;
    leader = (u8*)calloc(count, sizeof(u8));
    if (!leader)
        return;
    compute_leaders(insts, count, func_addr, leader);
    for (i = 0; i < count; i++) {
        if (leader[i] || is_extra_entry(&insts[i]))
            record_entry(insts[i].address);
    }
    free(leader);
}

void emit_function(FILE* out, const PPCInst* insts, u32 count, u32 own, u32 func_addr) {
    u32 i;
    u32 func_end = func_addr + count * 4u;

    u8* leader = (u8*)calloc(count ? count : 1u, sizeof(u8));
    u8* entry = (u8*)calloc(own ? own : 1u, sizeof(u8));
    u32* block_cost = (u32*)calloc(count ? count : 1u, sizeof(u32));

    compute_leaders(insts, count, func_addr, leader);
    /* The switch's cases are the WINDOW's leaders, computed exactly as
       emit_collect_entry_points() does, so the switch and the dispatch table
       still agree. With an overhang the whole range's leaders only add labels
       and block splits; the window's end is a leader, as it is in the owning
       chunk, so the owner's blocks are charged the same way. */
    compute_leaders(insts, own, func_addr, entry);
    if (own < count)
        leader[own] = 1;
    s_own_end = func_addr + own * 4u;

    u8* ca_live = NULL;
    if ((s_ca_liveness || s_ca_elide) && count) {
        ca_live = (u8*)calloc(count, sizeof(u8));
        if (ca_live) {
            compute_ca_live(insts, count, func_addr, ca_live);
            for (i = 0; s_ca_liveness && i < count; i++) {
                if (insts[i].embedded_data)
                    continue;
                if (inst_uses_ca(&insts[i]))
                    DR_STAT_INC(&s_ca_uses);
                if (inst_defs_ca(&insts[i])) {
                    DR_STAT_INC(&s_ca_defs);
                    if (!ca_live[i])
                        DR_STAT_INC(&s_ca_dead);
                }
            }
        }
    }

    /* Per-instruction cost of the REST of its block, for the exception refund.
     * Walked with exactly the same block bounds the charge uses, so the refund
     * can never exceed what was charged. */
    u32* suffix = (u32*)calloc(count ? count : 1u, sizeof(u32));

    for (i = 0; i < count; i++) {
        u32 j, last;
        if (!leader[i])
            continue;
        last = i;
        for (j = i;;) {
            block_cost[i] += inst_cycle_cost(&insts[j]);
            last = j;
            if (inst_ends_block(&insts[j]))
                break;
            j++;
            if (j >= count || leader[j])
                break;
        }
        /* Walk back from the end so suffix[j] = cost of everything after j. */
        for (j = last + 1u; j-- > i;)
            suffix[j] = (j == last) ? 0u : suffix[j + 1u] + inst_cycle_cost(&insts[j + 1u]);
    }

    fprintf(out, "%svoid func_%08X(CPUState* ctx) {\n", emit_chunk_cc(), func_addr);
    /* Caches ctx->ram in a local for the whole chunk. Expands to nothing unless
       the module is built with MODULE_RAM_LOCAL, so the same generated tree
       builds both arms of the A/B (see DOLRECOMP_RAM_LOCAL in cpu.h). */
    fprintf(out, "    DOLRECOMP_RAM_LOCAL(ctx);\n");
    /* Keeps the cycle charge in a local for the whole chunk (DOLRECOMP_DC_LOCAL
       in cpu.h); a no-op unless the module is built with MODULE_DC_LOCAL. */
    fprintf(out, "    DOLRECOMP_DC_LOCAL(ctx);\n");
    /* Not worth lowering differently: an index switch with every index a case
       (one jump table instead of clang's compare tree) measured -0.04% cycles
       on the US disc. The entry cost is the indirect jump and the prologue. */
    fprintf(out, "    switch (ctx->pc) {\n");
    for (i = 0; i < own; i++) {
        if (s_leader_cases && !entry[i] && !is_extra_entry(&insts[i]))
            continue;
        fprintf(out, "    case 0x%08Xu: goto label_%08X;\n",
                insts[i].address, insts[i].address);
    }
    /* The switch's own default is an exit too, and under --call-resume it is
       where a re-entry that does not belong to this chunk ends up. Leaving it
       uncounted hid ~49 M dispatches: the instrumented total fell while the
       chassis kept dispatching just as often. */
    if (s_exit_stats)
        fprintf(out, "    default: dolrecomp_exit_stat(DR_EXIT_SWITCH_MISS); DOLRECOMP_RETURN;\n");
    else
        fprintf(out, "    default: DOLRECOMP_RETURN;\n");
    fprintf(out, "    }\n");

    s_fp_ok = false;
    for (i = 0; i < count; i++) {
        const bool fp_inst = !insts[i].embedded_data && ppc_op_uses_fpu(insts[i].op);
        /* Any entry resets it, not only leaders: under --direct-calls a return
           point or a cross-chunk target mid-block is a switch case too, and the
           code arriving there may have run with FP turned off (a thread switch
           in between). An FP op's own entry is label_X, which still tests. */
        if (leader[i] || (!fp_inst && is_extra_entry(&insts[i])))
            s_fp_ok = false;
        s_fp_skip_label = 0;
        if (s_fp_check_once && fp_inst && s_fp_ok) {
            fprintf(out, "    goto fpok_%08X;\n", insts[i].address);
            s_fp_skip_label = insts[i].address;
        }
        fprintf(out, "label_%08X:\n", insts[i].address);
        // ctx->pc only has to be live where it is actually read: block entry
        // (the dispatch switch + lockstep check use the leader address, which
        // the caller/branch already placed in ctx->pc) and downcount context.
        // Branches set ctx->pc to their target themselves, faults/fallbacks
        // take the address as a literal, and no runtime helper reads ctx->pc —
        // so a per-instruction store was dead, yet the compiler had to keep it
        // before every mem/FP call (can't prove the callee ignores pc). Emit it
        // only at leaders; mid-block instructions inherit the leader's value.
        if (leader[i]) {
            fprintf(out, "    ctx->pc = 0x%08Xu;\n", insts[i].address);
            if (block_cost[i] != 0)
                fprintf(out, "    DOLRECOMP_DC -= %u;\n", block_cost[i]);
        }
        s_block_suffix = suffix[i];
        /* live_out[i] is "is CA live AFTER instruction i", so a defining
           instruction whose CA is not live afterwards is writing a value
           nothing reads. Only elide when asked: the analysis is conservative
           but a wrong answer here is a silent divergence, not a crash. */
        s_ca_dead_here = (s_ca_elide && ca_live && !ca_live[i]) ? 1 : 0;
        emit_instruction_with_range(out, &insts[i], func_addr, func_end);
        if (fp_inst)
            s_fp_ok = true;
        if (insts[i].op == PPC_OP_MTMSR)
            s_fp_ok = false;
    }
    s_fp_skip_label = 0;
    s_block_suffix = 0;
    s_ca_dead_here = 0;

    free(ca_live);
    free(leader);
    free(entry);
    free(block_cost);
    s_own_end = 0;
    free(suffix);

    fprintf(out, "    ctx->pc = 0x%08Xu;\n", func_end);
    /* Falling off the end is a return too: without this the DC_LOCAL charge of
       the chunk's last blocks was dropped (frame hash 5a2213402c34, not
       4217c7669322). With MODULE_DC_LOCAL off this is a plain `return;`. */
    fprintf(out, "    DOLRECOMP_RETURN;\n");
    fprintf(out, "}\n\n");
}
