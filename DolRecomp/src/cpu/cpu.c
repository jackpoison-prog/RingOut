// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ExpansionPak

#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <stdatomic.h>

// glibc 2.38 added a new symbol version for fmod, and the linker binds whatever
// the BUILD host has. That silently sets this module's glibc floor to 2.38 on
// any current distro (Arch, Fedora 39+, Ubuntu 24.04) -- above SteamOS's ~2.37,
// so the module fails to dlopen on a Deck with "version GLIBC_2.38 not found".
//
// That breaks the documented Deck workflow, where the player builds the module
// on a desktop and copies it across (dist/RingOut-1.0-deck/README.txt). It has
// gone unnoticed because the Deck's own module was built ON the Deck, where the
// floor is 2.2.5, and because a builder on Debian/Ubuntu LTS never sees it.
//
// Pinning the old version is safe: fmod is exact -- its result is always
// representable, so both symbol versions return identical values. Verified by
// frame hashes, not assumed. The single call site is the round-half-to-even
// tiebreak in ppc_round_to_nearest below.
//
// SCOPED TO THE MODULE BUILD (MODULE_GAME_ID is defined only there). cpu.c is
// also linked into the recompiler tool, which ships STATICALLY -- setup.sh runs
// it directly, so its glibc floor decides which distros can run setup at all.
// A static link has no symbol versioning, so this directive leaves an
// unresolvable `fmod@GLIBC_2.2.5` and the tool fails to link:
//
//   libdr_cpu.a(cpu.c.o): in function `ppc_fctiw':
//   undefined reference to `fmod@GLIBC_2.2.5'
//
// That broke the static tool build from the moment this pin was added, and went
// unnoticed because the shipped binary predates it and install(1) re-stamped its
// mtime, so it looked current. The pin is only ever needed for the module, which
// is the thing that gets dlopened on a Deck.
#if defined(__linux__) && defined(__GLIBC__) && defined(MODULE_GAME_ID)
__asm__(".symver fmod,fmod@GLIBC_2.2.5");
#endif

// Write-gather pipe direct path. GX command and vertex traffic reaches the
// host as ordinary guest stores to the write-gather pipe page, and every one of
// them used to leave the module: mem_writeN_slow -> resolve_addr miss ->
// cpu->external_write, an indirect call across the .so boundary into
// HookExternalWrite, which then wrote a few bytes into a buffer. On a
// draw-call-heavy scene that is the single busiest thing the chassis does.
//
// The pipe is just a byte buffer plus a cursor, so the module can write it
// directly and only call out when it fills -- once per 32 bytes instead of once
// per word. This is what Dolphin's own JITs do (optimizeGatherPipe): inline the
// write, keep the flush out of line.
//
// Wired up through a setter, in the same style as ppc_set_mem_write_journal
// below and for the same reason: a CPUState field would be tidier but bumps the
// module ABI, which couples module and runtime deployment (see the lazy-FPRF
// note in cpu.h). Unset, every field here is NULL and the code below falls
// straight through to the old external_write path, so an old runtime drives a
// new module and vice versa.
typedef void (*PPCGatherPipeFlush)(void* user);
static u8** g_gp_cursor = NULL;      // &ppc_state.gather_pipe_ptr, owned by the chassis
// The ADDRESS of the base pointer, not its value: the chassis fills
// gather_pipe_base_ptr in GPFifoManager::Init(), which need not have run when
// this is installed, so a snapshot taken here could be a permanent NULL.
// Re-reading it also survives any later reset. One extra load from a hot line.
static u8* const* g_gp_base = NULL;
static PPCGatherPipeFlush g_gp_flush = NULL;
static void* g_gp_user = NULL;
// Non-zero while the lockstep verifier is journaling. Those runs need every
// MMIO write recorded by the chassis hook, so the fast path stands down rather
// than silently dropping entries from the journal.
static const unsigned char* g_gp_bypass = NULL;

__attribute__((visibility("default"))) void ppc_set_gather_pipe(u8** cursor, u8* const* base,
                                                                PPCGatherPipeFlush flush,
                                                                void* user,
                                                                const unsigned char* bypass) {
    g_gp_cursor = cursor;
    g_gp_base = base;
    g_gp_flush = flush;
    g_gp_user = user;
    g_gp_bypass = bypass;
}

#define GATHER_PIPE_PAGE 0xCC008000u
#define GATHER_PIPE_FILL 32u

// Returns 1 when the store was serviced here. Keyed on the effective page,
// exactly as the chassis hook is, so which addresses take this path does not
// change -- only how they are serviced.
static inline int gather_pipe_store(u32 addr, u64 value, u32 size) {
    if (g_gp_cursor == NULL || (addr & 0xFFFFF000u) != GATHER_PIPE_PAGE)
        return 0;
    if (g_gp_bypass != NULL && *g_gp_bypass)
        return 0;

    const u8* base = *g_gp_base;
    if (base == NULL)
        return 0;  // GPFifoManager::Init has not run yet

    u8* p = *g_gp_cursor;
    switch (size) {
    case 1: *p = (u8)value; break;
    case 2: write_be16(p, (u16)value); break;
    case 4: write_be32(p, (u32)value); break;
    case 8: write_be64(p, value); break;
    default: return 0;
    }
    p += size;
    *g_gp_cursor = p;

    // The buffer carries GATHER_PIPE_EXTRA_SIZE bytes of slack past the fill
    // mark, so overshooting by one store before flushing is what the design
    // expects -- the same order FastWriteN + FastCheckGatherPipe uses.
    if ((u32)(p - base) >= GATHER_PIPE_FILL)
        g_gp_flush(g_gp_user);
    return 1;
}

// Lockstep memory-write journal: the chassis installs a callback via
// ppc_set_mem_write_journal so it can capture the pre-image of flat-RAM writes
// and re-run a block on Dolphin's interpreter for register/memory comparison.
// NULL and zero-cost unless installed. `offset` is the byte offset into cpu->ram.
typedef void (*PPCMemWriteJournal)(u32 offset, u32 size, void* user);
PPCMemWriteJournal g_mem_write_journal = NULL;
void* g_mem_write_journal_user = NULL;
__attribute__((visibility("default"))) void ppc_set_mem_write_journal(PPCMemWriteJournal fn,
                                                                      void* user) {
    g_mem_write_journal = fn;
    g_mem_write_journal_user = user;
}
static inline void ppc_journal_ram_write(CPUState* cpu, const u8* host, u32 size) {
    if (g_mem_write_journal && host >= cpu->ram && host < cpu->ram + cpu->ram_size)
        g_mem_write_journal((u32)(host - cpu->ram), size, g_mem_write_journal_user);
}

bool cpu_init(CPUState* cpu) {
    memset(cpu, 0, sizeof(*cpu));

    cpu->ram_size = GC_MAIN_RAM_SIZE;
    cpu->ram = (u8*)calloc(1, cpu->ram_size);
    if (!cpu->ram) {
        fprintf(stderr, "error: failed to allocate %u bytes for RAM\n", cpu->ram_size);
        return false;
    }

    cpu->spr[287] = PPC_GEKKO_PVR;

    return true;
}

bool cpu_alloc_mem2(CPUState* cpu, u32 size) {
    free(cpu->mem2);
    cpu->mem2 = NULL;
    cpu->mem2_size = 0;

    if (size == 0)
        return true;

    cpu->mem2 = (u8*)calloc(1, size);
    if (!cpu->mem2) {
        fprintf(stderr, "error, filed to allocate %u bytes for MEM2 region\n", size);
        return false;
    }

    cpu->mem2_size = size;
    return true;
}

void cpu_free(CPUState* cpu) {
    if (cpu->ram) {
        free(cpu->ram);
        cpu->ram = NULL;
    }
    if (cpu->mem2) {
        free(cpu->mem2);
        cpu->mem2 = NULL;
        cpu->mem2_size = 0;
    }
}

