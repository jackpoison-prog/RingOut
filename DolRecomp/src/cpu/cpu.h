#ifndef DOLRECOMP_CPU_H
#define DOLRECOMP_CPU_H

#include "common/types.h"

/* Chunk functions' calling convention (--preserve-none; see emitter.c). Empty
 * where the compiler lacks the attribute (clang < 19, gcc), which only costs
 * the saving: every declaration expands the same way, so they always agree. */
#if defined(__has_attribute)
#if __has_attribute(preserve_none) && defined(__x86_64__)
#define DOLRECOMP_CHUNK_FN __attribute__((preserve_none))
#endif
#endif
#ifndef DOLRECOMP_CHUNK_FN
#define DOLRECOMP_CHUNK_FN
#endif

/* A twin chunk's fast copy hands any entry PC not on its hot list to the cold
 * copy (backend/twin.c). DOLRECOMP_TWIN_MISS_LOG makes that hand-off count the
 * PC, so `setup.sh --pgo` can see traffic the shipped hot list misses and add
 * it -- a list recorded before a codegen change drifts silently (the chunk
 * overhang sent 7-8% of entries to the cold copies). Nothing otherwise: the
 * shipped module compiles exactly as before. */
#if defined(DOLRECOMP_TWIN_MISS_LOG)
void dolrecomp_twin_miss(u32 pc);
#define DOLRECOMP_TWIN_MISS(pc) dolrecomp_twin_miss(pc)
#else
#define DOLRECOMP_TWIN_MISS(pc) ((void)0)
#endif

// New-ABI CPUState (spr[1024] + mem2, no external_pointer). Kept in sync with
// the chassis runtime header (GXRuntime core/cpu.h) for the module ABI check.
#define GXRUNTIME_CPU_ABI_VERSION 4u

#define GC_MAIN_RAM_SIZE    (24 * 1024 * 1024)
#define GC_RAM_BASE         0x80000000u
#define GC_RAM_UNCACHED     0xC0000000u

#define WII_MEM2_SIZE       (64 * 1024 * 1024)
#define WII_MEM2_BASE       0x90000000u
#define WII_MEM2_UNCACHED   0xD0000000u

#define PPC_EXC_PROGRAM       0x00000001u
#define PPC_EXC_DSI           0x00000002u
#define PPC_EXC_ALIGNMENT     0x00000004u
#define PPC_EXC_SYSTEM_CALL   0x00000008u
#define PPC_EXC_MACHINE_CHECK 0x00000010u
#define PPC_EXC_FP_UNAVAILABLE 0x00000020u

#define PPC_PROGRAM_FP        0x00100000u
#define PPC_PROGRAM_ILLEGAL   0x00080000u
#define PPC_PROGRAM_PRIV      0x00040000u
#define PPC_PROGRAM_TRAP      0x00020000u

#define PPC_DSI_EAR_DISABLED  0x00100000u

#define PPC_VECTOR_MACHINE_CHECK 0x00200u
#define PPC_VECTOR_DSI           0x00300u
#define PPC_VECTOR_ALIGNMENT     0x00600u
#define PPC_VECTOR_PROGRAM       0x00700u
#define PPC_VECTOR_FP_UNAVAILABLE 0x00800u
#define PPC_VECTOR_SYSTEM_CALL   0x00C00u

#define PPC_HID2_LSQE   0x80000000u
#define PPC_HID2_PSE    0x20000000u
#define PPC_HID2_LCE    0x10000000u
#define PPC_HID2_DCHERR 0x00800000u
#define PPC_HID2_DCHEE  0x00080000u

#define PPC_GEKKO_PVR 0x00083214u

typedef struct CPUState CPUState;
typedef u64 (*PPCExternalRead)(CPUState* cpu, u32 ea, u8 size);
typedef void (*PPCExternalWrite)(CPUState* cpu, u32 ea, u64 value, u8 size);
typedef u32 (*PPCExternalRead32)(CPUState* cpu, u32 ea, u8 rid);
typedef void (*PPCExternalWrite32)(CPUState* cpu, u32 ea, u32 value, u8 rid);
typedef void (*PPCInstructionFallback)(CPUState* cpu, u32 raw, u32 cia);
typedef bool (*PPCHostCall)(CPUState* cpu, u32 address);

struct CPUState {
    u32 gpr[32];
    f64 fpr[32];
    f64 ps1[32];
    u32 pc;
    u32 lr;
    u32 ctr;
    u8 crf[8];   /* unpacked CR, see cpu_cr_get */
    u32 xer;
    u32 fpscr;
    u32 msr;
    u32 srr0;
    u32 srr1;
    u32 dar;
    u32 dsisr;
    u32 ear;
    u32 hid2;
    u64 timebase;
    u32 sr[16];
    u32 gqr[8];
    u32 spr[1024];
    u32 exception;
    u32 program_exception;
    u32 tlb_last_vps;
    u32 tlb_last_index;
    u32 tlb_invalidate_count;
    u32 external_addr;
    u32 external_value;
    u8 external_rid;
    u8 external_read_count;
    u8 external_write_count;
    u32 reserve_addr;
    bool reserve_valid;
    u32 locked_cache_tag[512];
    bool locked_cache_valid[512];
    PPCExternalRead external_read;
    PPCExternalWrite external_write;
    PPCExternalRead32 external_read32;
    PPCExternalWrite32 external_write32;
    PPCInstructionFallback instruction_fallback;
    PPCHostCall host_call;
    void* external_user_data;

    u8* ram;
    u32 ram_size;
    u8* mem2;
    u32 mem2_size;
    s64 downcount;
};

/* The CR is stored UNPACKED, one byte per field (crf[0] is CR0, the top
 * nibble of the architectural register), each holding LT/GT/EQ/SO as
 * 8/4/2/1. A field write is then one byte store instead of a
 * read-modify-write of a packed word -- the packed form cost 8.75% of the
 * gameplay profile. Only mfcr/mtcrf and the chassis sync need the packed
 * value, and they go through these two. ABI v4. */
