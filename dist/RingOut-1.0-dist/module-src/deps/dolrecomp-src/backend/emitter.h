#ifndef DOLRECOMP_EMITTER_H
#define DOLRECOMP_EMITTER_H

#include "../common/types.h"
#include "../frontend/decoder.h"
#include <stdio.h>

typedef enum {
    DOLRECOMP_CPU_GEKKO,
    DOLRECOMP_CPU_BROADWAY,
    DOLRECOMP_CPU_ESPRESSO,
} DolRecompCPU;

// Split C emitter used by the command-line recompiler.

// emit the boilerplate header (includes, typedefs, etc)
void emit_header(FILE* out);
void emit_header_for_cpu(FILE* out, DolRecompCPU cpu);

// Register chunk entries for the opt-in cross-chunk direct-call experiment.
// Direct calls bypass chassis dispatch checks and must not be enabled by a
// runtime that validates mutable guest code there. Call before worker emission;
// passing count == 0 restores the safe return-to-chassis form.
void emit_set_chunk_table(const u32* starts, u32 count);

// emit a single recompiled function as C code
/* insts[0, own) are the chunk's window: its entry cases, its dispatch-table
 * range. insts[own, count) are an overhang: the following chunk's code, emitted
 * again as plain labels so control that runs past the window stays here. */
void emit_function(FILE* out, const PPCInst* insts, u32 count, u32 own, u32 func_addr);

/* Guest PC of an OS idle spin loop, if the host skips it (see --idle-pc).
 * Back-edges to it are emitted as dispatcher returns so the host still sees it. */
void emit_set_idle_pc(u32 pc);
// Select the LLVM object backend instead of emitting C. A setter rather than a
// parameter because that is how this fork already threads options to the
// emitter, and threading a new argument through emit_dol/rpx/rel_split and
// every caller would touch far more than the feature needs.
void emit_set_llvm_backend(int enabled);
int emit_llvm_backend_enabled(void);

/* Guest PC that must always be reached through the dispatcher (a run-loop hook
   target). Repeatable; see emitter.c. */
void emit_add_dispatch_pc(u32 pc);

/* Turn local `bl` into a native goto (--chain-calls). Unproven: see emitter.c. */
void emit_set_chain_calls(bool enable);

/* Reduced chunk-entry switch (--leader-cases): cases only where control can
   actually arrive. See emitter.c for the entry rule and its evidence. */
void emit_set_leader_cases(bool enable);
void emit_set_chunk_overhang(u32 max_insts);
/* bit i: D-form accesses based on guest ri can only reach main RAM. */
void emit_set_ram_bases(u32 mask);
void emit_set_preserve_none(bool enable);
/* --fp-check-once: see emitter.c. */
void emit_set_fp_check_once(bool enable);
bool emit_preserve_none_enabled(void);
/* "DOLRECOMP_CHUNK_FN " under --preserve-none, else "". */
const char* emit_chunk_cc(void);
/* How many instructions after insts[own] a chunk may take as its overhang.
 * MAIN THREAD (pipeline). */
u32 emit_overhang_length(const PPCInst* insts, u32 own, u32 available,
                         const u32* stop_ranges, u32 stop_range_count);

/* Program-wide entry targets for --leader-cases: every direct branch target in
   any code section plus every data word pointing into code. Copied. MAIN THREAD
   ONLY, before any chunk job runs; the workers only read it. */
void emit_set_entry_targets(const u32* targets, u32 count);

/* Cross-chunk `bl` as a direct call to the target chunk (--direct-calls); see
   emitter.c. Needs the chunk table (emit_set_chunk_table). */
void emit_set_direct_calls(bool enable);
bool emit_direct_calls_enabled(void);

/* --exit-stats: count dispatches by the instruction that caused them
   (diagnostic build; prints when the module unloads). */
void emit_set_exit_stats(bool enable);
bool emit_exit_stats_enabled(void);

/* The DOL has no lwarx/stwcx: generated.h gets DOLRECOMP_DOL_NO_RESERVATION. */
void emit_set_no_reservation(bool none);

/* Widenings of --direct-calls (--self-calls, --tail-calls); see emitter.c. */
void emit_set_self_calls(bool enable);
void emit_set_tail_calls(bool enable);
bool emit_self_calls_enabled(void);
bool emit_tail_calls_enabled(void);
/* XER[CA] dead-write analysis (--ca-liveness). REPORTING ONLY: it counts how
   many write sites a SOUND intraprocedural analysis can prove dead, which is
   the number that decides whether eliding them is worth the risk. Emitted code
   is identical either way. */
void emit_set_ca_liveness(bool enable);

/* Elide XER[CA] writes the liveness pass proves dead. SEPARATE from
   --ca-liveness, which only reports and must leave codegen alone. Off by
   default: 98.4% of CA writes are dead, but CA READS are load-bearing (a
   RECOMP_NO_CA build produces zero frames), so a wrong verdict here is a silent
   divergence rather than a crash. Gate any build on the frame hashes. */
void emit_set_ca_elide(bool enable);
void emit_report_ca_stats(void);

/* Writes the entry-point sidecar. No-op (and writes nothing) unless the entry
   set was reduced; its absence means "the whole chunk range is entrable". */
int emit_write_entry_points(const char* path);

/* Records one chunk's entry points. MAIN THREAD ONLY -- emit_function() runs on
   the -jN workers, where appending to the shared list is a data race. */
void emit_collect_entry_points(const PPCInst* insts, u32 count, u32 func_addr);

// emit a single instruction as C code
void emit_instruction(FILE* out, const PPCInst* inst);

// emit the boilerplate footer
void emit_footer(FILE* out);

#endif /* DOLRECOMP_EMITTER_H */