void cpu_reset(CPUState* cpu) {
    u8* ram = cpu->ram;
    u32 ram_size = cpu->ram_size;
    u8* mem2 = cpu->mem2;
    u32 mem2_size = cpu->mem2_size;
    PPCExternalRead external_read = cpu->external_read;
    PPCExternalWrite external_write = cpu->external_write;
    PPCExternalRead32 external_read32 = cpu->external_read32;
    PPCExternalWrite32 external_write32 = cpu->external_write32;
    PPCInstructionFallback instruction_fallback = cpu->instruction_fallback;
    PPCHostCall host_call = cpu->host_call;
    void* external_user_data = cpu->external_user_data;

    memset(cpu, 0, sizeof(*cpu));
    cpu->ram = ram;
    cpu->ram_size = ram_size;
    cpu->mem2 = mem2;
    cpu->mem2_size = mem2_size;
    cpu->external_read = external_read;
    cpu->external_write = external_write;
    cpu->external_read32 = external_read32;
    cpu->external_write32 = external_write32;
    cpu->instruction_fallback = instruction_fallback;
    cpu->host_call = host_call;
    cpu->external_user_data = external_user_data;

    if (cpu->ram)
        memset(cpu->ram, 0, cpu->ram_size);
    if (cpu->mem2)
        memset(cpu->mem2, 0, cpu->mem2_size);

    cpu->spr[287] = PPC_GEKKO_PVR;
}

// NOT static, deliberately. LLVM keys an internal-linkage function in a PGO
// profile as "<source path>;<symbol>", using the path as the compiler received
// it -- so a static function's counts are recorded under the TRAINING machine's
// absolute path and match nothing on anyone else's, silently. External linkage
// makes the key the bare symbol, which is the same everywhere. Visibility is
// hidden module-wide, so nothing leaves the .so either way.
u8* resolve_addr(CPUState* cpu, u32 addr, u32* avail);
u8* resolve_addr(CPUState* cpu, u32 addr, u32* avail) {
    if (addr >= GC_RAM_BASE && addr < GC_RAM_BASE + cpu->ram_size) {
        u32 offset = addr - GC_RAM_BASE;
        *avail = cpu->ram_size - offset;
        return cpu->ram + offset;
    }

    if (addr >= GC_RAM_UNCACHED && addr < GC_RAM_UNCACHED + cpu->ram_size) {
        u32 offset = addr - GC_RAM_UNCACHED;
        *avail = cpu->ram_size - offset;
        return cpu->ram + offset;
    }

    if (cpu->mem2 && cpu->mem2_size) {
        if (addr >= WII_MEM2_BASE && addr < WII_MEM2_BASE + cpu->mem2_size) {
            u32 offset = addr - WII_MEM2_BASE;
            *avail = cpu->mem2_size - offset;
            return cpu->mem2 + offset;
        }

        if (addr >= WII_MEM2_UNCACHED && addr < WII_MEM2_UNCACHED + cpu->mem2_size) {
            u32 offset = addr - WII_MEM2_UNCACHED;
            *avail = cpu->mem2_size - offset;
            return cpu->mem2 + offset;
        }
    }

    *avail = 0;
    return NULL;
}

static void clear_matching_reservation(CPUState* cpu, u32 addr) {
    if (cpu->reserve_valid && ((cpu->reserve_addr ^ addr) & ~31u) == 0)
        cpu->reserve_valid = false;
}

#define PPC_BIT(n) (1u << (31u - (n)))
#define PPC_MSR_RFI_MASK 0x87C0FFFFu
#define PPC_MSR_POW PPC_BIT(13)
#define PPC_MSR_ILE PPC_BIT(15)
#define PPC_MSR_EE  PPC_BIT(16)
#define PPC_MSR_PR  PPC_BIT(17)
#define PPC_MSR_FP  PPC_BIT(18)
#define PPC_MSR_ME  PPC_BIT(19)
#define PPC_MSR_FE0 PPC_BIT(20)
#define PPC_MSR_SE  PPC_BIT(21)
#define PPC_MSR_BE  PPC_BIT(22)
#define PPC_MSR_FE1 PPC_BIT(23)
#define PPC_MSR_IP  PPC_BIT(25)
#define PPC_MSR_IR  PPC_BIT(26)
#define PPC_MSR_DR  PPC_BIT(27)
#define PPC_MSR_PM  PPC_BIT(29)
#define PPC_MSR_RI  PPC_BIT(30)
#define PPC_MSR_LE  PPC_BIT(31)

#define PPC_EAR_ENABLE 0x80000000u
#define PPC_SRR1_MACHINE_CHECK_DCBZL PPC_BIT(10)

static u32 exception_vector_address(u32 msr, u32 vector) {
    return ((msr & PPC_MSR_IP) ? 0xFFF00000u : 0u) + vector;
}

static u32 exception_msr(u32 old_msr, u32 exception) {
    u32 clear = PPC_MSR_POW | PPC_MSR_EE | PPC_MSR_PR | PPC_MSR_FP |
                PPC_MSR_FE0 | PPC_MSR_SE | PPC_MSR_BE | PPC_MSR_FE1 |
                PPC_MSR_IR | PPC_MSR_DR | PPC_MSR_PM | PPC_MSR_RI |
                PPC_MSR_LE;
    if (exception & PPC_EXC_MACHINE_CHECK)
        clear |= PPC_MSR_ME;

    u32 next = old_msr & ~clear;
    if (old_msr & PPC_MSR_ILE)
        next |= PPC_MSR_LE;
    return next;
}

u64 mem_read64_slow(CPUState* cpu, u32 addr) {
    u32 avail;
    u8* host = resolve_addr(cpu, addr, &avail);
    if (!host || avail < 8) {
        if (cpu->external_read)
            return cpu->external_read(cpu, addr, 8);
        fprintf(stderr, "warn: read64 from unmapped 0x%08X\n", addr);
        return 0;
    }
    return read_be64(host);
}

void mem_write64_slow(CPUState* cpu, u32 addr, u64 value) {
    if (gather_pipe_store(addr, value, 8))
        return;
    u32 avail;
    u8* host = resolve_addr(cpu, addr, &avail);
    if (!host || avail < 8) {
        if (cpu->external_write) {
            cpu->external_write(cpu, addr, value, 8);
            return;
        }
        fprintf(stderr, "warn: write64 to unmapped 0x%08X\n", addr);
        return;
    }
    clear_matching_reservation(cpu, addr);
    ppc_journal_ram_write(cpu, host, 8);
    write_be64(host, value);
}

u32 mem_read32_slow(CPUState* cpu, u32 addr) {
    u32 avail;
    u8* host = resolve_addr(cpu, addr, &avail);
    if (!host || avail < 4) {
        if (cpu->external_read)
            return (u32)cpu->external_read(cpu, addr, 4);
        fprintf(stderr, "warn: read32 from unmapped 0x%08X\n", addr);
        return 0;
    }
    return read_be32(host);
}

void mem_write32_slow(CPUState* cpu, u32 addr, u32 value) {
    if (gather_pipe_store(addr, value, 4))
        return;
    u32 avail;
    u8* host = resolve_addr(cpu, addr, &avail);
    if (!host || avail < 4) {
        if (cpu->external_write) {
            cpu->external_write(cpu, addr, value, 4);
            return;
        }
        fprintf(stderr, "warn: write32 to unmapped 0x%08X\n", addr);
        return;
    }
    clear_matching_reservation(cpu, addr);
    ppc_journal_ram_write(cpu, host, 4);
    write_be32(host, value);
}

u16 mem_read16_slow(CPUState* cpu, u32 addr) {
    u32 avail;
    u8* host = resolve_addr(cpu, addr, &avail);
    if (!host || avail < 2) {
        if (cpu->external_read)
            return (u16)cpu->external_read(cpu, addr, 2);
        fprintf(stderr, "warn: read16 from unmapped 0x%08X\n", addr);
        return 0;
    }
    return read_be16(host);
}