static inline uint32_t cpu_cr_get(const CPUState* cpu) {
    uint32_t cr = 0;
    for (unsigned i = 0; i < 8; ++i)
        cr |= (uint32_t)(cpu->crf[i] & 0xFu) << (28u - 4u * i);
    return cr;
}

static inline void cpu_cr_set(CPUState* cpu, uint32_t cr) {
    for (unsigned i = 0; i < 8; ++i)
        cpu->crf[i] = (uint8_t)((cr >> (28u - 4u * i)) & 0xFu);
}

bool cpu_init(CPUState* cpu);
bool cpu_alloc_mem2(CPUState* cpu, u32 size); //mem 2 only exists after first aloc
void cpu_free(CPUState* cpu);
void cpu_reset(CPUState* cpu);

// Slow-path memory access (MMIO / external / unmapped). The fast inline
// wrappers at the bottom of this header service the common cached/uncached
// MEM1+MEM2 hit inline, so the generated code never pays a call for RAM.
u64  mem_read64_slow(CPUState* cpu, u32 addr);
void mem_write64_slow(CPUState* cpu, u32 addr, u64 value);
u32  mem_read32_slow(CPUState* cpu, u32 addr);
void mem_write32_slow(CPUState* cpu, u32 addr, u32 value);
u16  mem_read16_slow(CPUState* cpu, u32 addr);
void mem_write16_slow(CPUState* cpu, u32 addr, u16 value);
u8   mem_read8_slow(CPUState* cpu, u32 addr);
void mem_write8_slow(CPUState* cpu, u32 addr, u8 value);

// Memory write journal (lockstep pre-image capture). Declared here so the
// inline write fast path can reference the globals and fold the null check.
typedef void (*PPCMemWriteJournal)(u32 offset, u32 size, void* user);
extern PPCMemWriteJournal g_mem_write_journal;
extern void* g_mem_write_journal_user;
void ppc_set_mem_write_journal(PPCMemWriteJournal fn, void* user);

f64 ppc_approx_reciprocal(f64 value);
f64 ppc_approx_rsqrt(f64 value);
bool ppc_fres(CPUState* cpu, f64 value, f64* result);
bool ppc_frsqrte(CPUState* cpu, f64 value, f64* result);
void ppc_ps_res(CPUState* cpu, f64 a, f64 b, f64* result_a, f64* result_b);
void ppc_ps_rsqrte(CPUState* cpu, f64 a, f64 b, f64* result_a, f64* result_b);
bool ppc_fma(CPUState* cpu, f64 a, f64 c, f64 b, bool single,
             bool subtract, bool negative, f64* output);
bool ppc_fctiw(CPUState* cpu, f64 value, bool toward_zero, u64* result);
bool ppc_add_overflowed(u32 a, u32 b, u32 result);
bool ppc_trap_condition(u8 to, u32 a, u32 b);
void ppc_set_xer_ov(CPUState* cpu, bool ov);
void ppc_take_exception(CPUState* cpu, u32 exception, u32 vector, u32 srr0, u32 srr1_info);
void ppc_program_exception(CPUState* cpu, u32 cause, u32 cia);
void ppc_fallback_instruction(CPUState* cpu, u32 raw, u32 cia);
bool ppc_host_call(CPUState* cpu, u32 address);
void ppc_system_call_exception(CPUState* cpu, u32 cia);
void ppc_dsi_exception(CPUState* cpu, u32 ea, u32 cia, u32 dsisr);
void ppc_alignment_exception(CPUState* cpu, u32 ea, u32 cia);
u32 ppc_mftb(CPUState* cpu, u16 tbr, u32 cia);
u32 ppc_mfspr(CPUState* cpu, u16 spr, u32 cia);
void ppc_mtspr(CPUState* cpu, u16 spr, u32 value, u32 cia);
void ppc_rfi(CPUState* cpu, u32 cia);
void ppc_dcbz_l(CPUState* cpu, u32 ea, u32 cia);
// Returns false when the access raised an exception, true otherwise. The C
// backend calls these as statements and ignores it; the LLVM backend BRANCHES on
// the result, and while these returned void it branched on a garbage register,
// took the failure path and returned without advancing pc -- an infinite
// re-dispatch of the same address.
bool ppc_psq_load(CPUState* cpu, u8 frD, u32 ea, bool w, u8 gqr, bool indexed, u32 cia);
bool ppc_psq_load_full(CPUState* cpu, u8 frD, u32 ea, bool w, u8 gqr, bool indexed, u32 cia);
bool ppc_psq_store(CPUState* cpu, u8 frS, u32 ea, bool w, u8 gqr, bool indexed, u32 cia);
bool ppc_psq_store_full(CPUState* cpu, u8 frS, u32 ea, bool w, u8 gqr, bool indexed, u32 cia);
u32 ppc_eciwx(CPUState* cpu, u32 ea, u32 cia);
void ppc_ecowx(CPUState* cpu, u32 ea, u32 value, u32 cia);
void ppc_tlbie(CPUState* cpu, u32 ea, u32 cia);
void ppc_fpscr_updated(CPUState* cpu);
void ppc_memory_fence(void);

// ---------------------------------------------------------------------------
// Hot inline fast paths. These were previously out-of-line functions in cpu.c,
// so every generated load/store/FP-guard emitted a real call (88k mem_read32,
// 87k ppc_fp_available, 70k mem_write32 sites) — the dominant CPU cost during
// heavy workloads (FMV decode). Inlining the common cases here collapses them
// to a couple of compares + a byteswap; only genuine misses call the slow path.

// Plain static inline. Measured: force-inlining the mem wrappers (always_inline)
// only bought ~2% (50%→52% FMV) while ballooning the module 20MB→36MB, so it is
// not worth it — the big win was inlining ppc_fp_available (the FP guard), which
// this FP-heavy path hit on every op. clang keeps the branchier mem wrappers as
// direct local calls (still far cheaper than the former PLT round-trip).
#define DOLRECOMP_AI static inline

