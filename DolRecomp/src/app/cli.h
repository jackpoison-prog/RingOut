#ifndef DOLRECOMP_APP_CLI_H
#define DOLRECOMP_APP_CLI_H

#include <stddef.h>
#include "common/types.h"
#include "backend/emitter.h"

typedef struct {
    const char* input_path;
    const char* title_id_arg;
    const char* output_arg;
    const char* map_path;
    DolRecompCPU cpu;
    u32 jobs;
    u32 rel_base;
    int gamecube_mode;
    /* Guest PC of the OS idle spin loop (--idle-pc), or 0. Back-edges to it stay
     * dispatcher returns so the host can still recognise and skip it. */
    u32 idle_pc;
    /* --idle-pc auto: find the loop in the DOL instead of being told where it
     * is, so a disc from another region recompiles without a new constant. */
    int idle_pc_auto;
    int llvm_backend;   // --backend llvm
    /* Guest PCs the run loop hooks by address (--dispatch-pc). Calls to these
       must not be chained into native gotos or the hook never fires. */
    u32 dispatch_pcs[32];
    u32 dispatch_pc_count;
    int chain_calls;
    int leader_cases;
    unsigned chunk_overhang; // --chunk-overhang <max>: emit past the window to the first exit
    const char* ram_bases;   // --ram-bases <list>: base registers that only address RAM
    int preserve_none;       // --preserve-none: chunk functions save no callee-saved registers
    int fp_check_once;       // --fp-check-once: one MSR.FP test per straight-line run
    const char* twin_hot;   // --twin-hot <file>: hot entry PCs -> twin chunks
    int twin_cr;            // --twin-cr: the fast copy also keeps CR fields in locals
    const char* twin_regs;  // --twin-regs <list>: guest registers kept in locals
    int direct_calls;   // --direct-calls
    int self_calls;     // --self-calls (needs --direct-calls)
    int tail_calls;     // --tail-calls (needs --direct-calls)
    int exit_stats;     // --exit-stats (diagnostic: count dispatches by cause)
    int ca_liveness;
    int ca_elide;       // --ca-elide (changes codegen; off by default)
    int cpu_explicit;
    int rel_base_set;
    int setup_mode;
    int show_help;
} CliOptions;

void print_usage(const char* argv0);
int is_title_id(const char* text);
int is_title_id_length_valid(const char* text);
int parse_cpu_name(const char* text, DolRecompCPU* cpu);
const char* cpu_display_name(DolRecompCPU cpu);
void copy_title_id(char* out, size_t out_size, const char* title_id);
int parse_job_count(const char* text, u32* jobs);
int parse_u32_arg(const char* text, const char* name, u32* value_out);
int parse_cli(int argc, char** argv, CliOptions* opts);

#endif