void mem_write16_slow(CPUState* cpu, u32 addr, u16 value) {
    if (gather_pipe_store(addr, value, 2))
        return;
    u32 avail;
    u8* host = resolve_addr(cpu, addr, &avail);
    if (!host || avail < 2) {
        if (cpu->external_write) {
            cpu->external_write(cpu, addr, value, 2);
            return;
        }
        fprintf(stderr, "warn: write16 to unmapped 0x%08X\n", addr);
        return;
    }
    clear_matching_reservation(cpu, addr);
    ppc_journal_ram_write(cpu, host, 2);
    write_be16(host, value);
}

u8 mem_read8_slow(CPUState* cpu, u32 addr) {
    u32 avail;
    u8* host = resolve_addr(cpu, addr, &avail);
    if (!host) {
        if (cpu->external_read)
            return (u8)cpu->external_read(cpu, addr, 1);
        fprintf(stderr, "warn: read8 from unmapped 0x%08X\n", addr);
        return 0;
    }
    return *host;
}

void mem_write8_slow(CPUState* cpu, u32 addr, u8 value) {
    if (gather_pipe_store(addr, value, 1))
        return;
    u32 avail;
    u8* host = resolve_addr(cpu, addr, &avail);
    if (!host) {
        if (cpu->external_write) {
            cpu->external_write(cpu, addr, value, 1);
            return;
        }
        fprintf(stderr, "warn: write8 to unmapped 0x%08X\n", addr);
        return;
    }
    clear_matching_reservation(cpu, addr);
    ppc_journal_ram_write(cpu, host, 1);
    *host = value;
}

bool ppc_add_overflowed(u32 a, u32 b, u32 result) {
    return (((a ^ result) & (b ^ result)) >> 31) != 0;
}

void ppc_set_xer_ov(CPUState* cpu, bool ov) {
    cpu->xer = (cpu->xer & ~0x40000000u) | (ov ? 0x40000000u : 0u);
    if (ov)
        cpu->xer |= 0x80000000u;
}

void ppc_take_exception(CPUState* cpu, u32 exception, u32 vector, u32 srr0, u32 srr1_info) {
    u32 old_msr = cpu->msr;
    cpu->srr0 = srr0;
    cpu->srr1 = (old_msr & PPC_MSR_RFI_MASK) | srr1_info;
    cpu->exception |= exception;
    cpu->msr = exception_msr(old_msr, exception);
    cpu->pc = exception_vector_address(cpu->msr, vector);
}

void ppc_program_exception(CPUState* cpu, u32 cause, u32 cia) {
    cpu->program_exception |= cause;
    ppc_take_exception(cpu, PPC_EXC_PROGRAM, PPC_VECTOR_PROGRAM, cia, cause);
}

/* Depth of the generated code's native call chain. Cross-chunk direct calls
   turn guest recursion into host recursion, and the chunk headers declare this
   extern so all ~180 chunk translation units share one counter -- as a static
   in the header each would get its own and the guard would bound nothing.
   It lives here because the C backend compiles only chunk source files, so generated.c
   is not linked and cannot hold the definition. */
unsigned dolrecomp_call_depth = 0;

/* ppc_fp_available is now a static inline fast path in cpu.h. */

f64 g_fprf_value;
u8  g_fprf_kind;

#ifdef RECOMP_FPRF_TRACE
u32 g_fprf_pc;
void ppc_fprf_trace_mffs(CPUState* cpu) {
    static unsigned long long n;
    fprintf(stderr, "[fprf] %llu writer=%08X fprf=%02X\n",
            n++, g_fprf_pc, (cpu->fpscr >> 12) & 0x1Fu);
}
#endif

/* Defined further down next to the FP helpers; forward-declared so the
 * materialiser can sit beside the exit points that call it. */
static u32 classify_f64(f64 value);
static u32 classify_f32(f32 value);

void ppc_fprf_materialize(CPUState* cpu) {
    /* Classification must match dolrecomp_classify_d / _s in the generated
     * header exactly; those are the eager path this replaces. */
    u32 fprf = (g_fprf_kind == 1u) ? classify_f32((f32)g_fprf_value)
                                   : classify_f64(g_fprf_value);
    cpu->fpscr = (cpu->fpscr & ~(0x1Fu << 12)) | (fprf << 12);
    g_fprf_kind = 0u;
}

void ppc_fallback_instruction(CPUState* cpu, u32 raw, u32 cia) {
    /* The interpreter is about to read this state. */
    ppc_fprf_flush(cpu);
    if (cpu->instruction_fallback) {
        cpu->instruction_fallback(cpu, raw, cia);
        return;
    }

    (void)raw;
    ppc_program_exception(cpu, PPC_PROGRAM_ILLEGAL, cia);
}

bool ppc_host_call(CPUState* cpu, u32 address) {
    ppc_fprf_flush(cpu);
    return cpu->host_call ? cpu->host_call(cpu, address) : false;
}

void ppc_system_call_exception(CPUState* cpu, u32 cia) {
    ppc_take_exception(cpu, PPC_EXC_SYSTEM_CALL, PPC_VECTOR_SYSTEM_CALL, cia + 4u, 0);
}

void ppc_dsi_exception(CPUState* cpu, u32 ea, u32 cia, u32 dsisr) {
    cpu->dar = ea;
    cpu->dsisr = dsisr;
    ppc_take_exception(cpu, PPC_EXC_DSI, PPC_VECTOR_DSI, cia, 0);
}

void ppc_alignment_exception(CPUState* cpu, u32 ea, u32 cia) {
    cpu->dar = ea;
    ppc_take_exception(cpu, PPC_EXC_ALIGNMENT, PPC_VECTOR_ALIGNMENT, cia, 0);
}

u32 ppc_mftb(CPUState* cpu, u16 tbr, u32 cia) {
    if (tbr == 268)
        return (u32)cpu->timebase;
    if (tbr == 269)
        return (u32)(cpu->timebase >> 32);

    ppc_program_exception(cpu, PPC_PROGRAM_ILLEGAL, cia);
    return 0;
}

enum {
    SPR_READ = 1u << 0,
    SPR_WRITE = 1u << 1,
    SPR_STORAGE = 1u << 2,
    SPR_RW = SPR_READ | SPR_WRITE | SPR_STORAGE,
    SPR_RO = SPR_READ | SPR_STORAGE,
};

static const u8 ppc_spr_access[1024] = {
    [22] = SPR_RW,
    [25] = SPR_RW,
    [272] = SPR_RW,
    [273] = SPR_RW,
    [274] = SPR_RW,
    [275] = SPR_RW,
    [287] = SPR_RO,
    [528] = SPR_RW,
    [529] = SPR_RW,
    [530] = SPR_RW,
    [531] = SPR_RW,
    [532] = SPR_RW,
    [533] = SPR_RW,
    [534] = SPR_RW,
    [535] = SPR_RW,
    [536] = SPR_RW,
    [537] = SPR_RW,
    [538] = SPR_RW,
    [539] = SPR_RW,
    [540] = SPR_RW,
    [541] = SPR_RW,
    [542] = SPR_RW,
    [543] = SPR_RW,
    [921] = SPR_RW,
    [922] = SPR_RW,
    [923] = SPR_RW,
    [936] = SPR_READ | SPR_WRITE,
    [937] = SPR_READ | SPR_WRITE,
    [938] = SPR_READ | SPR_WRITE,
    [939] = SPR_READ | SPR_WRITE,
    [940] = SPR_READ | SPR_WRITE,
    [941] = SPR_READ | SPR_WRITE,
    [942] = SPR_READ | SPR_WRITE,
    [952] = SPR_RW,
    [953] = SPR_RW,
    [954] = SPR_RW,
    [955] = SPR_RW,
    [956] = SPR_RW,
    [957] = SPR_RW,
    [958] = SPR_RW,
    [1008] = SPR_RW,
    [1009] = SPR_RW,
    [1010] = SPR_RW,
    [1013] = SPR_RW,
    [1017] = SPR_RW,
    [1019] = SPR_RW,
    [1020] = SPR_RW,
    [1021] = SPR_RW,
    [1022] = SPR_RW,
};