// Cached-MEM1 host pointer for [addr, addr+size) or NULL. This is the
// overwhelmingly common access (0x80xxxxxx); uncached (0xC0…), MEM2 and MMIO
// fall through to the *_slow path, which resolve_addr() handles identically.
// Kept to a single compare so the wrappers below inline naturally at -O2 (no
// always_inline needed → no compile-time / code-size blowup at 80k+ sites).
// Unsigned wrap makes any out-of-region address exceed the bound and fail.
//
// The hints below lay out the common case as the fall-through: the cached-RAM
// hit, no write journal (only a watch or lockstep arms it), no reservation.
// Measured together with the read/write_be* change in common/types.h, no-PGO
// US module, 14000-frame VS fight, 3 alternating reps: -2.74% cycles (the bound
// compare alone was profiled at 2.11 M cycles/frame), frame hashes identical.
#if defined(__GNUC__) || defined(__clang__)
#define DOLRECOMP_LIKELY(x)   __builtin_expect(!!(x), 1)
#define DOLRECOMP_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define DOLRECOMP_LIKELY(x)   (x)
#define DOLRECOMP_UNLIKELY(x) (x)
#endif

/* DOLRECOMP_RAM_LOCAL_ENABLE (module-src MODULE_RAM_LOCAL): cache cpu->ram in a
 * chunk-entry local. A store through cpu->ram may alias the CPUState fields, so
 * clang cannot keep the pointer in a register and reloads it on EVERY access --
 * 999 reloads against ~999 accesses in the hottest chunk, measured. A local is
 * an SSA value, not memory, so the reload disappears; unlike a literal bound it
 * gives clang nothing to range-reason about, which is what killed the constant
 * base attempt. Sound because the chassis binds m_guest.ram once at Run() entry
 * and never during a dispatch. The generated chunk prologue declares it
 * unconditionally; with the flag off the macro is a no-op and accesses read
 * cpu->ram as before, so ONE generated tree serves both arms of an A/B. */
#if defined(DOLRECOMP_RAM_LOCAL_ENABLE)
#define DOLRECOMP_RAM_LOCAL(cpu) u8 *const dolrecomp_ram = (cpu)->ram
#define DOLRECOMP_RAM_ARG(cpu)   dolrecomp_ram
#else
#define DOLRECOMP_RAM_LOCAL(cpu) ((void)0)
#define DOLRECOMP_RAM_ARG(cpu)   ((cpu)->ram)
#endif
/* DOLRECOMP_DC_LOCAL_ENABLE (module-src MODULE_DC_LOCAL): keep the cycle
 * charge in a chunk local instead of ctx->downcount. Every block leader does
 * `downcount -= N`, and since nearly every leader is also an entry-switch case
 * clang must reload the field from memory there, so each block's charge waits
 * on the previous block's store -- a store-forwarding chain through the whole
 * chunk (4.15% of samples in the 1.6.3 profile). A local is SSA, so the joins
 * become phis and the chain stays in a register.
 *
 * ctx->downcount is a charge accumulator that only three things read: the
 * chassis after a dispatch returns, HookInstructionFallback (SyncOut reads it,
 * SyncIn zeroes it), and a directly called chunk. So the local is written back
 * at every return (DOLRECOMP_RETURN) and around those two calls, and reloaded
 * after them. MMIO hooks never touch it. With the option off every macro
 * expands to the original ctx->downcount / return, byte for byte. */
#if defined(DOLRECOMP_DC_LOCAL_ENABLE)
#define DOLRECOMP_DC_LOCAL(cpu)  s64 dolrecomp_dc = (cpu)->downcount
#define DOLRECOMP_DC             dolrecomp_dc
#define DOLRECOMP_DC_FLUSH(cpu)  ((cpu)->downcount = dolrecomp_dc)
#define DOLRECOMP_DC_RELOAD(cpu) (dolrecomp_dc = (cpu)->downcount)
#define DOLRECOMP_RETURN         do { ctx->downcount = dolrecomp_dc; return; } while (0)
#else
#define DOLRECOMP_DC_LOCAL(cpu)  ((void)0)
#define DOLRECOMP_DC             ctx->downcount
#define DOLRECOMP_DC_FLUSH(cpu)  ((void)0)
#define DOLRECOMP_DC_RELOAD(cpu) ((void)0)
#define DOLRECOMP_RETURN         return
#endif
DOLRECOMP_AI u8* dolrecomp_cached_ram_r(const CPUState* cpu, u8* ram, u32 addr, u32 size) {
    u32 off = addr - GC_RAM_BASE;
    (void)cpu;
    return DOLRECOMP_LIKELY(off <= GC_MAIN_RAM_SIZE - size) ? ram + off : (u8*)0;
}
#define dolrecomp_cached_ram(cpu, addr, size) \
    dolrecomp_cached_ram_r((cpu), DOLRECOMP_RAM_ARG(cpu), (addr), (size))

// host is always inside cpu->ram here (cached MEM1), so the journal offset is
// direct and needs no range check.
/* DOLRECOMP_MEM_FAST (module-src MODULE_MEM_FAST) strips what a shipped module
 * never needs from every inlined RAM access:
 *   - the write-journal test: only lockstep and RINGOUT_DETERMINISM_WATCH arm
 *     the journal, and ppc_set_mem_write_journal says so when it is compiled out;
 *   - the reservation test, only when the recompiler proved the DOL has no
 *     lwarx/stwcx (DOLRECOMP_DOL_NO_RESERVATION in generated.h): nothing else
 *     ever sets reserve_valid, so the test can never be true;
 *   - the NULL test on the host pointer (see the accessors below). */
DOLRECOMP_AI void dolrecomp_journal_cached(CPUState* cpu, const u8* host, u32 size) {
#if defined(DOLRECOMP_MEM_FAST)
    (void)cpu; (void)host; (void)size;
#else
    if (DOLRECOMP_UNLIKELY(g_mem_write_journal))
        g_mem_write_journal((u32)(host - cpu->ram), size, g_mem_write_journal_user);
#endif
}
DOLRECOMP_AI void dolrecomp_clear_reservation(CPUState* cpu, u32 addr) {
#if defined(DOLRECOMP_MEM_FAST) && defined(DOLRECOMP_DOL_NO_RESERVATION)
    (void)cpu; (void)addr;
#else
    if (DOLRECOMP_UNLIKELY(cpu->reserve_valid) && ((cpu->reserve_addr ^ addr) & ~31u) == 0)
        cpu->reserve_valid = false;
#endif
}

#if defined(DOLRECOMP_MEM_FAST)
/* Branch on the range itself. The generic accessors test the pointer returned
 * by dolrecomp_cached_ram, and since ram + off could in principle be NULL the
 * compiler keeps a `test ram, ram` on every access even after the range test. */
#define DOLRECOMP_MEM_FAST_RW(bits, rd, wr)                                          \
    DOLRECOMP_AI u##bits dolrecomp_mem_read##bits##_r(CPUState* cpu, u8* ram,        \
                                                      u32 addr) {                    \
        u32 off = addr - GC_RAM_BASE;                                                \
        return DOLRECOMP_LIKELY(off <= GC_MAIN_RAM_SIZE - (bits / 8u))               \
            ? rd(ram + off) : mem_read##bits##_slow(cpu, addr);                      \
    }                                                                                \
    DOLRECOMP_AI void dolrecomp_mem_write##bits##_r(CPUState* cpu, u8* ram,          \
                                                    u32 addr, u##bits value) {       \
        u32 off = addr - GC_RAM_BASE;                                                \
        if (DOLRECOMP_LIKELY(off <= GC_MAIN_RAM_SIZE - (bits / 8u))) {               \
            dolrecomp_clear_reservation(cpu, addr);                                  \
            wr(ram + off, value);                                                    \
        } else {                                                                     \
            mem_write##bits##_slow(cpu, addr, value);                                \
        }                                                                            \
    }
static inline u8 dolrecomp_rd8(const u8* h) { return *h; }
static inline void dolrecomp_wr8(u8* h, u8 v) { *h = v; }
DOLRECOMP_MEM_FAST_RW(8, dolrecomp_rd8, dolrecomp_wr8)
DOLRECOMP_MEM_FAST_RW(16, read_be16, write_be16)
DOLRECOMP_MEM_FAST_RW(32, read_be32, write_be32)
DOLRECOMP_MEM_FAST_RW(64, read_be64, write_be64)
#undef DOLRECOMP_MEM_FAST_RW
/* RAM-base sites (--ram-bases). A load or store whose base register is the
 * stack pointer or a small-data base can only address main RAM, so it needs
 * neither the range test nor the slow path: one host load at a biased pointer.
 * The emitter marks such a site by opening its block with
 * `enum { dr_ram_site = 1 };`, which shadows the file-scope 0 below.
 * __builtin_choose_expr resolves at parse time, so an unmarked site compiles to
 * exactly what it did before -- no branch, and no new region for PGO to count,
 * which matters: a `?:` here would change every chunk's CFG hash and void the
 * shipped profiles.
 *
 * DOLRECOMP_RAM_SITE_CHECK builds a census variant: out-of-range addresses at a
 * marked site are counted and take the checked path, so a run proves (or
 * refutes) the "always RAM" premise before the unchecked form is trusted. */
enum { dr_ram_site = 0 };
#if defined(DOLRECOMP_RAM_SITE_CHECK)
extern unsigned long long g_dr_ram_site_misses;
#define DOLRECOMP_RAM_SITE_OK(addr, n) \
    (((u32)(addr) - GC_RAM_BASE) <= GC_MAIN_RAM_SIZE - (n) || (++g_dr_ram_site_misses, 0))
#define DOLRECOMP_RAM_SITE_RW(bits, rd, wr)                                          \
    DOLRECOMP_AI u##bits dolrecomp_ram_read##bits(CPUState* cpu, u8* ram, u32 addr) { \
        return DOLRECOMP_RAM_SITE_OK(addr, bits / 8u) ? rd(ram + (addr - GC_RAM_BASE)) \
                                                      : mem_read##bits##_slow(cpu, addr); \
    }                                                                                \
    DOLRECOMP_AI void dolrecomp_ram_write##bits(CPUState* cpu, u8* ram, u32 addr,     \
                                                u##bits value) {                     \
        if (DOLRECOMP_RAM_SITE_OK(addr, bits / 8u)) {                                \
            dolrecomp_clear_reservation(cpu, addr);                                  \
            wr(ram + (addr - GC_RAM_BASE), value);                                   \
        } else {                                                                     \
            mem_write##bits##_slow(cpu, addr, value);                                \
        }                                                                            \
    }
#else
/* The biased base is loop-invariant, so the compiler hoists it and the access
 * becomes a single [base + addr] load or store. */
#define DOLRECOMP_RAM_SITE_RW(bits, rd, wr)                                          \
    DOLRECOMP_AI u##bits dolrecomp_ram_read##bits(CPUState* cpu, u8* ram, u32 addr) { \
        (void)cpu;                                                                   \
        return rd((u8*)((uintptr_t)ram - GC_RAM_BASE + addr));                       \
    }                                                                                \
    DOLRECOMP_AI void dolrecomp_ram_write##bits(CPUState* cpu, u8* ram, u32 addr,     \
                                                u##bits value) {                     \
        dolrecomp_clear_reservation(cpu, addr);                                      \
        wr((u8*)((uintptr_t)ram - GC_RAM_BASE + addr), value);                       \
    }