static u16 ppc_spr_storage_index(u16 spr) {
    switch (spr) {
    case 936: return 952;
    case 937: return 953;
    case 938: return 954;
    case 939: return 955;
    case 940: return 956;
    case 941: return 957;
    case 942: return 958;
    default: return spr;
    }
}

u32 ppc_mfspr(CPUState* cpu, u16 spr, u32 cia) {
    switch (spr) {
    case 1:
        return cpu->xer;
    case 8:
        return cpu->lr;
    case 9:
        return cpu->ctr;
    case 18:
        return cpu->dsisr;
    case 19:
        return cpu->dar;
    case 26:
        return cpu->srr0;
    case 27:
        return cpu->srr1;
    case 282:
        return cpu->ear;
    case 912:
    case 913:
    case 914:
    case 915:
    case 916:
    case 917:
    case 918:
    case 919:
        return cpu->gqr[spr - 912];
    case 920:
        return cpu->hid2;
    default:
        break;
    }

    if (spr < 1024 && (ppc_spr_access[spr] & SPR_READ))
        return cpu->spr[ppc_spr_storage_index(spr)];

    ppc_program_exception(cpu, PPC_PROGRAM_ILLEGAL, cia);
    return 0;
}

void ppc_mtspr(CPUState* cpu, u16 spr, u32 value, u32 cia) {
    switch (spr) {
    case 1:
        cpu->xer = value;
        return;
    case 8:
        cpu->lr = value;
        return;
    case 9:
        cpu->ctr = value;
        return;
    case 18:
        cpu->dsisr = value;
        return;
    case 19:
        cpu->dar = value;
        return;
    case 26:
        cpu->srr0 = value;
        return;
    case 27:
        cpu->srr1 = value;
        return;
    case 282:
        cpu->ear = value;
        return;
    case 284:
        cpu->timebase = (cpu->timebase & 0xFFFFFFFF00000000ull) | value;
        return;
    case 285:
        cpu->timebase = ((u64)value << 32) | (cpu->timebase & 0xFFFFFFFFull);
        return;
    case 912:
    case 913:
    case 914:
    case 915:
    case 916:
    case 917:
    case 918:
    case 919:
        cpu->gqr[spr - 912] = value;
        return;
    case 920:
        cpu->hid2 = value;
        return;
    default:
        break;
    }

    if (spr < 1024 && (ppc_spr_access[spr] & SPR_WRITE)) {
        cpu->spr[ppc_spr_storage_index(spr)] = value;
        return;
    }

    ppc_program_exception(cpu, PPC_PROGRAM_ILLEGAL, cia);
}