#endif
DOLRECOMP_RAM_SITE_RW(8, dolrecomp_rd8, dolrecomp_wr8)
DOLRECOMP_RAM_SITE_RW(16, read_be16, write_be16)
DOLRECOMP_RAM_SITE_RW(32, read_be32, write_be32)
DOLRECOMP_RAM_SITE_RW(64, read_be64, write_be64)
#undef DOLRECOMP_RAM_SITE_RW
/* Marked LOADS keep the checked path unless DOLRECOMP_RAM_SITE_LOADS_TOO. On
 * Zen 2 an unchecked load issues before an older store to the same slot has
 * resolved: store-to-load interlocks rose 34% and the Deck lost 1.13% cycles
 * although instructions fell 2.2% (the desktop gained 1.83%). Stores produce
 * the data, so they cannot cause it: stores-only measured -0.61% on the Deck. */
#if defined(DOLRECOMP_RAM_SITE_LOADS_TOO)
#define DOLRECOMP_RAM_SITE_LOADS dr_ram_site
#else
#define DOLRECOMP_RAM_SITE_LOADS 0
#endif
#define DOLRECOMP_MEM_PICK(bits, cpu, addr)                                          \
    __builtin_choose_expr(DOLRECOMP_RAM_SITE_LOADS,                                  \
        dolrecomp_ram_read##bits((cpu), DOLRECOMP_RAM_ARG(cpu), (addr)),             \
        dolrecomp_mem_read##bits##_r((cpu), DOLRECOMP_RAM_ARG(cpu), (addr)))
#define DOLRECOMP_MEM_PICKW(bits, cpu, addr, v)                                      \
    __builtin_choose_expr(dr_ram_site,                                               \
        dolrecomp_ram_write##bits((cpu), DOLRECOMP_RAM_ARG(cpu), (addr), (v)),       \
        dolrecomp_mem_write##bits##_r((cpu), DOLRECOMP_RAM_ARG(cpu), (addr), (v)))
#define mem_read8(cpu, addr)         DOLRECOMP_MEM_PICK(8, cpu, addr)
#define mem_read16(cpu, addr)        DOLRECOMP_MEM_PICK(16, cpu, addr)
#define mem_read32(cpu, addr)        DOLRECOMP_MEM_PICK(32, cpu, addr)
#define mem_read64(cpu, addr)        DOLRECOMP_MEM_PICK(64, cpu, addr)
#define mem_write8(cpu, addr, v)     DOLRECOMP_MEM_PICKW(8, cpu, addr, v)
#define mem_write16(cpu, addr, v)    DOLRECOMP_MEM_PICKW(16, cpu, addr, v)
#define mem_write32(cpu, addr, v)    DOLRECOMP_MEM_PICKW(32, cpu, addr, v)
#define mem_write64(cpu, addr, v)    DOLRECOMP_MEM_PICKW(64, cpu, addr, v)
#else
DOLRECOMP_AI u8 mem_read8(CPUState* cpu, u32 addr) {
    u8* h = dolrecomp_cached_ram(cpu, addr, 1u);
    return h ? *h : mem_read8_slow(cpu, addr);
}
DOLRECOMP_AI u16 mem_read16(CPUState* cpu, u32 addr) {
    u8* h = dolrecomp_cached_ram(cpu, addr, 2u);
    return h ? read_be16(h) : mem_read16_slow(cpu, addr);
}
DOLRECOMP_AI u32 mem_read32(CPUState* cpu, u32 addr) {
    u8* h = dolrecomp_cached_ram(cpu, addr, 4u);
    return h ? read_be32(h) : mem_read32_slow(cpu, addr);
}
DOLRECOMP_AI u64 mem_read64(CPUState* cpu, u32 addr) {
    u8* h = dolrecomp_cached_ram(cpu, addr, 8u);
    return h ? read_be64(h) : mem_read64_slow(cpu, addr);
}

DOLRECOMP_AI void mem_write8(CPUState* cpu, u32 addr, u8 value) {
    u8* h = dolrecomp_cached_ram(cpu, addr, 1u);
    if (h) { dolrecomp_clear_reservation(cpu, addr); dolrecomp_journal_cached(cpu, h, 1u); *h = value; }
    else mem_write8_slow(cpu, addr, value);
}
DOLRECOMP_AI void mem_write16(CPUState* cpu, u32 addr, u16 value) {
    u8* h = dolrecomp_cached_ram(cpu, addr, 2u);
    if (h) { dolrecomp_clear_reservation(cpu, addr); dolrecomp_journal_cached(cpu, h, 2u); write_be16(h, value); }
    else mem_write16_slow(cpu, addr, value);
}
DOLRECOMP_AI void mem_write32(CPUState* cpu, u32 addr, u32 value) {
    u8* h = dolrecomp_cached_ram(cpu, addr, 4u);
    if (h) { dolrecomp_clear_reservation(cpu, addr); dolrecomp_journal_cached(cpu, h, 4u); write_be32(h, value); }
    else mem_write32_slow(cpu, addr, value);
}
DOLRECOMP_AI void mem_write64(CPUState* cpu, u32 addr, u64 value) {
    u8* h = dolrecomp_cached_ram(cpu, addr, 8u);
    if (h) { dolrecomp_clear_reservation(cpu, addr); dolrecomp_journal_cached(cpu, h, 8u); write_be64(h, value); }
    else mem_write64_slow(cpu, addr, value);
}
#endif

/* LAZY FPRF.
 *
 * FPRF (FPSCR bits 12-16) classifies every FP result. Measured on gameplay it
 * is written 3.073 G times and read 15,885 -- 100% dead -- and costs ~20 host
 * instructions a time, 3.1% of all cycles. So do not compute it eagerly:
 * remember the value, and classify only when someone can actually see FPSCR.
 *
 * The pending value lives in module globals rather than CPUState on purpose.
 * A CPUState field would be the tidier home, but the chassis exports guest
 * state to Dolphin through SyncOut() -- the path savestates, rollback and
 * netplay all run on -- and hooking that needs a new module export and an ABI
 * bump, which couples module and runtime deployment. Instead this flushes at
 * every point control LEAVES the module, so the chassis can never observe a
 * stale FPSCR and none of that machinery has to change:
 *
 *   chassis_dispatch        (module_export.c) every return to the run loop
 *   ppc_fallback_instruction  before the interpreter sees the state
 *   ppc_host_call             before any HLE hook runs
 *   mffs / mcrfs              the guest reading it itself
 *
 * and it is DROPPED (not flushed) wherever the guest writes FPSCR explicitly
 * (mtfsf/mtfsfi/mtfsb0/mtfsb1), because that write supersedes the pending
 * classification of an earlier op.
 *
 * Correctness is exactly checkable: guest state must stay bit-identical, so
 * the gameplay benchmark's frame hash must be unchanged.
 */
/* RECOMP_FPRF_TRACE: name the instruction that last wrote FPRF before each
 * mffs, so an eager and a lazy build can be diffed to find which op the two
 * disagree about. Off by default and expanding to nothing. */
#ifdef RECOMP_FPRF_TRACE
extern u32 g_fprf_pc;
void ppc_fprf_trace_mffs(CPUState* cpu);
#define PPC_FPRF_TAG(addr)      (g_fprf_pc = (u32)(addr))
#define PPC_FPRF_TRACE_MFFS(c)  ppc_fprf_trace_mffs(c)
#else
#define PPC_FPRF_TAG(addr)      ((void)0)
#define PPC_FPRF_TRACE_MFFS(c)  ((void)0)
#endif

extern f64 g_fprf_value;
extern u8  g_fprf_kind;   /* 0 none, 1 single, 2 double */
void ppc_fprf_materialize(CPUState* cpu);

static inline void ppc_fprf_flush(CPUState* cpu) {
    if (g_fprf_kind) ppc_fprf_materialize(cpu);
}
static inline void ppc_fprf_drop(void) { g_fprf_kind = 0u; }

/* Condition-register liveness instrumentation.
 *
 * Every `.`-form instruction and every compare eagerly computes a CR field --
 * about six host instructions each, into a store to ctx->crf[]. If
 * most of those fields are overwritten before anything reads them, that is
 * pure emitted waste and eliding it is a codegen win. Nothing has ever
 * measured which it is, and in this codebase static structure has predicted
 * execution badly every single time (static psq sites said 92.8% GQR0; the
 * measured truth for stores was 15.3%), so this counts at runtime.
 *
 * OFF BY DEFAULT AND EXPANDING TO NOTHING, so one generation serves both the
 * shipped module and the probe: build with -DRECOMP_CR_STATS to arm it.
 */
#ifdef RECOMP_CR_STATS
void ppc_cr_note_write(unsigned field);
void ppc_cr_note_read(unsigned field_mask);
void ppc_ca_note_write(void);
void ppc_ca_note_read(void);
void ppc_fprf_note_write(void);
void ppc_fprf_note_read(void);
void ppc_ps_note_lane(void);
#define PPC_CR_WRITE(field) ppc_cr_note_write((field))
#define PPC_CR_READ(mask)   ppc_cr_note_read((mask))
#define PPC_CA_WRITE()      ppc_ca_note_write()
#define PPC_CA_READ()       ppc_ca_note_read()
#define PPC_FPRF_WRITE()    ppc_fprf_note_write()
#define PPC_FPRF_READ()     ppc_fprf_note_read()
#define PPC_PS_LANE()       ppc_ps_note_lane()
#else
#define PPC_CR_WRITE(field) ((void)0)
#define PPC_CR_READ(mask)   ((void)0)
#define PPC_CA_WRITE()      ((void)0)
#define PPC_CA_READ()       ((void)0)
#define PPC_FPRF_WRITE()    ((void)0)
#define PPC_FPRF_READ()     ((void)0)
#define PPC_PS_LANE()       ((void)0)
#endif

// MSR.FP (PPC_BIT(18) = 0x2000). Common case (FP enabled) is a single bit test.
static inline bool ppc_fp_available(CPUState* cpu, u32 cia) {
    if (cpu->msr & 0x00002000u)
        return true;
    ppc_take_exception(cpu, PPC_EXC_FP_UNAVAILABLE, PPC_VECTOR_FP_UNAVAILABLE, cia, 0);
    return false;
}


/* Double -> single for guest STORES, done by the hardware but OPAQUE to the
 * compiler. A plain (f32)x is a cvtsd2ss that clang may fold away entirely --
 * e.g. an lfs->stfs round trip becomes a bit copy -- and whether it does
 * depends on the surrounding control flow. The game runs with FTZ on
 * (MXCSR 0x9F80 on ~all chunk entries), so an executed conversion flushes a
 * denormal to zero and a folded one does not: an unrelated CFG change moved the
 * frame hash (0x80CE6A4C at frame 4367, found by pruning entry cases). Inline
 * asm always executes the instruction, so every build converts the same way.
 * This keeps the existing semantics (round, FTZ flush); matching Dolphin's
 * truncating ConvertToSingle exactly measured +1.64% cycles on the Deck. */
static inline f32 dolrecomp_cvt_single(f64 v) {
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
    f32 r;
#if defined(__AVX__)
    /* VEX form, as the compiler would emit it: a legacy-SSE write merges the
     * register's upper half and stalls behind whatever last wrote it. The
     * legacy form made JP's twin build 5.8% SLOWER on the Deck. */
    __asm__("vcvtsd2ss %1, %1, %0" : "=x"(r) : "x"(v));
#else
    __asm__("cvtsd2ss %1, %0" : "=x"(r) : "x"(v));
#endif
    return r;
#else
    return (f32)v;
#endif
}
static inline u32 dolrecomp_store_single(f64 v) {
    f32 r = dolrecomp_cvt_single(v);
    u32 b; memcpy(&b, &r, sizeof(b)); return b;
}

/* Paired-single quantised load/store: a SLIM inline fast path per call site.
 *
 * Every psq_l/psq_st in a chunk used to be a 7-argument out-of-line call (the
 * 1.6.1 profile charged ppc_psq_store alone 4.34% of the CPU thread). Inlining
 * the WHOLE function removed 7.3% of instructions but grew .text 25% and cost
 * +5.46% cycles. So only the common case is inlined here: quantisation type 0
 * (plain f32), psq enabled, address aligned. Anything else -- the integer
 * types, a disabled HID2 bit, a misaligned address that must raise the
 * alignment exception -- calls the unchanged full implementation, which
 * re-checks everything, so behaviour is identical by construction.
 *
 * Type 0 facts from ppc_psq_*_full: size 4, so lane 1 is at ea+4 and is aligned
 * whenever ea is; a load is (f64)f32; a store writes 0 for a denormal single.
 *
 * Both lanes are bounds-checked ONCE, against the whole 8-byte span, instead of
 * calling mem_read32/mem_write32 per lane and checking each. Type 0 puts lane 1
 * at ea+4 and the gate already requires ea aligned, so one `off <= ram_size - 8`
 * covers both; anything that fails it falls back to the per-lane accessors and
 * is handled exactly as before. Worth -1.2 to -1.4% instructions on all four
 * discs, and cycles: US -0.66%, JP -0.67%, PAL -0.74% (n=8, trimmed), Plus
 * -1.05%, over 16000 frames of arcade-match with frame hashes identical.
 *
 * The 8-byte bound is deliberately conservative for the w (single-lane) case,
 * which only needs 4: that lands in the fallback and stays correct.
 *
 * NOT taken from the same source: masking bit 30 off the address so 0xC0xxxxxx
 * uncached accesses hit the inline path too. Measured on its own it is +3.52%
 * cycles and +5.79% instructions -- an extra AND on all 80k+ memory sites to
 * catch a case this game almost never issues.
 *
 * OPT-IN (DOLRECOMP_PSQ_FAST, module CMake option MODULE_PSQ_FAST). It changes
 * every chunk's control flow before inlining, so a PGO profile trained without
 * it stops matching -- measured: clang drops the counts on a Japanese chunk.
 * setup turns it on only for a disc whose shipped profile was trained with it. */
#if defined(DOLRECOMP_PSQ_FAST) && !defined(DOLRECOMP_PSQ_OUT_OF_LINE)
/* always_inline, not plain inline: a PGO profile keys an INTERNAL-linkage
 * function as "<source path>;<name>". Plain inline let clang keep an out-of-line
 * copy of dolrecomp_psq_store_inline in ~100 chunks, so the trained profile
 * embedded the build machine's directory layout and those counts could never
 * match on a player's machine, where the path differs. */
#if defined(__GNUC__) || defined(__clang__)
#define DOLRECOMP_PSQ_AI static inline __attribute__((always_inline))
#else
#define DOLRECOMP_PSQ_AI static inline
#endif
DOLRECOMP_PSQ_AI bool dolrecomp_psq_type0_ok(const CPUState* cpu, u32 gqr_type_bits, u32 ea, bool indexed) {
    return gqr_type_bits == 0u && (ea & 3u) == 0u &&
           (cpu->hid2 & PPC_HID2_PSE) != 0u && (indexed || (cpu->hid2 & PPC_HID2_LSQE) != 0u);
}

DOLRECOMP_PSQ_AI u32 dolrecomp_psq_single_bits(f64 value) {
    f32 single = dolrecomp_cvt_single(value);
    u32 bits;
    memcpy(&bits, &single, sizeof(bits));
    return ((bits & 0x7F800000u) == 0u && (bits & 0x007FFFFFu) != 0u) ? 0u : bits;
}

DOLRECOMP_PSQ_AI f64 dolrecomp_psq_single_value(u32 bits) {
    f32 single;
    memcpy(&single, &bits, sizeof(single));
    return (f64)single;
}

/* DOLRECOMP_PSQ_SIMD (module-src MODULE_PSQ_SIMD): move BOTH lanes of a
 * type-0 paired-single load/store through one SSE register instead of two
 * scalar conversions. Bit-identical by construction: cvtpd2ps/cvtps2pd round
 * each lane exactly as cvtsd2ss/cvtss2sd do under the same MXCSR, and the
 * denormal flush is the same predicate as dolrecomp_psq_single_bits, applied
 * with vector masks: a lane whose exponent is 0 and fraction non-zero becomes
 * +0.0f (0u), everything else, +/-0 included, passes through. The profile put
 * that per-lane flush line alone at 2.1% of all samples. */
#if defined(DOLRECOMP_PSQ_SIMD) && defined(__SSSE3__)
#include <immintrin.h>
#define DOLRECOMP_PSQ_SIMD_ON 1
DOLRECOMP_PSQ_AI void dolrecomp_psq_store2(u8* h, f64 lane0, f64 lane1) {
    /* opaque cvtpd2ps, for the same reason as dolrecomp_cvt_single */
    __m128 f;
#if defined(__AVX__)
    __asm__("vcvtpd2psx %1, %0" : "=x"(f) : "x"(_mm_set_pd(lane1, lane0)));
#else
    __asm__("cvtpd2ps %1, %0" : "=x"(f) : "x"(_mm_set_pd(lane1, lane0)));
#endif
    __m128i s = _mm_castps_si128(f);
    const __m128i zero = _mm_setzero_si128();
    __m128i expz  = _mm_cmpeq_epi32(_mm_and_si128(s, _mm_set1_epi32(0x7F800000)), zero);
    __m128i fracz = _mm_cmpeq_epi32(_mm_and_si128(s, _mm_set1_epi32(0x007FFFFF)), zero);
    s = _mm_andnot_si128(_mm_andnot_si128(fracz, expz), s);
    s = _mm_shuffle_epi8(s, _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 8, 9, 10, 11, 12, 13, 14, 15));
    _mm_storel_epi64((__m128i*)(void*)h, s);
}
DOLRECOMP_PSQ_AI void dolrecomp_psq_load2(const u8* h, f64* lane0, f64* lane1) {
    __m128i s = _mm_loadl_epi64((const __m128i*)(const void*)h);
    s = _mm_shuffle_epi8(s, _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 8, 9, 10, 11, 12, 13, 14, 15));
    __m128d d = _mm_cvtps_pd(_mm_castsi128_ps(s));
    _mm_storel_pd(lane0, d);
    _mm_storeh_pd(lane1, d);
}
#endif

DOLRECOMP_PSQ_AI bool dolrecomp_psq_load_inline_r(CPUState* cpu, u8* ram, u8 frD, u32 ea, bool w, u8 gqr_index, bool indexed, u32 cia) {
    if (__builtin_expect(dolrecomp_psq_type0_ok(cpu, (cpu->gqr[gqr_index & 7u] >> 16) & 7u, ea, indexed), 1)) {
        u32 off = ea - GC_RAM_BASE;
        if (__builtin_expect(off <= GC_MAIN_RAM_SIZE - 8u, 1)) {
            const u8* h = ram + off;
#if defined(DOLRECOMP_PSQ_SIMD_ON)
            if (!w) {
                dolrecomp_psq_load2(h, &cpu->fpr[frD], &cpu->ps1[frD]);
                return true;
            }
#endif
            cpu->fpr[frD] = dolrecomp_psq_single_value(read_be32(h));
            cpu->ps1[frD] = w ? 1.0 : dolrecomp_psq_single_value(read_be32(h + 4));
        } else {
            cpu->fpr[frD] = dolrecomp_psq_single_value(dolrecomp_mem_read32_r(cpu, ram, ea));
            cpu->ps1[frD] = w ? 1.0 : dolrecomp_psq_single_value(dolrecomp_mem_read32_r(cpu, ram, ea + 4u));
        }
        return true;
    }
    /* QUANTISED TYPES (4-7: u8, u16, s8, s16) INLINE. These went to
     * ppc_psq_load_full, an out-of-line call, every time -- 0.76% of module
     * samples in a fight profile (psq_load_value + ppc_psq_load_full, US,
     * frames 4800-16000, 2026-10-01). Same arithmetic as psq_load_value:
     * (f64)(f32)((f64)v * 2^-scale), so bit-identical. Integer types have no
     * alignment rule; only both lanes inside main RAM are taken here, anything
     * else still goes the full path. */
    {
        const u32 gqr = cpu->gqr[gqr_index & 7u];
        const u32 type = (gqr >> 16) & 7u;
        if (__builtin_expect(type >= 4u && (cpu->hid2 & PPC_HID2_PSE) != 0u &&
                             (indexed || (cpu->hid2 & PPC_HID2_LSQE) != 0u), 1)) {
            const u32 size = (type & 1u) ? 2u : 1u; /* 4,6: one byte; 5,7: two */
            const u32 off = ea - GC_RAM_BASE;
            if (__builtin_expect(off <= GC_MAIN_RAM_SIZE - 2u * size, 1)) {
                const u8* h = ram + off;
                const s32 scale = ((s32)(((gqr >> 24) & 0x3Fu) << 26)) >> 26;
                union { u64 u; f64 d; } m;
                m.u = (u64)(u32)(1023 - scale) << 52; /* 2^-scale, exact */
                f64 a, b = 0.0;
                switch (type) {
                case 4: a = (f64)h[0];                 if (!w) b = (f64)h[1];                 break;
                case 5: a = (f64)read_be16(h);         if (!w) b = (f64)read_be16(h + 2);     break;
                case 6: a = (f64)(s8)h[0];             if (!w) b = (f64)(s8)h[1];             break;
                default: a = (f64)(s16)read_be16(h);   if (!w) b = (f64)(s16)read_be16(h + 2); break;
                }
                cpu->fpr[frD] = (f64)(f32)(a * m.d);
                cpu->ps1[frD] = w ? 1.0 : (f64)(f32)(b * m.d);
                return true;
            }
        }
    }
    return ppc_psq_load_full(cpu, frD, ea, w, gqr_index, indexed, cia);
}