static f32 f32_value(u32 bits) {
    f32 value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static u32 f32_bits(f32 value) {
    u32 bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static bool f32_is_denormal(f32 value) {
    u32 bits = f32_bits(value);
    return (bits & 0x7F800000u) == 0 && (bits & 0x007FFFFFu) != 0;
}

/* ---- Paired-single quantised load/store: collapse the helper call chain ----
 * psq_l/psq_st is the single hottest thing this core does during FMV -- 39% of
 * the CPU thread, measured -- and one non-w psq_l was costing about six
 * out-of-line calls: ppc_psq_load -> psq_check_enabled, then
 * psq_access_is_valid + psq_load_value once per lane. Profiles showed all of
 * them as separate symbols (psq_store_value 12.8%, psq_load_value 9.3%,
 * psq_access_is_valid 1.3%), so -O3 + LTO was NOT inlining them despite their
 * being static in this very file: the type switch makes them look expensive to
 * the cost model. Forcing it collapses the chain to a single call.
 *
 * Measured over four FMV profiles (work/perf-fmv/): psq cost per unit of
 * recompiled-chunk work fell from 0.890 to 0.506, about -43%, with the two
 * groups' ranges not overlapping. Guest state is bit-identical to the previous
 * build over 1800 frames and the module came out 208 bytes smaller.
 *
 * Do NOT extend this into a type-specialised fast path -- that was tried and
 * measured (work/experiments/psq-typed-fastpath-REJECTED.patch): dispatching the
 * type once for both lanes, inlining psq_quantize_int and dropping the
 * redundant second psq_access_is_valid landed *inside* the noise of this
 * version. The dispatch was never the cost; the loads, stores and FP conversion
 * are. Only collapsing genuine out-of-line call chains has paid here. */
#define DOLRECOMP_PSQ_FI static inline __attribute__((always_inline))

DOLRECOMP_PSQ_FI s32 gqr_scale(u32 value) {
    return sign_extend(value & 0x3Fu, 6);
}

DOLRECOMP_PSQ_FI u32 psq_type_size(u8 type) {
    switch (type) {
    case 0: return 4;
    case 4:
    case 6: return 1;
    case 5:
    case 7: return 2;
    default: return 0;
    }
}

DOLRECOMP_PSQ_FI bool psq_access_is_valid(CPUState* cpu, u8 type, u32 ea, u32 cia) {
    if (psq_type_size(type) == 0) {
        ppc_program_exception(cpu, PPC_PROGRAM_ILLEGAL, cia);
        return false;
    }

    if (type == 0 && (ea & 3u) != 0) {
        ppc_alignment_exception(cpu, ea, cia);
        return false;
    }

    return true;
}

/* GQR scale is a 6-bit signed field (-32..31) and psq values are 8/16-bit
 * integers, so 2^e here is always a normal f64 and the multiply is exact —
 * bit-identical to ldexp over this domain, without the libm PLT call. */
static inline f64 psq_pow2i(s32 e) {
    union { u64 u; f64 d; } v;
    v.u = (u64)(u32)(1023 + e) << 52;
    return v.d;
}

DOLRECOMP_PSQ_FI f64 psq_load_value(CPUState* cpu, u32 ea, u8 type, s32 scale) {
    switch (type) {
    case 0:
        return (f64)f32_value(mem_read32(cpu, ea));
    case 4:
        return (f64)(f32)((f64)mem_read8(cpu, ea) * psq_pow2i(-scale));
    case 5:
        return (f64)(f32)((f64)mem_read16(cpu, ea) * psq_pow2i(-scale));
    case 6:
        return (f64)(f32)((f64)(s8)mem_read8(cpu, ea) * psq_pow2i(-scale));
    case 7:
        return (f64)(f32)((f64)(s16)mem_read16(cpu, ea) * psq_pow2i(-scale));
    default:
        return 0.0;
    }
}

static s64 psq_quantize_int(f64 value, s64 min_value, s64 max_value, s32 scale) {
    if (isnan(value))
        return max_value;
    if (isinf(value))
        return value < 0.0 ? min_value : max_value;

    f64 scaled = trunc(value * psq_pow2i(scale));
    if (scaled <= (f64)min_value)
        return min_value;
    if (scaled >= (f64)max_value)
        return max_value;
    return (s64)scaled;
}

DOLRECOMP_PSQ_FI void psq_store_value(CPUState* cpu, u32 ea, u8 type, s32 scale, f64 value) {
    switch (type) {
    case 0: {
        f32 single = (f32)value;
        mem_write32(cpu, ea, f32_is_denormal(single) ? 0u : f32_bits(single));
        break;
    }
    case 4:
        mem_write8(cpu, ea, (u8)psq_quantize_int(value, 0, 255, scale));
        break;
    case 5:
        mem_write16(cpu, ea, (u16)psq_quantize_int(value, 0, 65535, scale));
        break;
    case 6:
        mem_write8(cpu, ea, (u8)(s8)psq_quantize_int(value, -128, 127, scale));
        break;
    case 7:
        mem_write16(cpu, ea, (u16)(s16)psq_quantize_int(value, -32768, 32767, scale));
        break;
    }
}

DOLRECOMP_PSQ_FI bool psq_check_enabled(CPUState* cpu, bool indexed, u32 cia) {
    if ((cpu->hid2 & PPC_HID2_PSE) == 0 || (!indexed && (cpu->hid2 & PPC_HID2_LSQE) == 0)) {
        ppc_program_exception(cpu, PPC_PROGRAM_ILLEGAL, cia);
        return false;
    }
    return true;
}

bool ppc_psq_load(CPUState* cpu, u8 frD, u32 ea, bool w, u8 gqr_index, bool indexed, u32 cia) {
    if (!psq_check_enabled(cpu, indexed, cia))
        return false;

    u32 gqr = cpu->gqr[gqr_index & 7u];
    s32 scale = gqr_scale(gqr >> 24);
    u8 type = (u8)((gqr >> 16) & 7u);
    u32 size = psq_type_size(type);
    if (!psq_access_is_valid(cpu, type, ea, cia))
        return false;

    cpu->fpr[frD] = psq_load_value(cpu, ea, type, scale);
    if (w) {
        cpu->ps1[frD] = 1.0;
    } else {
        u32 ps1_ea = ea + size;
        if (!psq_access_is_valid(cpu, type, ps1_ea, cia))
            return false;
        cpu->ps1[frD] = psq_load_value(cpu, ps1_ea, type, scale);
    }
    return true;
}

bool ppc_psq_store(CPUState* cpu, u8 frS, u32 ea, bool w, u8 gqr_index, bool indexed, u32 cia) {
    if (!psq_check_enabled(cpu, indexed, cia))
        return false;

    u32 gqr = cpu->gqr[gqr_index & 7u];
    s32 scale = gqr_scale(gqr >> 8);
    u8 type = (u8)(gqr & 7u);
    u32 size = psq_type_size(type);
    if (!psq_access_is_valid(cpu, type, ea, cia))
        return false;

    psq_store_value(cpu, ea, type, scale, cpu->fpr[frS]);
    if (!w) {
        u32 ps1_ea = ea + size;
        if (!psq_access_is_valid(cpu, type, ps1_ea, cia))
            return false;
        psq_store_value(cpu, ps1_ea, type, scale, cpu->ps1[frS]);
    }
    return true;
}

void ppc_rfi(CPUState* cpu, u32 cia) {
    if (cpu->msr & PPC_MSR_PR) {
        ppc_program_exception(cpu, PPC_PROGRAM_PRIV, cia);
        return;
    }

    cpu->msr = (cpu->msr & ~PPC_MSR_RFI_MASK) | (cpu->srr1 & PPC_MSR_RFI_MASK);
    cpu->msr &= ~PPC_MSR_POW;
    cpu->pc = cpu->srr0 & ~3u;
}

void ppc_dcbz_l(CPUState* cpu, u32 ea, u32 cia) {
    if (cpu->msr & PPC_MSR_PR) {
        ppc_program_exception(cpu, PPC_PROGRAM_PRIV, cia);
        return;
    }

    if ((cpu->hid2 & PPC_HID2_LCE) == 0) {
        ppc_program_exception(cpu, PPC_PROGRAM_ILLEGAL, cia);
        return;
    }

    u32 block = ea & ~31u;
    u32 slot = (block >> 5) & 511u;
    bool hit = cpu->locked_cache_valid[slot] && cpu->locked_cache_tag[slot] == block;
    bool first_hit_error = hit && (cpu->hid2 & PPC_HID2_DCHERR) == 0;

    if (hit) {
        cpu->hid2 |= PPC_HID2_DCHERR;
        if (first_hit_error && (cpu->hid2 & PPC_HID2_DCHEE) &&
            (cpu->msr & PPC_MSR_EE) && (cpu->msr & PPC_MSR_ME)) {
            ppc_take_exception(cpu, PPC_EXC_MACHINE_CHECK, PPC_VECTOR_MACHINE_CHECK,
                               cia, PPC_SRR1_MACHINE_CHECK_DCBZL);
        }
    } else {
        cpu->locked_cache_valid[slot] = true;
        cpu->locked_cache_tag[slot] = block;
    }

    for (u32 i = 0; i < 32; i += 4)
        mem_write32(cpu, block + i, 0);
}

u32 ppc_eciwx(CPUState* cpu, u32 ea, u32 cia) {
    if ((cpu->ear & PPC_EAR_ENABLE) == 0) {
        ppc_dsi_exception(cpu, ea, cia, PPC_DSI_EAR_DISABLED);
        return 0;
    }

    if ((ea & 3u) != 0) {
        ppc_alignment_exception(cpu, ea, cia);
        return 0;
    }

    u8 rid = (u8)(cpu->ear & 0xFu);
    cpu->external_addr = ea;
    cpu->external_rid = rid;
    cpu->external_read_count++;
    if (cpu->external_read32)
        return cpu->external_read32(cpu, ea, rid);
    return 0;
}

void ppc_ecowx(CPUState* cpu, u32 ea, u32 value, u32 cia) {
    if ((cpu->ear & PPC_EAR_ENABLE) == 0) {
        ppc_dsi_exception(cpu, ea, cia, PPC_DSI_EAR_DISABLED);
        return;
    }

    if ((ea & 3u) != 0) {
        ppc_alignment_exception(cpu, ea, cia);
        return;
    }

    u8 rid = (u8)(cpu->ear & 0xFu);
    cpu->external_addr = ea;
    cpu->external_value = value;
    cpu->external_rid = rid;
    cpu->external_write_count++;
    if (cpu->external_write32)
        cpu->external_write32(cpu, ea, value, rid);
}

void ppc_tlbie(CPUState* cpu, u32 ea, u32 cia) {
    if (cpu->msr & PPC_MSR_PR) {
        ppc_program_exception(cpu, PPC_PROGRAM_PRIV, cia);
        return;
    }

    cpu->tlb_last_vps = (ea >> 12) & 0xFFFFu;
    cpu->tlb_last_index = (ea >> 12) & 0x3Fu;
    cpu->tlb_invalidate_count++;
}

bool ppc_trap_condition(u8 to, u32 a, u32 b) {
    s32 sa = (s32)a;
    s32 sb = (s32)b;

    return ((sa < sb) && (to & 0x10u)) ||
           ((sa > sb) && (to & 0x08u)) ||
           ((sa == sb) && (to & 0x04u)) ||
           ((a < b) && (to & 0x02u)) ||
           ((a > b) && (to & 0x01u));
}

typedef struct {
    s32 base;
    s32 dec;
} EstimateEntry;

/* Adapted from Dolphin Emulator's Common/FloatUtils.cpp.
 * Copyright 2018 Dolphin Emulator Project, GPL-2.0-or-later. */
static const EstimateEntry frsqrte_table[32] = {
    {0x1a7e800, -0x568}, {0x17cb800, -0x4f3}, {0x1552800, -0x48d}, {0x130c000, -0x435},
    {0x10f2000, -0x3e7}, {0x0eff000, -0x3a2}, {0x0d2e000, -0x365}, {0x0b7c000, -0x32e},
    {0x09e5000, -0x2fc}, {0x0867000, -0x2d0}, {0x06ff000, -0x2a8}, {0x05ab800, -0x283},
    {0x046a000, -0x261}, {0x0339800, -0x243}, {0x0218800, -0x226}, {0x0105800, -0x20b},
    {0x3ffa000, -0x7a4}, {0x3c29000, -0x700}, {0x38aa000, -0x670}, {0x3572000, -0x5f2},
    {0x3279000, -0x584}, {0x2fb7000, -0x524}, {0x2d26000, -0x4cc}, {0x2ac0000, -0x47e},
    {0x2881000, -0x43a}, {0x2665000, -0x3fa}, {0x2468000, -0x3c2}, {0x2287000, -0x38e},
    {0x20c1000, -0x35e}, {0x1f12000, -0x332}, {0x1d79000, -0x30a}, {0x1bf4000, -0x2e6},
};

static const EstimateEntry fres_table[32] = {
    {0x7ff800, 0x3e1}, {0x783800, 0x3a7}, {0x70ea00, 0x371}, {0x6a0800, 0x340},
    {0x638800, 0x313}, {0x5d6200, 0x2ea}, {0x579000, 0x2c4}, {0x520800, 0x2a0},
    {0x4cc800, 0x27f}, {0x47ca00, 0x261}, {0x430800, 0x245}, {0x3e8000, 0x22a},
    {0x3a2c00, 0x212}, {0x360800, 0x1fb}, {0x321400, 0x1e5}, {0x2e4a00, 0x1d1},
    {0x2aa800, 0x1be}, {0x272c00, 0x1ac}, {0x23d600, 0x19b}, {0x209e00, 0x18b},
    {0x1d8800, 0x17c}, {0x1a9000, 0x16e}, {0x17ae00, 0x15b}, {0x14f800, 0x15b},
    {0x124400, 0x143}, {0x0fbe00, 0x143}, {0x0d3800, 0x12d}, {0x0ade00, 0x12d},
    {0x088400, 0x11a}, {0x065000, 0x11a}, {0x041c00, 0x108}, {0x020c00, 0x106},
};

static u64 f64_bits(f64 value) {
    u64 bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static f64 f64_value(u64 bits) {
    f64 value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

f64 ppc_approx_rsqrt(f64 value) {
    u64 bits = f64_bits(value);
    u64 mantissa = bits & 0x000FFFFFFFFFFFFFull;
    u64 sign = bits & 0x8000000000000000ull;
    s64 exponent = (s64)(bits & 0x7FF0000000000000ull);

    if (mantissa == 0 && exponent == 0)
        return f64_value(sign | 0x7FF0000000000000ull);
    if (exponent == (s64)0x7FF0000000000000ull) {
        if (mantissa == 0)
            return sign ? f64_value(0x7FF8000000000000ull) : 0.0;
        return f64_value(bits | 0x0008000000000000ull);
    }
    if (sign)
        return f64_value(0x7FF8000000000000ull);

    if (exponent == 0) {
        do {
            exponent -= (s64)0x0010000000000000ull;
            mantissa <<= 1;
        } while ((mantissa & 0x0010000000000000ull) == 0);
        mantissa &= 0x000FFFFFFFFFFFFFull;
        exponent += (s64)0x0010000000000000ull;
    }

    u64 exponent_lsb = (u64)exponent & 0x0010000000000000ull;
    exponent = ((s64)0x3FF0000000000000ull -
                (exponent - (s64)0x3FE0000000000000ull) / 2) &
               (s64)0x7FF0000000000000ull;
    u32 i = (u32)((exponent_lsb | mantissa) >> 37);
    const EstimateEntry* entry = &frsqrte_table[i / 2048u];
    bits = (u64)exponent |
           ((u64)(entry->base + entry->dec * (s32)(i % 2048u)) << 26);
    return f64_value(bits);
}

f64 ppc_approx_reciprocal(f64 value) {
    u64 bits = f64_bits(value);
    u64 mantissa = bits & 0x000FFFFFFFFFFFFFull;
    u64 sign = bits & 0x8000000000000000ull;
    u64 exponent = bits & 0x7FF0000000000000ull;

    if (mantissa == 0 && exponent == 0)
        return f64_value(sign | 0x7FF0000000000000ull);
    if (exponent == 0x7FF0000000000000ull) {
        if (mantissa == 0)
            return f64_value(sign);
        return f64_value(bits | 0x0008000000000000ull);
    }
    if (exponent < (895ull << 52))
        return f64_value(sign | 0x47EFFFFFE0000000ull);
    if (exponent >= (1149ull << 52))
        return f64_value(sign);

    exponent = 0x7FD0000000000000ull - exponent;
    u32 i = (u32)(mantissa >> 37);
    const EstimateEntry* entry = &fres_table[i / 1024u];
    bits = sign | exponent |
           ((u64)(entry->base - (entry->dec * (s32)(i % 1024u) + 1) / 2) << 29);
    return f64_value(bits);
}

void ppc_fpscr_updated(CPUState* cpu) {
    const u32 vx_any = 0x01F80700u;
    const u32 any_e = 0x000000F8u;
    u32 fpscr = cpu->fpscr;
    fpscr = (fpscr & ~0x20000000u) | ((fpscr & vx_any) ? 0x20000000u : 0u);
    fpscr = (fpscr & ~0x40000000u) |
            ((((fpscr >> 22) & fpscr & any_e) != 0) ? 0x40000000u : 0u);
    cpu->fpscr = fpscr;
}

static void set_fp_exception(CPUState* cpu, u32 bit) {
    if ((cpu->fpscr & bit) != bit)
        cpu->fpscr |= 0x80000000u;
    cpu->fpscr |= bit;
    ppc_fpscr_updated(cpu);
}

static bool is_snan(f64 value) {
    u64 bits = f64_bits(value);
    u64 fraction = bits & 0x000FFFFFFFFFFFFFull;
    return (bits & 0x7FF0000000000000ull) == 0x7FF0000000000000ull &&
           fraction != 0 && (fraction & 0x0008000000000000ull) == 0;
}

static u32 classify_f64(f64 value) {
    u64 bits = f64_bits(value);
    u64 sign = bits >> 63;
    u64 exponent = bits & 0x7FF0000000000000ull;
    u64 fraction = bits & 0x000FFFFFFFFFFFFFull;
    if (exponent == 0x7FF0000000000000ull)
        return fraction ? 0x11u : (sign ? 0x09u : 0x05u);
    if (exponent == 0)
        return fraction ? (sign ? 0x18u : 0x14u) : (sign ? 0x12u : 0x02u);
    return sign ? 0x08u : 0x04u;
}

static u32 classify_f32(f32 value) {
    u32 bits;
    memcpy(&bits, &value, sizeof(bits));
    u32 sign = bits >> 31;
    u32 exponent = bits & 0x7F800000u;
    u32 fraction = bits & 0x007FFFFFu;
    if (exponent == 0x7F800000u)
        return fraction ? 0x11u : (sign ? 0x09u : 0x05u);
    if (exponent == 0)
        return fraction ? (sign ? 0x18u : 0x14u) : (sign ? 0x12u : 0x02u);
    return sign ? 0x08u : 0x04u;
}

static void set_fprf(CPUState* cpu, u32 value) {
    /* These helpers classify their own result, so any value the generated code
     * left pending belongs to an EARLIER instruction and must not survive to
     * overwrite this one at the next flush. Dropping it here is what makes the
     * lazy path bit-exact -- without it the frame hash diverges. */
    g_fprf_kind = 0u;
    PPC_FPRF_TAG(0xFFFFFFFFu);
    cpu->fpscr = (cpu->fpscr & ~(0x1Fu << 12)) | ((value & 0x1Fu) << 12);
}

bool ppc_fres(CPUState* cpu, f64 value, f64* result) {
    /* Land any deferred FPRF before this helper touches FPSCR. These all
     * read-modify-write bits 13/14 (FI/FR), which sit INSIDE the 0x1F<<12
     * FPRF mask, so a flush arriving afterwards would overwrite them and
     * the frame hash diverges -- measured at frame 1280, one word at
     * 0x804072A4 differing by exactly bit 13. Flushing on entry restores
     * the eager ordering. */
    ppc_fprf_flush(cpu);
    if (value == 0.0) {
        set_fp_exception(cpu, 0x04000000u);
        cpu->fpscr &= ~0x00006000u;
        if (cpu->fpscr & 0x10u)
            return false;
    } else if (is_snan(value)) {
        set_fp_exception(cpu, 0x01000000u);
        cpu->fpscr &= ~0x00006000u;
        if (cpu->fpscr & 0x80u)
            return false;
    } else if (isnan(value) || isinf(value)) {
        cpu->fpscr &= ~0x00006000u;
    }

    *result = ppc_approx_reciprocal(value);
    set_fprf(cpu, classify_f32((f32)*result));
    return true;
}

bool ppc_frsqrte(CPUState* cpu, f64 value, f64* result) {
    /* Land any deferred FPRF before this helper touches FPSCR. These all
     * read-modify-write bits 13/14 (FI/FR), which sit INSIDE the 0x1F<<12
     * FPRF mask, so a flush arriving afterwards would overwrite them and
     * the frame hash diverges -- measured at frame 1280, one word at
     * 0x804072A4 differing by exactly bit 13. Flushing on entry restores
     * the eager ordering. */
    ppc_fprf_flush(cpu);
    if (value < 0.0) {
        set_fp_exception(cpu, 0x00000200u);
        cpu->fpscr &= ~0x00006000u;
        if (cpu->fpscr & 0x80u)
            return false;
    } else if (value == 0.0) {
        set_fp_exception(cpu, 0x04000000u);
        cpu->fpscr &= ~0x00006000u;
        if (cpu->fpscr & 0x10u)
            return false;
    } else if (is_snan(value)) {
        set_fp_exception(cpu, 0x01000000u);
        cpu->fpscr &= ~0x00006000u;
        if (cpu->fpscr & 0x80u)
            return false;
    } else if (isnan(value) || isinf(value)) {
        cpu->fpscr &= ~0x00006000u;
    }

    *result = ppc_approx_rsqrt(value);
    set_fprf(cpu, classify_f64(*result));
    return true;
}

void ppc_ps_res(CPUState* cpu, f64 a, f64 b, f64* result_a, f64* result_b) {
    /* Land any deferred FPRF before this helper touches FPSCR. These all
     * read-modify-write bits 13/14 (FI/FR), which sit INSIDE the 0x1F<<12
     * FPRF mask, so a flush arriving afterwards would overwrite them and
     * the frame hash diverges -- measured at frame 1280, one word at
     * 0x804072A4 differing by exactly bit 13. Flushing on entry restores
     * the eager ordering. */
    ppc_fprf_flush(cpu);
    if (a == 0.0 || b == 0.0) {
        set_fp_exception(cpu, 0x04000000u);
        cpu->fpscr &= ~0x00006000u;
    }
    if (is_snan(a) || is_snan(b))
        set_fp_exception(cpu, 0x01000000u);
    if (isnan(a) || isinf(a) || isnan(b) || isinf(b))
        cpu->fpscr &= ~0x00006000u;
    *result_a = ppc_approx_reciprocal(a);
    *result_b = ppc_approx_reciprocal(b);
    set_fprf(cpu, classify_f32((f32)*result_a));
}

void ppc_ps_rsqrte(CPUState* cpu, f64 a, f64 b, f64* result_a, f64* result_b) {
    /* Land any deferred FPRF before this helper touches FPSCR. These all
     * read-modify-write bits 13/14 (FI/FR), which sit INSIDE the 0x1F<<12
     * FPRF mask, so a flush arriving afterwards would overwrite them and
     * the frame hash diverges -- measured at frame 1280, one word at
     * 0x804072A4 differing by exactly bit 13. Flushing on entry restores
     * the eager ordering. */
    ppc_fprf_flush(cpu);
    if (a == 0.0 || b == 0.0) {
        set_fp_exception(cpu, 0x04000000u);
        cpu->fpscr &= ~0x00006000u;
    }
    if (a < 0.0 || b < 0.0) {
        set_fp_exception(cpu, 0x00000200u);
        cpu->fpscr &= ~0x00006000u;
    }
    if (is_snan(a) || is_snan(b))
        set_fp_exception(cpu, 0x01000000u);
    if (isnan(a) || isinf(a) || isnan(b) || isinf(b))
        cpu->fpscr &= ~0x00006000u;
    *result_a = ppc_approx_rsqrt(a);
    *result_b = ppc_approx_rsqrt(b);
    set_fprf(cpu, classify_f32((f32)*result_a));
}

static unsigned leading_zeroes_u64(u64 value) {
    unsigned count = 0;
    while ((value & 0x8000000000000000ull) == 0) {
        value <<= 1;
        count++;
    }
    return count;
}

static f64 force_25_bit(f64 value) {
    u64 bits = f64_bits(value);
    u64 fraction = bits & 0x000FFFFFFFFFFFFFull;
    u64 keep_mask = 0xFFFFFFFFF8000000ull;
    u64 round = 0x0000000008000000ull;

    if ((bits & 0x7FF0000000000000ull) == 0 && fraction != 0) {
        unsigned shift = leading_zeroes_u64(fraction) - 11;
        if (shift < 28) {
            keep_mask = ~((1ull << (27 - shift)) - 1);
            round >>= shift;
        } else {
            keep_mask = ~0ull;
            round = 0;
        }
    }

    bits = (bits & keep_mask) + (bits & round);
    return f64_value(bits);
}

bool ppc_fma(CPUState* cpu, f64 a, f64 c, f64 b, bool single,
             bool subtract, bool negative, f64* output) {
    /* Land any deferred FPRF before this helper touches FPSCR. These all
     * read-modify-write bits 13/14 (FI/FR), which sit INSIDE the 0x1F<<12
     * FPRF mask, so a flush arriving afterwards would overwrite them and
     * the frame hash diverges -- measured at frame 1280, one word at
     * 0x804072A4 differing by exactly bit 13. Flushing on entry restores
     * the eager ordering. */
    ppc_fprf_flush(cpu);
    f64 addend = subtract ? -b : b;
    f64 result;

    if (!single) {
        result = fma(a, c, addend);
    } else {
        f64 rounded_c = force_25_bit(c);
        result = fma(a, rounded_c, addend);
        u64 bits = f64_bits(result);
        if ((bits & 0x000000001FFFFFFFull) == 0x0000000010000000ull) {
            f64 a_prime = addend - result;
            f64 b_prime = result + a_prime;
            f64 error = fma(a, rounded_c, a_prime) + (addend - b_prime);
            if (error != 0.0) {
                if ((error > 0.0) == (result > 0.0)) bits++;
                else bits--;
                result = f64_value(bits);
            }
        }
        result = (f64)(f32)result;
    }

    if (isnan(result)) {
        u32 invalid = 0;
        if (is_snan(a) || is_snan(b) || is_snan(c))
            invalid |= 0x01000000u;

        cpu->fpscr &= ~0x00006000u;
        if (isnan(a)) {
            result = f64_value(f64_bits(a) | 0x0008000000000000ull);
        } else if (isnan(b)) {
            result = f64_value(f64_bits(b) | 0x0008000000000000ull);
        } else if (isnan(c)) {
            result = f64_value(f64_bits(c) | 0x0008000000000000ull);
        } else {
            bool invalid_multiply = (a == 0.0 && isinf(c)) ||
                                    (isinf(a) && c == 0.0);
            invalid |= invalid_multiply ? 0x00100000u : 0x00800000u;
            result = f64_value(0x7FF8000000000000ull);
        }

        if (invalid) {
            set_fp_exception(cpu, invalid);
            if (cpu->fpscr & 0x80u)
                return false;
        }
    } else if (isinf(a) || isinf(b) || isinf(c)) {
        cpu->fpscr &= ~0x00006000u;
    }

    if (negative && !isnan(result))
        result = -result;
    set_fprf(cpu, single ? classify_f32((f32)result) : classify_f64(result));
    *output = result;
    return true;
}

void ppc_memory_fence(void) {
    atomic_thread_fence(memory_order_seq_cst);
}

static f64 round_nearest_even(f64 value) {
    f64 lo = floor(value);
    f64 fraction = value - lo;
    if (fraction < 0.5)
        return lo;
    if (fraction > 0.5)
        return lo + 1.0;
    return fmod(lo, 2.0) == 0.0 ? lo : lo + 1.0;
}

bool ppc_fctiw(CPUState* cpu, f64 value, bool toward_zero, u64* output) {
    /* Land any deferred FPRF before this helper touches FPSCR. These all
     * read-modify-write bits 13/14 (FI/FR), which sit INSIDE the 0x1F<<12
     * FPRF mask, so a flush arriving afterwards would overwrite them and
     * the frame hash diverges -- measured at frame 1280, one word at
     * 0x804072A4 differing by exactly bit 13. Flushing on entry restores
     * the eager ordering. */
    ppc_fprf_flush(cpu);
    f64 rounded;
    switch (toward_zero ? 1u : (cpu->fpscr & 3u)) {
    case 1: rounded = trunc(value); break;
    case 2: rounded = ceil(value); break;
    case 3: rounded = floor(value); break;
    default: rounded = round_nearest_even(value); break;
    }

    u32 result;
    bool invalid = false;
    if (isnan(value)) {
        if (is_snan(value))
            set_fp_exception(cpu, 0x01000000u);
        result = 0x80000000u;
        invalid = true;
    } else if (rounded >= 2147483648.0) {
        result = 0x7FFFFFFFu;
        invalid = true;
    } else if (rounded < -2147483648.0) {
        result = 0x80000000u;
        invalid = true;
    } else {
        result = (u32)(s32)rounded;
    }

    cpu->fpscr &= ~0x00006000u;
    if (invalid) {
        set_fp_exception(cpu, 0x00000100u);
    } else if (rounded != value) {
        set_fp_exception(cpu, 0x02000000u);
        cpu->fpscr |= 0x00004000u;
        if (fabs(rounded) > fabs(value))
            cpu->fpscr |= 0x00002000u;
    }

    if (invalid && (cpu->fpscr & 0x80u))
        return false;

    *output = 0xFFF8000000000000ull | result |
              ((result == 0 && signbit(value)) ? 0x100000000ull : 0ull);
    return true;
}

/* ---------------------------------------------------------------------------
 * Condition-register liveness counters (RECOMP_CR_STATS; see cpu.h).
 *
 * `pending` is the set of CR fields written but not yet read. A write to a
 * field already in that set is a DEAD write -- the previous value never
 * reached anything -- which is the number that decides whether eliding CR
 * computation is worth codegen work.
 *
 * Deliberately not thread-safe and deliberately not atomic: this is a counting
 * probe run on the single-threaded determinism harness, and making it atomic
 * would change the timing it is trying to describe. It is compiled out of any
 * normal build.
 * ------------------------------------------------------------------------- */
#ifdef RECOMP_CR_STATS
static unsigned long long g_cr_writes[8];
static unsigned long long g_cr_reads[8];
static unsigned long long g_cr_dead[8];
static unsigned g_cr_pending;

void ppc_cr_note_write(unsigned field) {
    field &= 7u;
    if (g_cr_pending & (1u << field))
        g_cr_dead[field]++;
    g_cr_pending |= (1u << field);
    g_cr_writes[field]++;
}

void ppc_cr_note_read(unsigned field_mask) {
    field_mask &= 0xFFu;
    g_cr_pending &= ~field_mask;
    for (unsigned f = 0; f < 8u; f++)
        if (field_mask & (1u << f))
            g_cr_reads[f]++;
}

/* XER[CA]. Unlike the CR, the guest compiler does not get to CHOOSE whether a
 * carry is produced: srawi, addc, subfc and friends set CA unconditionally by
 * architecture, whether or not anything consumes it. So the dead rate here can
 * be far higher than the CR's, and every one costs a read-modify-write of
 * ctx->xer plus the bits that compute it -- turning a one-instruction shift
 * into roughly seven. This is what the hot cluster at 0x8000DA8C is full of.
 */
static unsigned long long g_ca_writes, g_ca_reads, g_ca_dead;
static bool g_ca_pending;

void ppc_ca_note_write(void) {
    if (g_ca_pending) g_ca_dead++;
    g_ca_pending = true;
    g_ca_writes++;
}

void ppc_ca_note_read(void) {
    g_ca_pending = false;
    g_ca_reads++;
}

/* FPRF (FPSCR bits 12-16) is recomputed and stored after EVERY FP arithmetic
 * result -- 13334 emitted sites -- while this game reads FPSCR at two mffs
 * sites. It was added for lockstep fidelity against Dolphin's interpreter and
 * measured "free (within noise)", but that measurement was taken on the
 * boot/menu harness, which carries ~0.1% of a real session's paired-single
 * traffic. This counts it on actual gameplay.
 */
static unsigned long long g_fprf_writes, g_fprf_reads, g_fprf_dead;

/* Paired-single LANE operations. Hooked in the emitted dolrecomp_ps_round, so
   one increment per lane, two per two-lane op. The question it answers is
   whether interleaving ps0/ps1 and emitting packed SSE is worth a CPU ABI
   break -- which needs the dynamic volume, since static ps_* site counts have
   mispredicted execution in this codebase every time. */
static unsigned long long g_ps_lanes;
static bool g_fprf_pending;

void ppc_ps_note_lane(void) {
    g_ps_lanes++;
}

void ppc_fprf_note_write(void) {
    if (g_fprf_pending) g_fprf_dead++;
    g_fprf_pending = true;
    g_fprf_writes++;
}

void ppc_fprf_note_read(void) {
    g_fprf_pending = false;
    g_fprf_reads++;
}

__attribute__((destructor))
static void ppc_cr_report(void) {
    unsigned long long tw = 0, tr = 0, td = 0;
    for (unsigned f = 0; f < 8u; f++) { tw += g_cr_writes[f]; tr += g_cr_reads[f]; td += g_cr_dead[f]; }
    if (tw == 0) return;
    fprintf(stderr, "[cr-stats] field      writes         reads          dead   dead%%\n");
    for (unsigned f = 0; f < 8u; f++) {
        if (!g_cr_writes[f] && !g_cr_reads[f]) continue;
        fprintf(stderr, "[cr-stats] cr%u   %12llu  %12llu  %12llu  %5.1f%%\n", f,
                g_cr_writes[f], g_cr_reads[f], g_cr_dead[f],
                g_cr_writes[f] ? 100.0 * (double)g_cr_dead[f] / (double)g_cr_writes[f] : 0.0);
    }
    fprintf(stderr, "[cr-stats] TOTAL %12llu  %12llu  %12llu  %5.1f%%\n",
            tw, tr, td, 100.0 * (double)td / (double)tw);
    fprintf(stderr, "[ca-stats] xer.CA %12llu  %12llu  %12llu  %5.1f%%\n",
            g_ca_writes, g_ca_reads, g_ca_dead,
            g_ca_writes ? 100.0 * (double)g_ca_dead / (double)g_ca_writes : 0.0);
    fprintf(stderr, "[ca-stats] FPRF   %12llu  %12llu  %12llu  %5.1f%%\n",
            g_fprf_writes, g_fprf_reads, g_fprf_dead,
            g_fprf_writes ? 100.0 * (double)g_fprf_dead / (double)g_fprf_writes : 0.0);
    fprintf(stderr, "[ps-stats] ps lanes %12llu   ops(approx) %12llu\n",
            g_ps_lanes, g_ps_lanes / 2ull);
}
#endif /* RECOMP_CR_STATS */