DOLRECOMP_PSQ_AI bool dolrecomp_psq_store_inline_r(CPUState* cpu, u8* ram, u8 frS, u32 ea, bool w, u8 gqr_index, bool indexed, u32 cia) {
    if (__builtin_expect(dolrecomp_psq_type0_ok(cpu, cpu->gqr[gqr_index & 7u] & 7u, ea, indexed), 1)) {
        u32 off = ea - GC_RAM_BASE;
        if (__builtin_expect(off <= GC_MAIN_RAM_SIZE - 8u, 1)) {
            u8* h = ram + off;
            dolrecomp_clear_reservation(cpu, ea);
            dolrecomp_journal_cached(cpu, h, w ? 4u : 8u);
#if defined(DOLRECOMP_PSQ_SIMD_ON)
            if (!w) {
                dolrecomp_psq_store2(h, cpu->fpr[frS], cpu->ps1[frS]);
                return true;
            }
#endif
            write_be32(h, dolrecomp_psq_single_bits(cpu->fpr[frS]));
            if (!w) {
                write_be32(h + 4, dolrecomp_psq_single_bits(cpu->ps1[frS]));
            }
        } else {
            dolrecomp_mem_write32_r(cpu, ram, ea, dolrecomp_psq_single_bits(cpu->fpr[frS]));
            if (!w)
                dolrecomp_mem_write32_r(cpu, ram, ea + 4u, dolrecomp_psq_single_bits(cpu->ps1[frS]));
        }
        return true;
    }
    return ppc_psq_store_full(cpu, frS, ea, w, gqr_index, indexed, cia);
}

#define ppc_psq_load(cpu, frD, ea, w, gqr, idx, cia) \
    dolrecomp_psq_load_inline_r((cpu), DOLRECOMP_RAM_ARG(cpu), (frD), (ea), (w), (gqr), (idx), (cia))
#define ppc_psq_store(cpu, frS, ea, w, gqr, idx, cia) \
    dolrecomp_psq_store_inline_r((cpu), DOLRECOMP_RAM_ARG(cpu), (frS), (ea), (w), (gqr), (idx), (cia))
#endif

#endif /* DOLRECOMP_CPU_H */
