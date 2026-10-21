/* --native-regions: what the post-pass does to real chunk files.
 *
 * The fixture is generated, not checked in: synthetic instruction words go
 * through the real decoder, emitter and twin writer (run_chunk_jobs) with the
 * shipped flags, so the pass sees exactly the text it sees in a game build --
 * and no game code is involved.
 *
 * Layout: two 16-instruction windows at 0x80100000 and 0x80100040. The first
 * window is written with an OVERHANG: the second window's first instructions
 * are emitted a second time at its end, the way --chunk-overhang does it. A
 * region inside that span therefore exists in TWO hot copies, and execution
 * can reach either one.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/backend/codegen.h"
#include "../src/backend/emitter.h"
#include "../src/backend/native_regions.h"
#include "../src/backend/twin.h"
#include "../src/common/types.h"
#include "../src/frontend/decoder.h"

#define BASE 0x80100000u
#define WIN  16u

static int failures;
static const char* s_twin_regs = "ratio";  /* gen_tree: --twin-regs */

static void expect(const char* test, bool cond, const char* what) {
    if (!cond) {
        fprintf(stderr, "FAIL %s: %s\n", test, what);
        failures++;
    }
}

static char* slurp(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* t = malloc((size_t)n + 1);
    if (fread(t, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(t); return NULL; }
    fclose(f);
    t[n] = 0;
    return t;
}

static bool write_text(const char* path, const char* text) {
    FILE* f = fopen(path, "w");
    if (!f) return false;
    fputs(text, f);
    return fclose(f) == 0;
}

/* Two windows of `window_insts` from `words` at `base`; the first also emits the
 * second's first `overhang` instructions. Hot entry PCs from hot_pcs. */
static bool gen_tree(const u32* words, u32 n, u32 base, u32 window_insts, u32 overhang, const u32* hot_pcs, u32 n_hot,
                     const char* dir) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/hot.txt", dir);
    FILE* h = fopen(path, "w");
    if (!h) return false;
    for (u32 i = 0; i < n_hot; i++) fprintf(h, "%08X 1000\n", hot_pcs[i]);
    fclose(h);
    if (!twin_load_hot(path) || !twin_set_regs(s_twin_regs)) return false;
    twin_set_cr(1);
    emit_set_leader_cases(true);
    emit_set_fp_check_once(true);
    emit_set_ram_bases((1u << 1) | (1u << 2) | (1u << 13));
    emit_set_direct_calls(true);
    emit_set_self_calls(true);

    PPCInst* insts = calloc(n, sizeof(PPCInst));
    for (u32 i = 0; i < n; i++) insts[i] = ppc_decode(words[i], base + 4u * i);
    snprintf(path, sizeof(path), "%s/chunks", dir);
    {
        char cmd[1200];
        snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", path);
        if (system(cmd) != 0) { free(insts); return false; }
    }
    ChunkJob jobs[2];
    memset(jobs, 0, sizeof(jobs));
    for (u32 j = 0; j < 2; j++) {
        jobs[j].insts = insts + j * window_insts;
        jobs[j].count = window_insts;
        jobs[j].func_addr = base + 4u * j * window_insts;
        snprintf(jobs[j].path, sizeof(jobs[j].path), "%s/chunk_%04u_text1_%08X.c", path, j, jobs[j].func_addr);
        snprintf(jobs[j].include_name, sizeof(jobs[j].include_name), "generated.h");
    }
    jobs[0].emit_count = window_insts + overhang;
    bool ok = run_chunk_jobs(jobs, 2, 1) != 0;
    free(insts);
    return ok;
}

/* Run the pass over dir/chunks with the given region list; stdout captured into *log. */
static bool apply(const char* dir, const char* regions, char** log) {
    char path[1024], chunks[1024], logp[1024];
    snprintf(path, sizeof(path), "%s/list.regions", dir);
    snprintf(chunks, sizeof(chunks), "%s/chunks", dir);
    snprintf(logp, sizeof(logp), "%s/apply.log", dir);
    if (!write_text(path, regions)) return false;
    native_regions_reset();
    if (!native_regions_load(path)) return false;
    fflush(stdout);
    int saved = dup(1);
    FILE* lf = freopen(logp, "w", stdout);
    bool ok = lf && native_regions_apply(chunks);
    fflush(stdout);
    dup2(saved, 1);
    close(saved);
    *log = slurp(logp);
    return ok && *log;
}

static char* chunk_text(const char* dir, u32 j) {
    char path[1200];
    snprintf(path, sizeof(path), "%s/chunks/chunk_%04u_text1_%08X.c", dir, j, BASE + 4u * j * WIN);
    return slurp(path);
}

/* The hot function's body: from `void func_<window>(CPUState* ctx) {` to the end of the file. */
static const char* hot_part(const char* text, u32 window) {
    char want[64];
    snprintf(want, sizeof(want), "\nvoid func_%08X(CPUState* ctx) {", window);
    const char* p = strstr(text, want);
    return p ? p : "";
}

static void rm_tmp(char* dir) {
    char cmd[1200];
    if (getenv("NREG_TEST_KEEP")) { fprintf(stderr, "kept %s\n", dir); free(dir); return; }
    if (strncmp(dir, "/tmp/test_native_regions_", 25) == 0) {
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
        if (system(cmd) != 0) fprintf(stderr, "warning: could not remove %s\n", dir);
    }
    free(dir);
}

static char* mkdir_tmp(void) {
    char* t = strdup("/tmp/test_native_regions_XXXXXX");
    return mkdtemp(t);
}

/* Window 1: filler. Window 2 (0x80100040): a CTR loop, then blr. */
static void loop_program(u32* w) {
    for (u32 i = 0; i < 2 * WIN; i++) w[i] = 0x60000000u; /* nop */
    w[WIN - 1] = 0x48000004u;                            /* b +4: fall into window 2 */
    u32 k = WIN;
    w[k++] = 0x39200004u; /* 40 li    r9, 4      */
    w[k++] = 0x7D2903A6u; /* 44 mtctr r9         */
    w[k++] = 0x80640000u; /* 48 lwz   r3, 0(r4)  loop head */
    w[k++] = 0x38630001u; /* 4C addi  r3, r3, 1  */
    w[k++] = 0x90640000u; /* 50 stw   r3, 0(r4)  */
    w[k++] = 0x38840004u; /* 54 addi  r4, r4, 4  */
    w[k++] = 0x4200FFF0u; /* 58 bdnz  0x80100048 */
    w[k++] = 0x4E800020u; /* 5C blr              */
    w[2 * WIN - 1] = 0x4E800020u;
}

static void test_overhang_copy_patched(void) {
    const char* T = "overhang_copy_patched";
    u32 w[2 * WIN];
    loop_program(w);
    u32 hot[] = {BASE, BASE + 0x40, BASE + 0x48};
    char* dir = mkdir_tmp();
    char* log = NULL;
    expect(T, gen_tree(w, 2 * WIN, BASE, WIN, 8, hot, 3, dir), "fixture generated");
    expect(T, apply(dir, "80100040 8010005C\n", &log), "pass ran");
    char* a = chunk_text(dir, 0);
    char* b = chunk_text(dir, 1);
    expect(T, a && strstr(hot_part(a, BASE), "/* NATIVE REGION 80100040"), "region in window 1's overhang copy");
    expect(T, b && strstr(hot_part(b, BASE + 0x40), "/* NATIVE REGION 80100040"), "region in window 2's home copy");
    expect(T, log && strstr(log, "(2 copies"), "log counts 2 copies");
    if (failures) fprintf(stderr, "--- log:\n%s\n", log ? log : "(none)");
    free(a); free(b); free(log); rm_tmp(dir);
}

static void test_partial_span_not_patched(void) {
    const char* T = "partial_span_not_patched";
    u32 w[2 * WIN];
    loop_program(w);
    u32 hot[] = {BASE, BASE + 0x40, BASE + 0x48};
    char* dir = mkdir_tmp();
    char* log = NULL;
    expect(T, gen_tree(w, 2 * WIN, BASE, WIN, 8, hot, 3, dir), "fixture generated");
    /* END 0x80100070 is past the 8-instruction overhang: only the home copy holds the span */
    expect(T, apply(dir, "80100040 80100070\n", &log), "pass ran");
    char* a = chunk_text(dir, 0);
    char* b = chunk_text(dir, 1);
    expect(T, a && !strstr(a, "/* NATIVE REGION"), "window 1 untouched");
    expect(T, b && strstr(hot_part(b, BASE + 0x40), "/* NATIVE REGION 80100040"), "home copy patched");
    expect(T, log && strstr(log, "(1 copies"), "log counts 1 copy");
    free(a); free(b); free(log); rm_tmp(dir);
}

/* Every `natI<n>_<pc>:` label defined in `text` is defined once. */
static bool labels_unique(const char* text) {
    for (const char* p = strstr(text, "\nnatI"); p; p = strstr(p + 1, "\nnatI")) {
        const char* e = strchr(p + 1, ':');
        if (!e || e - p > 40) continue;
        char lab[48];
        snprintf(lab, sizeof(lab), "%.*s:", (int)(e - p), p);
        const char* again = strstr(e, lab);
        if (again) return false;
    }
    return true;
}

static void test_two_inlines_distinct_labels(void) {
    const char* T = "two_inlines_distinct_labels";
    u32 w[2 * WIN];
    for (u32 i = 0; i < 2 * WIN; i++) w[i] = 0x60000000u;
    w[WIN - 1] = 0x48000004u;
    u32 k = WIN;
    w[k++] = 0x38800000u; /* 40 li   r4, 0              */
    w[k++] = 0x48000015u; /* 44 bl   0x80100058 (leaf)  */
    w[k++] = 0x48000011u; /* 48 bl   0x80100058         */
    w[k++] = 0x38A00000u; /* 4C li   r5, 0              */
    w[k++] = 0x4E800020u; /* 50 blr                     */
    w[k++] = 0x60000000u; /* 54 nop                     */
    w[k++] = 0x38630001u; /* 58 addi r3, r3, 1 (leaf)   */
    w[k++] = 0x4E800020u; /* 5C blr                     */
    u32 hot[] = {BASE, BASE + 0x40, BASE + 0x58};
    char* dir = mkdir_tmp();
    char* log = NULL;
    expect(T, gen_tree(w, 2 * WIN, BASE, WIN, 8, hot, 3, dir), "fixture generated");
    expect(T, apply(dir, "80100040 80100050\n", &log), "pass ran");
    char* a = chunk_text(dir, 0);
    char* b = chunk_text(dir, 1);
    expect(T, b && strstr(b, "/* NATIVE REGION 80100040"), "region made native");
    expect(T, b && strstr(b, "natI"), "leaf inlined");
    expect(T, a && labels_unique(a), "window 1 natI labels unique");
    expect(T, b && labels_unique(b), "window 2 natI labels unique");
    if (failures) fprintf(stderr, "--- log:\n%s\n", log ? log : "(none)");
    free(a); free(b); free(log); rm_tmp(dir);
}


/* Count of `needle` occurrences in text. */
static int count_of(const char* text, const char* needle) {
    int n = 0;
    for (const char* p = strstr(text, needle); p; p = strstr(p + 1, needle)) n++;
    return n;
}

/* A downcount exit inside the loop sets pc = 0x80100048 (the loop head). The chunk
 * entry switch must resume it INSIDE the region, not in the original code. */
static void test_resume_case_enters_region(void) {
    const char* T = "resume_case_enters_region";
    u32 w[2 * WIN];
    loop_program(w);
    u32 hot[] = {BASE, BASE + 0x40, BASE + 0x48};
    char* dir = mkdir_tmp();
    char* log = NULL;
    expect(T, gen_tree(w, 2 * WIN, BASE, WIN, 8, hot, 3, dir), "fixture generated");
    char* before = chunk_text(dir, 1);
    expect(T, before && strstr(hot_part(before, BASE + 0x40), "case 0x80100048u: goto label_80100048;"), "fixture has the resume case");
    expect(T, apply(dir, "80100040 8010005C\n", &log), "pass ran");
    char* b = chunk_text(dir, 1);
    const char* hot_b = b ? hot_part(b, BASE + 0x40) : "";
    expect(T, strstr(hot_b, "case 0x80100048u: goto nreg_e_80100048;") != NULL, "hot case enters the region");
    expect(T, strstr(hot_b, "case 0x80100040u: goto label_80100040;") != NULL, "START keeps its own case");
    expect(T, b && count_of(b, "\nnreg_e_80100048:") == 1, "exactly one stub");
    const char* def = strstr(hot_b, "#define NAT_RL");
    const char* stub = strstr(hot_b, "\nnreg_e_80100048:");
    expect(T, def && stub && stub > def, "stub after the NAT_RL definition");
    expect(T, stub && strstr(stub, "NAT_RL(); goto nat_80100048;") != NULL, "stub reloads and jumps to nat_");
    free(before); free(b); free(log); rm_tmp(dir);
}

/* Window 2 as an FP loop: lfs / fadds / stfs over r4. */
static void fp_loop_program(u32* w) {
    for (u32 i = 0; i < 2 * WIN; i++) w[i] = 0x60000000u;
    w[WIN - 1] = 0x48000004u;
    u32 k = WIN;
    w[k++] = 0x39200004u; /* 40 li    r9, 4         */
    w[k++] = 0x7D2903A6u; /* 44 mtctr r9            */
    w[k++] = 0xC0240000u; /* 48 lfs   f1, 0(r4)  loop head */
    w[k++] = 0xEC21082Au; /* 4C fadds f1, f1, f1    */
    w[k++] = 0xD0240000u; /* 50 stfs  f1, 0(r4)     */
    w[k++] = 0x38840004u; /* 54 addi  r4, r4, 4     */
    w[k++] = 0x4200FFF0u; /* 58 bdnz  0x80100048    */
    w[k++] = 0x4E800020u; /* 5C blr                 */
    w[2 * WIN - 1] = 0x4E800020u;
}

static void test_resume_stub_checks_fp(void) {
    const char* T = "resume_stub_checks_fp";
    u32 w[2 * WIN];
    fp_loop_program(w);
    u32 hot[] = {BASE, BASE + 0x40, BASE + 0x48};
    char* dir = mkdir_tmp();
    char* log = NULL;
    expect(T, gen_tree(w, 2 * WIN, BASE, WIN, 8, hot, 3, dir), "fixture generated");
    expect(T, apply(dir, "80100040 8010005C\n", &log), "pass ran");
    char* b = chunk_text(dir, 1);
    expect(T, b && strstr(b, "/* NATIVE REGION 80100040"), "FP region made native");
    expect(T, b && strstr(b, "\nnreg_e_80100048: if (!(ctx->msr & 0x00002000u)) goto label_80100048; NAT_RL(); goto nat_80100048;"),
           "FP stub tests MSR.FP first and falls back to the original label");
    free(b); free(log); rm_tmp(dir);
}

/* A failed RAM guard must still resume in the ORIGINAL code: if it entered the stub it
 * would re-enter the region and fail the same guard forever. */
static void test_bail_still_original(void) {
    const char* T = "bail_still_original";
    u32 w[2 * WIN];
    loop_program(w);
    u32 hot[] = {BASE, BASE + 0x40, BASE + 0x48};
    char* dir = mkdir_tmp();
    char* log = NULL;
    expect(T, gen_tree(w, 2 * WIN, BASE, WIN, 8, hot, 3, dir), "fixture generated");
    expect(T, apply(dir, "80100040 8010005C\n", &log), "pass ran");
    char* b = chunk_text(dir, 1);
    int guards = 0, bad = 0;
    for (const char* p = b ? strstr(b, "if (!nreg_in_ram(") : NULL; p; p = strstr(p + 1, "if (!nreg_in_ram(")) {
        const char* eol = strchr(p, '\n');
        const char* g = strstr(p, "goto ");
        guards++;
        if (!g || (eol && g > eol) || strncmp(g, "goto label_", 11) != 0) bad++;
    }
    expect(T, guards >= 2, "the loop has guarded accesses");
    expect(T, bad == 0, "every guard bails to label_");
    free(b); free(log); rm_tmp(dir);
}

/* The cold copy never has a region, so its switch must not change. */
static void test_cold_switch_untouched(void) {
    const char* T = "cold_switch_untouched";
    u32 w[2 * WIN];
    loop_program(w);
    u32 hot[] = {BASE, BASE + 0x40, BASE + 0x48};
    char* dir = mkdir_tmp();
    char* log = NULL;
    expect(T, gen_tree(w, 2 * WIN, BASE, WIN, 8, hot, 3, dir), "fixture generated");
    char* before = chunk_text(dir, 1);
    expect(T, apply(dir, "80100040 8010005C\n", &log), "pass ran");
    char* after = chunk_text(dir, 1);
    const char* cb = before ? strstr(before, "_cold(CPUState* ctx) {") : NULL;
    const char* ca = after ? strstr(after, "_cold(CPUState* ctx) {") : NULL;
    const char* cbe = cb ? strstr(cb, "default:") : NULL;
    const char* cae = ca ? strstr(ca, "default:") : NULL;
    expect(T, cb && ca && cbe && cae && (cbe - cb) == (cae - ca) && memcmp(cb, ca, (size_t)(cbe - cb)) == 0,
           "cold switch byte-identical");
    free(before); free(after); free(log); rm_tmp(dir);
}

/* Window 2 as a loop over one double: lfd / <op> / stfd. */
static void double_loop_program(u32* w, u32 op1, u32 op2) {
    for (u32 i = 0; i < 2 * WIN; i++) w[i] = 0x60000000u;
    w[WIN - 1] = 0x48000004u;
    u32 k = WIN;
    w[k++] = 0x39200004u; /* 40 li    r9, 4      */
    w[k++] = 0x7D2903A6u; /* 44 mtctr r9         */
    w[k++] = 0xC8240000u; /* 48 lfd   f1, 0(r4)  */
    w[k++] = op1;         /* 4C                  */
    w[k++] = op2;         /* 50                  */
    w[k++] = 0xD8240000u; /* 54 stfd  f1, 0(r4)  */
    w[k++] = 0x4200FFF0u; /* 58 bdnz  0x80100048 */
    w[k++] = 0x4E800020u; /* 5C blr              */
    w[2 * WIN - 1] = 0x4E800020u;
}

static void expect_native(const char* T, const u32* w, const char* list, const char* must_contain) {
    u32 hot[] = {BASE, BASE + 0x40, BASE + 0x48};
    char* dir = mkdir_tmp();
    char* log = NULL;
    expect(T, gen_tree(w, 2 * WIN, BASE, WIN, 8, hot, 3, dir), "fixture generated");
    char* before = chunk_text(dir, 1);
    expect(T, before && strstr(before, must_contain), "fixture emits the helper under test");
    expect(T, apply(dir, list, &log), "pass ran");
    char* b = chunk_text(dir, 1);
    expect(T, b && strstr(b, "/* NATIVE REGION 80100040"), "region made native");
    if (b && !strstr(b, "/* NATIVE REGION 80100040")) fprintf(stderr, "--- %s log:\n%s\n", T, log ? log : "(none)");
    free(before); free(b); free(log); rm_tmp(dir);
}

/* Double-precision arithmetic emits dolrecomp_fprf_d(ctx, ...): FPSCR only, like the listed _s twin. */
static void test_fprf_d_region_native(void) {
    u32 w[2 * WIN];
    double_loop_program(w, 0xFC21082Au /* fadd f1,f1,f1 */, 0xFC210072u /* fmul f1,f1,f1 */);
    expect_native("fprf_d_region_native", w, "80100040 8010005C\n", "dolrecomp_fprf_d(ctx");
}

/* frsqrte: FPSCR only, result through a pointer to a C local. */
static void test_frsqrte_region_native(void) {
    u32 w[2 * WIN];
    double_loop_program(w, 0xFC200834u /* frsqrte f1,f1 */, 0x60000000u);
    expect_native("frsqrte_region_native", w, "80100040 8010005C\n", "ppc_frsqrte(ctx");
}

/* ps_rsqrte: same, paired. */
static void test_ps_rsqrte_region_native(void) {
    u32 w[2 * WIN];
    double_loop_program(w, 0x10200834u /* ps_rsqrte f1,f1 */, 0x60000000u);
    expect_native("ps_rsqrte_region_native", w, "80100040 8010005C\n", "ppc_ps_rsqrte(ctx");
}

/* A caller of a float clamp that returns early with a conditional `bclr` (the shape of
 * the US clamp at 800165EC): the clamp is a leaf and must be inlined. */
static void test_bclr_leaf_inlined(void) {
    const char* T = "bclr_leaf_inlined";
    u32 w[2 * WIN];
    for (u32 i = 0; i < 2 * WIN; i++) w[i] = 0x60000000u;
    w[WIN - 1] = 0x48000004u;
    u32 k = WIN;
    w[k++] = 0xC0240000u; /* 40 lfs   f1, 0(r4)          */
    w[k++] = 0x48000011u; /* 44 bl    0x80100054 (clamp) */
    w[k++] = 0xD0240000u; /* 48 stfs  f1, 0(r4)          */
    w[k++] = 0x4E800020u; /* 4C blr                      */
    k = WIN + 5;          /* the clamp is not the last code: callee_text needs the label after it */
    w[k++] = 0xFC011040u; /* 54 fcmpo cr0, f1, f2        */
    w[k++] = 0x4080000Cu; /* 58 bc    4, 0, 0x80100064   */
    w[k++] = 0xFC201090u; /* 5C fmr   f1, f2             */
    w[k++] = 0x4E800020u; /* 60 blr                      */
    w[k++] = 0xFC011840u; /* 64 fcmpo cr0, f1, f3        */
    w[k++] = 0x4C810020u; /* 68 bclr  4, 1               */
    w[k++] = 0xFC201890u; /* 6C fmr   f1, f3             */
    w[k++] = 0x4E800020u; /* 70 blr                      */
    u32 hot[] = {BASE, BASE + 0x40, BASE + 0x54};
    char* dir = mkdir_tmp();
    char* log = NULL;
    expect(T, gen_tree(w, 2 * WIN, BASE, WIN, 0, hot, 3, dir), "fixture generated");
    expect(T, apply(dir, "80100040 8010004C\n", &log), "pass ran");
    char* b = chunk_text(dir, 1);
    expect(T, b && strstr(b, "/* NATIVE REGION 80100040"), "caller made native");
    expect(T, b && strstr(b, "/*NREG_INLINE_BEGIN 80100054*/"), "clamp inlined");
    if (b && !strstr(b, "/* NATIVE REGION 80100040")) fprintf(stderr, "--- %s log:\n%s\n", T, log ? log : "(none)");
    free(b); free(log); rm_tmp(dir);
}

/* mfcr reads the packed CR while the region holds its fields in nC locals: the region must
 * compose it from the locals instead of calling cpu_cr_get(ctx) on stale ctx->crf. */
static void test_mfcr_region_native(void) {
    const char* T = "mfcr_region_native";
    u32 w[2 * WIN];
    for (u32 i = 0; i < 2 * WIN; i++) w[i] = 0x60000000u;
    w[WIN - 1] = 0x48000004u;
    u32 k = WIN;
    w[k++] = 0x39200004u; /* 40 li    r9, 4       */
    w[k++] = 0x7D2903A6u; /* 44 mtctr r9          */
    w[k++] = 0x80640000u; /* 48 lwz   r3, 0(r4)   */
    w[k++] = 0x2C030000u; /* 4C cmpwi r3, 0       */
    w[k++] = 0x7C000026u; /* 50 mfcr  r0          */
    w[k++] = 0x90040000u; /* 54 stw   r0, 0(r4)   */
    w[k++] = 0x4200FFF0u; /* 58 bdnz  0x80100048  */
    w[k++] = 0x4E800020u; /* 5C blr               */
    u32 hot[] = {BASE, BASE + 0x40, BASE + 0x48};
    char* dir = mkdir_tmp();
    char* log = NULL;
    expect(T, gen_tree(w, 2 * WIN, BASE, WIN, 0, hot, 3, dir), "fixture generated");
    expect(T, apply(dir, "80100040 8010005C\n", &log), "pass ran");
    char* b = chunk_text(dir, 1);
    const char* r = b ? strstr(b, "/* NATIVE REGION 80100040") : NULL;
    const char* e = r ? strstr(r, "#undef NAT_RL") : NULL;
    expect(T, r != NULL, "region made native");
    if (r && e) {
        char* blk = strndup(r, (size_t)(e - r));
        expect(T, !strstr(blk, "cpu_cr_get(ctx)"), "no cpu_cr_get(ctx) inside the region");
        expect(T, strstr(blk, "((u32)(nC0 & 0xFu) << 28)") && strstr(blk, "((u32)(nC7 & 0xFu) << 0)"),
               "CR composed from all eight field locals, cpu_cr_get's bit order");
        free(blk);
    }
    if (!r) fprintf(stderr, "--- %s log:\n%s\n", T, log ? log : "(none)");
    free(b); free(log); rm_tmp(dir);
}

/* A region calling A, where A saves LR and calls the leaf C: A and C both inlined, each
 * with its own natI prefix. (The US shape: 800082C8 -> 80006260 -> 801BBECC -> ...) */
static void test_two_level_chain_inlined(void) {
    const char* T = "two_level_chain_inlined";
    u32 w[2 * WIN];
    for (u32 i = 0; i < 2 * WIN; i++) w[i] = 0x60000000u;
    w[WIN - 1] = 0x48000004u;
    u32 k = WIN;
    w[k++] = 0x48000011u; /* 40 bl    0x80100050 (A) */
    w[k++] = 0x60000000u; /* 44 nop                  */
    w[k++] = 0x4E800020u; /* 48 blr                  */
    w[k++] = 0x60000000u; /* 4C nop                  */
    w[k++] = 0x7C0802A6u; /* 50 A: mflr r0           */
    w[k++] = 0x90010004u; /* 54 stw   r0, 4(r1)      */
    w[k++] = 0x48000015u; /* 58 bl    0x8010006C (C) */
    w[k++] = 0x80010004u; /* 5C lwz   r0, 4(r1)      */
    w[k++] = 0x7C0803A6u; /* 60 mtlr  r0             */
    w[k++] = 0x4E800020u; /* 64 blr                  */
    w[k++] = 0x60000000u; /* 68 nop                  */
    w[k++] = 0x38630001u; /* 6C C: addi r3, r3, 1    */
    w[k++] = 0x4E800020u; /* 70 blr                  */
    u32 hot[] = {BASE, BASE + 0x40, BASE + 0x50, BASE + 0x6C};
    char* dir = mkdir_tmp();
    char* log = NULL;
    expect(T, gen_tree(w, 2 * WIN, BASE, WIN, 0, hot, 4, dir), "fixture generated");
    expect(T, apply(dir, "80100040 80100048\n", &log), "pass ran");
    char* b = chunk_text(dir, 1);
    expect(T, b && strstr(b, "/* NATIVE REGION 80100040"), "region made native");
    expect(T, b && strstr(b, "/*NREG_INLINE_BEGIN 80100050*/") && strstr(b, "/*NREG_INLINE_BEGIN 8010006C*/"), "A and C inlined");
    expect(T, b && labels_unique(b), "natI labels unique");
    if (b && !strstr(b, "/* NATIVE REGION 80100040")) fprintf(stderr, "--- %s log:\n%s\n", T, log ? log : "(none)");
    free(b); free(log); rm_tmp(dir);
}

/* A five-deep chain (F1 -> F2 -> F3 -> F4 -> F5): past the depth limit, so the region
 * stays as emitted and the log names the call and the reason. */
static void test_depth_limit_reports(void) {
    const char* T = "depth_limit_reports";
    u32 w[2 * WIN];
    for (u32 i = 0; i < 2 * WIN; i++) w[i] = 0x60000000u;
    w[WIN - 1] = 0x48000004u;
    u32 k = WIN;
    w[k++] = 0x4800000Du; /* 40 bl  0x8010004C (F1) */
    w[k++] = 0x60000000u; /* 44 nop                 */
    w[k++] = 0x4E800020u; /* 48 blr                 */
    for (u32 f = 0; f < 4; f++) {                       /* F1..F4 at 4C, 54, 5C, 64: bl next; blr */
        w[k++] = 0x48000009u;
        w[k++] = 0x4E800020u;
    }
    w[k++] = 0x38630001u; /* 6C F5: addi r3, r3, 1  */
    w[k++] = 0x4E800020u; /* 70 blr                 */
    u32 hot[] = {BASE, BASE + 0x40};
    char* dir = mkdir_tmp();
    char* log = NULL;
    expect(T, gen_tree(w, 2 * WIN, BASE, WIN, 0, hot, 2, dir), "fixture generated");
    expect(T, apply(dir, "80100040 80100048\n", &log), "pass ran");
    char* b = chunk_text(dir, 1);
    expect(T, b && !strstr(b, "/* NATIVE REGION 80100040"), "region left as emitted");
    expect(T, log && strstr(log, "not inlined: too deep"), "log says too deep");
    if (log && !strstr(log, "too deep")) fprintf(stderr, "--- %s log:\n%s\n", T, log);
    free(b); free(log); rm_tmp(dir);
}

/* Window 2: a region whose one call goes to a function the pass cannot inline (an indirect
 * branch through CTR). */
static void call_exit_program(u32* w) {
    for (u32 i = 0; i < 2 * WIN; i++) w[i] = 0x60000000u;
    w[WIN - 1] = 0x48000004u;
    u32 k = WIN;
    w[k++] = 0x38800000u; /* 40 li    r4, 0             */
    w[k++] = 0x48000011u; /* 44 bl    0x80100054        */
    w[k++] = 0x38A00000u; /* 48 li    r5, 0             */
    w[k++] = 0x4E800020u; /* 4C blr                     */
    w[k++] = 0x60000000u; /* 50 nop                     */
    w[k++] = 0x7C6903A6u; /* 54 mtctr r3   (callee)     */
    w[k++] = 0x4E800420u; /* 58 bctr                    */
}

/* With the `x` column, the call becomes an exit to the original code at the bl. */
static void test_x_region_call_exits(void) {
    const char* T = "x_region_call_exits";
    u32 w[2 * WIN];
    call_exit_program(w);
    u32 hot[] = {BASE, BASE + 0x40, BASE + 0x54};
    char* dir = mkdir_tmp();
    char* log = NULL;
    expect(T, gen_tree(w, 2 * WIN, BASE, WIN, 0, hot, 3, dir), "fixture generated");
    expect(T, apply(dir, "80100040 8010004C x\n", &log), "pass ran");
    char* b = chunk_text(dir, 1);
    expect(T, b && strstr(b, "/* NATIVE REGION 80100040"), "region made native");
    expect(T, b && strstr(b, "NREG_CALL_EXIT(80100044);"), "the call is an exit at its own pc");
    expect(T, b && strstr(b, "#define NREG_CALL_EXIT(pc) do { NAT_WB(); goto label_##pc; } while (0)"),
           "the exit writes the locals back and resumes the original code");
    if (b && !strstr(b, "/* NATIVE REGION 80100040")) fprintf(stderr, "--- %s log:\n%s\n", T, log ? log : "(none)");
    free(b); free(log); rm_tmp(dir);
}

/* Without it, nothing changes: the region is refused and the log names the call. */
static void test_no_x_still_refuses(void) {
    const char* T = "no_x_still_refuses";
    u32 w[2 * WIN];
    call_exit_program(w);
    u32 hot[] = {BASE, BASE + 0x40, BASE + 0x54};
    char* dir = mkdir_tmp();
    char* log = NULL;
    expect(T, gen_tree(w, 2 * WIN, BASE, WIN, 0, hot, 3, dir), "fixture generated");
    expect(T, apply(dir, "80100040 8010004C\n", &log), "pass ran");
    char* b = chunk_text(dir, 1);
    expect(T, b && !strstr(b, "/* NATIVE REGION 80100040"), "region left as emitted");
    expect(T, log && strstr(log, "call 80100044 -> 80100054 not inlined"), "log names the call");
    free(b); free(log); rm_tmp(dir);
}

/* The line of `text` holding `needle`, as a new string. */
static char* line_with(const char* text, const char* needle) {
    const char* p = text ? strstr(text, needle) : NULL;
    if (!p) return NULL;
    const char* a = p; while (a > text && a[-1] != '\n') a--;
    const char* e = strchr(p, '\n');
    return strndup(a, e ? (size_t)(e - a) : strlen(a));
}

/* An exit to a BLOCK HEAD lands on `ctx->pc = X; DOLRECOMP_DC -= n;`, which the region already
 * charged at nat_X: it must give the n back first or every later event moves by n cycles. A bail
 * from mid-block lands on a label with no charge and must not. */
static void test_bail_at_block_head_refunds(void) {
    const char* T = "bail_at_block_head_refunds";
    u32 w[2 * WIN];
    loop_program(w);   /* 48 lwz = loop head (block head), 50 stw = mid-block */
    u32 hot[] = {BASE, BASE + 0x40, BASE + 0x48};
    char* dir = mkdir_tmp();
    char* log = NULL;
    expect(T, gen_tree(w, 2 * WIN, BASE, WIN, 0, hot, 3, dir), "fixture generated");
    expect(T, apply(dir, "80100040 8010005C\n", &log), "pass ran");
    char* b = chunk_text(dir, 1);
    char* head = line_with(b, "goto label_80100048; }");
    char* mid = line_with(b, "goto label_80100050; }");
    expect(T, head && strstr(head, "nreg_in_ram") && strstr(head, "DOLRECOMP_DC += "), "block-head bail refunds the block charge");
    expect(T, mid && strstr(mid, "nreg_in_ram") && !strstr(mid, "DOLRECOMP_DC += "), "mid-block bail does not");
    free(head); free(mid); free(b); free(log); rm_tmp(dir);
}

/* A bail at the region's FIRST instruction must not jump to label_START: the region block sits
 * right there, so it would re-enter and bail again forever. It goes past the block instead. */
static void test_bail_at_start_skips_region(void) {
    const char* T = "bail_at_start_skips_region";
    u32 w[2 * WIN];
    loop_program(w);
    u32 hot[] = {BASE, BASE + 0x40, BASE + 0x48};
    char* dir = mkdir_tmp();
    char* log = NULL;
    expect(T, gen_tree(w, 2 * WIN, BASE, WIN, 0, hot, 3, dir), "fixture generated");
    expect(T, apply(dir, "80100048 8010005C\n", &log), "pass ran");   /* starts AT the lwz */
    char* b = chunk_text(dir, 1);
    expect(T, b && strstr(b, "/* NATIVE REGION 80100048"), "region made native");
    char* bail = line_with(b, "goto nreg_o_80100048; }");
    expect(T, bail && strstr(bail, "nreg_in_ram") && b && !strstr(b, "goto label_80100048; }"), "start bail skips the region");
    const char* und = b ? strstr(b, "#undef NAT_RL") : NULL;
    const char* lab = b ? strstr(b, "\nnreg_o_80100048:") : NULL;
    expect(T, und && lab && lab > und, "nreg_o_START sits after the region block");
    free(bail); free(b); free(log); rm_tmp(dir);
}

/* NREG_CALL_EXIT to a block-head bl gives the block charge back too. */
static void test_call_exit_refunds(void) {
    const char* T = "call_exit_refunds";
    u32 w[2 * WIN];
    for (u32 i = 0; i < 2 * WIN; i++) w[i] = 0x60000000u;
    w[WIN - 1] = 0x48000004u;
    u32 k = WIN;
    w[k++] = 0x38800000u; /* 40 li    r4, 0                     */
    w[k++] = 0x48000004u; /* 44 b     0x80100048 (makes 48 a head) */
    w[k++] = 0x4800000Du; /* 48 bl    0x80100054                */
    w[k++] = 0x38A00000u; /* 4C li    r5, 0                     */
    w[k++] = 0x4E800020u; /* 50 blr                             */
    w[k++] = 0x7C6903A6u; /* 54 mtctr r3   (callee)             */
    w[k++] = 0x4E800420u; /* 58 bctr                            */
    u32 hot[] = {BASE, BASE + 0x40, BASE + 0x48, BASE + 0x54};
    char* dir = mkdir_tmp();
    char* log = NULL;
    expect(T, gen_tree(w, 2 * WIN, BASE, WIN, 0, hot, 4, dir), "fixture generated");
    expect(T, apply(dir, "80100040 80100050 x\n", &log), "pass ran");
    char* b = chunk_text(dir, 1);
    char* ex = line_with(b, "NREG_CALL_EXIT(80100048);");
    expect(T, ex && strstr(ex, "DOLRECOMP_DC += "), "block-head call exit refunds the block charge");
    free(ex); free(b); free(log); rm_tmp(dir);
}

/* A twinned register written only by an inlined callee: the call block it replaced resynced the
 * twin (dr_gN = ctx->gpr[N]), so NAT_WB must write dr_gN back too. */
static void test_inlined_call_resyncs_twins(void) {
    const char* T = "inlined_call_resyncs_twins";
    u32 w[2 * WIN];
    for (u32 i = 0; i < 2 * WIN; i++) w[i] = 0x60000000u;
    w[WIN - 1] = 0x48000004u;
    u32 k = WIN;
    w[k++] = 0x48000011u; /* 40 bl   0x80100050 (leaf writes r3) */
    w[k++] = 0x60000000u; /* 44 nop                              */
    w[k++] = 0x4E800020u; /* 48 blr                              */
    k = WIN + 4;
    w[k++] = 0x38630001u; /* 50 addi r3, r3, 1                   */
    w[k++] = 0x4E800020u; /* 54 blr                              */
    u32 hot[] = {BASE, BASE + 0x40, BASE + 0x50};
    char* dir = mkdir_tmp();
    char* log = NULL;
    s_twin_regs = "1,2,3,13";
    expect(T, gen_tree(w, 2 * WIN, BASE, WIN, 0, hot, 3, dir), "fixture generated");
    s_twin_regs = "ratio";
    char* before = chunk_text(dir, 1);
    expect(T, before && strstr(hot_part(before, BASE + 0x40), "dr_g3"), "fixture twins r3");
    expect(T, apply(dir, "80100040 80100048\n", &log), "pass ran");
    char* b = chunk_text(dir, 1);
    char* wb = line_with(b, "#define NAT_WB()");
    expect(T, b && strstr(b, "/* NATIVE REGION 80100040"), "region made native");
    expect(T, wb && strstr(wb, "dr_g3 = nG3;"), "NAT_WB resyncs the twin the inlined call wrote");
    free(wb); free(before); free(b); free(log); rm_tmp(dir);
}

/* Rewrite every chunk file of dir with CRLF line endings, the way dolrecomp.exe writes them on Windows. */
static void to_crlf(const char* dir) {
    for (u32 j = 0; j < 2; j++) {
        char path[1200];
        snprintf(path, sizeof(path), "%s/chunks/chunk_%04u_text1_%08X.c", dir, j, BASE + 4u * j * WIN);
        char* t = slurp(path);
        if (!t) continue;
        FILE* f = fopen(path, "wb");
        for (const char* p = t; *p; p++) { if (*p == '\n') fputc('\r', f); fputc(*p, f); }
        fclose(f);
        free(t);
    }
}

/* Windows: the chunk files have CRLF endings. Every label compare in the pass must still match --
 * the 1.6.7 Windows build made 0 of 45 US regions native because of this. */
static void test_crlf_chunks_patched(void) {
    const char* T = "crlf_chunks_patched";
    u32 w[2 * WIN];
    loop_program(w);
    u32 hot[] = {BASE, BASE + 0x40, BASE + 0x48};
    char* dir = mkdir_tmp();
    char* log = NULL;
    expect(T, gen_tree(w, 2 * WIN, BASE, WIN, 8, hot, 3, dir), "fixture generated");
    to_crlf(dir);
    char* before = chunk_text(dir, 1);
    expect(T, before && strstr(before, "label_80100048:\r\n"), "fixture has CRLF");
    expect(T, apply(dir, "80100040 8010005C\n", &log), "pass ran");
    char* b = chunk_text(dir, 1);
    expect(T, b && strstr(hot_part(b, BASE + 0x40), "/* NATIVE REGION 80100040"), "region made native from CRLF files");
    expect(T, log && strstr(log, "(2 copies, 1 resume entries"), "both copies patched and the resume case found");
    if (log && !strstr(log, "(2 copies")) fprintf(stderr, "--- %s log:\n%s\n", T, log);
    free(before); free(b); free(log); rm_tmp(dir);
}

/* --indirect-calls: with --region-indirect-calls the region keeps the native call, writing its locals back
 * first (the callee runs on ctx) and returning to its resume entry; without the flag it drops the
 * DOLRECOMP_INDIRECT_CALL line and the bctrl exits to the dispatcher. Either way the original code outside the
 * region keeps the native call. */
static void indirect_call_region(const char* T, bool flag) {
    u32 w[2 * WIN];
    for (u32 i = 0; i < 2 * WIN; i++) w[i] = 0x60000000u; /* nop */
    w[WIN - 1] = 0x4E800020u;                           /* blr: window 1 ends */
    u32 k = WIN;
    w[k++] = 0x38600000u; /* 40 li    r3, 0     */
    w[k++] = 0x38630001u; /* 44 addi  r3, r3, 1 */
    w[k++] = 0x7D8903A6u; /* 48 mtctr r12       */
    w[k++] = 0x4E800421u; /* 4C bctrl           */
    w[k++] = 0x38630002u; /* 50 addi  r3, r3, 2 */
    w[k++] = 0x4E800020u; /* 54 blr             */
    w[2 * WIN - 1] = 0x4E800020u;
    u32 hot[] = {BASE, BASE + 0x40, BASE + 0x50};
    char* dir = mkdir_tmp();
    char* log = NULL;
    emit_set_indirect_calls(true);
    expect(T, gen_tree(w, 2 * WIN, BASE, WIN, 0, hot, 3, dir), "fixture generated");
    emit_set_indirect_calls(false);
    char* before = chunk_text(dir, 1);
    expect(T, before && strstr(before, "DOLRECOMP_INDIRECT_CALL(target, 0x80100050u, label_80100050);"),
           "the emitter wrote the native call");
    free(before);
    native_regions_set_indirect_calls(flag);
    expect(T, apply(dir, "80100040 80100058\n", &log), "pass ran");
    native_regions_set_indirect_calls(false);
    char* b = chunk_text(dir, 1);
    const char* r = b ? strstr(b, "/* NATIVE REGION 80100040") : NULL;
    const char* r_end = r ? strstr(r, "#undef NAT_RL") : NULL;
    expect(T, r && r_end, "region made native");
    if (r && r_end) {
        const char* m = strstr(r, "DOLRECOMP_INDIRECT_CALL(");
        if (flag) {
            /* The call stays native, after a write-back; it returns to the resume entry at 50, which reloads. */
            const char* k = strstr(r, "{ NAT_WB(); DOLRECOMP_INDIRECT_CALL(target, 0x80100050u, nreg_e_80100050); "
                                      "DOLRECOMP_RETURN; }");
            expect(T, k && k < r_end, "the native call is kept inside the region, written back first");
        } else {
            expect(T, m && m > r_end, "flag off: no native call inside the region");
        }
        expect(T, strstr(r_end, "DOLRECOMP_INDIRECT_CALL(") != NULL, "the original code after the region keeps it");
    }
    if (!r) fprintf(stderr, "--- %s log:\n%s\n", T, log ? log : "(none)");
    free(b); free(log); rm_tmp(dir);
}

static void test_indirect_call_native_in_region(void) { indirect_call_region("indirect_call_native_in_region", true); }
static void test_indirect_call_dropped_flag_off(void) { indirect_call_region("indirect_call_dropped_flag_off", false); }

/* ---- --inline-leaves ---- */
static bool apply_leaves(const char* dir, unsigned max, char** log) {
    char chunks[1024], logp[1024];
    snprintf(chunks, sizeof(chunks), "%s/chunks", dir);
    snprintf(logp, sizeof(logp), "%s/leaves.log", dir);
    native_regions_reset();
    native_leaves_set(max);
    fflush(stdout);
    int saved = dup(1);
    FILE* lf = freopen(logp, "w", stdout);
    bool ok = lf && native_leaves_apply(chunks);
    fflush(stdout);
    dup2(saved, 1);
    close(saved);
    native_leaves_set(0);
    *log = slurp(logp);
    return ok && *log;
}

/* Window 2: a caller at 0x40 calls the callee at 0x54 (a self call). */
static void leaf_program(u32* w, bool loop) {
    for (u32 i = 0; i < 2 * WIN; i++) w[i] = 0x60000000u;
    w[WIN - 1] = 0x48000004u;
    u32 k = WIN;
    w[k++] = 0x38800010u; /* 40 li    r4, 16             */
    w[k++] = 0x48000011u; /* 44 bl    0x80100054         */
    w[k++] = 0x90640000u; /* 48 stw   r3, 0(r4)          */
    w[k++] = 0x4E800020u; /* 4C blr                      */
    k = WIN + 5;
    w[k++] = 0x80640000u; /* 54 lwz   r3, 0(r4)          */
    w[k++] = 0x2C030000u; /* 58 cmpwi r3, 0              */
    w[k++] = loop ? 0x4082FFFCu   /* 5C bne   0x80100058 (backward: a loop) */
                  : 0x41820008u;  /* 5C beq   0x80100064 (forward)          */
    w[k++] = 0x38630001u; /* 60 addi  r3, r3, 1          */
    w[k++] = 0x4E800020u; /* 64 blr                      */
}

static void test_leaf_inlined_both_copies(void) {
    const char* T = "leaf_inlined_both_copies";
    u32 w[2 * WIN];
    leaf_program(w, false);
    u32 hot[] = {BASE, BASE + 0x40, BASE + 0x54};
    char* dir = mkdir_tmp();
    char* log = NULL;
    expect(T, gen_tree(w, 2 * WIN, BASE, WIN, 0, hot, 3, dir), "fixture generated");
    expect(T, apply_leaves(dir, 64, &log), "pass ran");
    char* b = chunk_text(dir, 1);
    expect(T, b && count_of(b, "/* inlined leaf call: 80100054") == 2, "inlined in the cold AND the fast copy");
    expect(T, b && count_of(b, "func_80100040(ctx);") == 0, "no call left");
    expect(T, b && count_of(b, "if (target == 0x80100048u) {") == 2, "the blr returns to the call's label");
    expect(T, b && labels_unique(b), "labels unique");
    expect(T, log && strstr(log, "inline-leaves: 2 call sites inlined"), "reported");
    if (failures) fprintf(stderr, "--- %s log:\n%s\n", T, log ? log : "(none)");
    free(b); free(log); rm_tmp(dir);
}

static void test_leaf_loop_not_inlined(void) {
    const char* T = "leaf_loop_not_inlined";
    u32 w[2 * WIN];
    leaf_program(w, true);
    u32 hot[] = {BASE, BASE + 0x40, BASE + 0x54};
    char* dir = mkdir_tmp();
    char* log = NULL;
    expect(T, gen_tree(w, 2 * WIN, BASE, WIN, 0, hot, 3, dir), "fixture generated");
    char* before = chunk_text(dir, 1);
    expect(T, apply_leaves(dir, 64, &log), "pass ran");
    char* b = chunk_text(dir, 1);
    expect(T, b && before && strcmp(b, before) == 0, "a callee with a loop stays a call");
    free(before); free(b); free(log); rm_tmp(dir);
}

static void test_leaf_off_untouched(void) {
    const char* T = "leaf_off_untouched";
    u32 w[2 * WIN];
    leaf_program(w, false);
    u32 hot[] = {BASE, BASE + 0x40, BASE + 0x54};
    char* dir = mkdir_tmp();
    char* log = NULL;
    expect(T, gen_tree(w, 2 * WIN, BASE, WIN, 0, hot, 3, dir), "fixture generated");
    char* before = chunk_text(dir, 1);
    expect(T, apply_leaves(dir, 0, &log), "pass ran");
    char* b = chunk_text(dir, 1);
    expect(T, b && before && strcmp(b, before) == 0, "off: nothing changes");
    free(before); free(b); free(log); rm_tmp(dir);
}

/* --region-psq: a float psq_l/psq_st pair in a loop gets the direct path, with the helper (and its exception check)
 * as the else branch; a quantised psq_l (GQR6) keeps the helper alone; with the flag off nothing changes. */
static const char* lskip_t(const char* p) { while (*p == ' ') p++; return p; }

static void psq_loop_program(u32* w, u32 load) {
    for (u32 i = 0; i < 2 * WIN; i++) w[i] = 0x60000000u;
    w[WIN - 1] = 0x48000004u;
    u32 k = WIN;
    w[k++] = 0x39200004u; /* 40 li     r9, 4           */
    w[k++] = 0x7D2903A6u; /* 44 mtctr  r9              */
    w[k++] = load;        /* 48 psq_l  f1, 0(r4), 0, q */
    w[k++] = 0x1021082Au; /* 4C ps_add f1, f1, f1      */
    w[k++] = 0x60000000u; /* 50                        */
    w[k++] = 0xF0240000u; /* 54 psq_st f1, 0(r4), 0, 0 */
    w[k++] = 0x4200FFF0u; /* 58 bdnz   0x80100048      */
    w[k++] = 0x4E800020u; /* 5C blr                    */
    w[2 * WIN - 1] = 0x4E800020u;
}

static char* psq_region_text(const char* T, u32 load, bool flag) {
    u32 w[2 * WIN];
    psq_loop_program(w, load);
    u32 hot[] = {BASE, BASE + 0x40, BASE + 0x48};
    char* dir = mkdir_tmp();
    char* log = NULL;
    expect(T, gen_tree(w, 2 * WIN, BASE, WIN, 8, hot, 3, dir), "fixture generated");
    native_regions_set_psq_direct(flag);
    expect(T, apply(dir, "80100040 8010005C\n", &log), "pass ran");
    native_regions_set_psq_direct(false);
    char* b = chunk_text(dir, 1);
    expect(T, b && strstr(b, "/* NATIVE REGION 80100040"), "region made native");
    if (b && !strstr(b, "/* NATIVE REGION 80100040")) fprintf(stderr, "--- %s log:\n%s\n", T, log ? log : "(none)");
    free(log); rm_tmp(dir);
    return b;
}

static void test_region_psq_direct(void) {
    const char* T = "region_psq_direct";
    char* b = psq_region_text(T, 0xE0240000u /* psq_l f1, 0(r4), 0, 0 */, true);
    expect(T, b && strstr(b, "NREG_FN bool nreg_q0_ok("), "helpers written");
    expect(T, b && strstr(b, "if (__builtin_expect(nreg_q0_ok(ctx, ea), 1)) NREG_PSQ_L2(ea, nF1, nP1); else {"), "load fast path");
    expect(T, b && strstr(b, "NREG_PSQ_S2(h, nF1, nP1); } else {"), "store fast path");
    expect(T, b && strstr(b, "ppc_psq_load(ctx, 1u") && strstr(b, "ppc_psq_store(ctx, 1u"), "helpers kept as the fallback");
    /* the else block closes right after the helper's exception check */
    const char* e = b ? strstr(b, "nP1 = ctx->ps1[1];") : NULL;
    const char* x = e ? strstr(e, "ctx->exception") : NULL;
    const char* nl = x ? strchr(x, '\n') : NULL;
    expect(T, nl && strncmp(lskip_t(nl + 1), "}\n", 2) == 0, "else closed after the exception check");
    free(b);
}

static void test_region_psq_quantised_untouched(void) {
    const char* T = "region_psq_quantised_untouched";
    char* b = psq_region_text(T, 0xE0246000u /* psq_l f1, 0(r4), 0, 6 */, true);
    expect(T, b && strstr(b, "ppc_psq_load(ctx, 1u, ea, false, 6u"), "quantised load on the helper");
    expect(T, b && !strstr(b, "NREG_PSQ_L2(ea, "), "no direct load for GQR6");
    expect(T, b && strstr(b, "NREG_PSQ_S2(h, nF1, nP1)"), "the float store still direct");
    free(b);
}

static void test_region_psq_off_untouched(void) {
    const char* T = "region_psq_off_untouched";
    char* b = psq_region_text(T, 0xE0240000u, false);
    expect(T, b && !strstr(b, "nreg_q0_ok"), "flag off: no fast path, no helpers");
    free(b);
}

/* --region-simd: a loop with eight paired-single ops, one of each form the rewrite knows */
static void simd_loop_program(u32* w) {
    for (u32 i = 0; i < 2 * WIN; i++) w[i] = 0x60000000u;
    w[WIN - 1] = 0x48000004u;
    u32 k = WIN;
    w[k++] = 0x39200004u; /* 40 li         r9, 4             */
    w[k++] = 0x7D2903A6u; /* 44 mtctr      r9                */
    w[k++] = 0xE0240000u; /* 48 psq_l      f1, 0(r4), 0, 0   */
    w[k++] = 0x1041082Au; /* 4C ps_add     f2, f1, f1        */
    w[k++] = 0x10620072u; /* 50 ps_mul     f3, f2, f1        */
    w[k++] = 0x108118BAu; /* 54 ps_madd    f4, f1, f2, f3    */
    w[k++] = 0x10A40828u; /* 58 ps_sub     f5, f4, f1        */
    w[k++] = 0x10C02890u; /* 5C ps_mr      f6, f5            */
    w[k++] = 0x10E53460u; /* 60 ps_merge01 f7, f5, f6        */
    w[k++] = 0x11070058u; /* 64 ps_muls0   f8, f7, f1        */
    w[k++] = 0x1128385Eu; /* 68 ps_madds1  f9, f8, f1, f7    */
    w[k++] = 0xF1240000u; /* 6C psq_st     f9, 0(r4), 0, 0   */
    w[k++] = 0x4200FFD8u; /* 70 bdnz       0x80100048        */
    w[k++] = 0x4E800020u; /* 74 blr                          */
    w[2 * WIN - 1] = 0x4E800020u;
}

static char* simd_region_text(const char* T, bool many, bool flag) {
    u32 w[2 * WIN];
    if (many) simd_loop_program(w);
    else psq_loop_program(w, 0xE0240000u);
    u32 hot[] = {BASE, BASE + 0x40, BASE + 0x48};
    char* dir = mkdir_tmp();
    char* log = NULL;
    expect(T, gen_tree(w, 2 * WIN, BASE, WIN, 8, hot, 3, dir), "fixture generated");
    native_regions_set_psq_direct(true);
    native_regions_set_simd(flag);
    expect(T, apply(dir, many ? "80100040 80100074\n" : "80100040 8010005C\n", &log), "pass ran");
    native_regions_set_simd(false);
    native_regions_set_psq_direct(false);
    char* b = chunk_text(dir, 1);
    expect(T, b && strstr(b, "/* NATIVE REGION 80100040"), "region made native");
    if (b && !strstr(b, "/* NATIVE REGION 80100040")) fprintf(stderr, "--- %s log:\n%s\n", T, log ? log : "(none)");
    free(log); rm_tmp(dir);
    return b;
}

static void test_region_simd(void) {
    const char* T = "region_simd";
    char* b = simd_region_text(T, true, true);
    expect(T, b && strstr(b, "typedef double nrv_v2d __attribute__((vector_size(16)));"), "vector helpers written");
    expect(T, b && strstr(b, "nrv_v2d nV1 = { ctx->fpr[1], ctx->ps1[1] };"), "FP local pair declared as one vector");
    expect(T, b && strstr(b, "#define nF1 (nV1[0])") && strstr(b, "#define nP1 (nV1[1])"), "lane macros");
    expect(T, b && strstr(b, "#undef nF1") && strstr(b, "#undef nP1"), "lane macros undone at the region end");
    expect(T, b && strstr(b, "nV2 = nrv_d(nrv_f(nV1) + nrv_f(nV1));"), "ps_add as one vector op");
    expect(T, b && strstr(b, "nV3 = nrv_round(nV2 * nrv_force25(nV1));"), "ps_mul as one vector op");
    expect(T, b && strstr(b, "nrv_v2f ps_ = nrv_f(nV1) * nrv_f(nV2);") && strstr(b, "ps_ += nrv_f(nV3);") &&
                  strstr(b, "nV4 = nrv_d(ps_);"), "ps_madd as two vector statements (no FMA contraction)");
    expect(T, b && strstr(b, "nV5 = nrv_d(nrv_f(nV4) - nrv_f(nV1));"), "ps_sub as one vector op");
    expect(T, b && strstr(b, "nV6 = nV5;"), "ps_mr as a vector move");
    expect(T, b && strstr(b, "__builtin_shufflevector(nV5, nV6, 0, 3); nV7 = nrv_round(t_);"), "ps_merge01 as a shuffle");
    expect(T, b && strstr(b, "nV8 = nrv_round(nV7 * nrv_force25(NRV_S0(nV1)));"), "ps_muls0 with a lane splat");
    expect(T, b && strstr(b, "nrv_f(nV8) * nrv_f(NRV_S1(nV1)) + nrv_f(nV7); nV9 = nrv_d(t_);"), "ps_madds1 with a lane splat");
    expect(T, b && strstr(b, "NREG_PSQ_L2(ea, a_, b_); nrv_v2d t_ = { a_, b_ }; nV1 = t_; } else {"), "psq_l through temporaries");
    expect(T, b && !strstr(b, "f64 nF1 = ctx->fpr[1]"), "no scalar pair left for a vector register");
    free(b);
}

static void test_region_simd_few_ops_scalar(void) {
    const char* T = "region_simd_few_ops_scalar";
    char* b = simd_region_text(T, false, true);
    expect(T, b && !strstr(b, "nrv_v2d"), "one ps op: region stays scalar, no helpers");
    free(b);
}

static void test_region_simd_off_untouched(void) {
    const char* T = "region_simd_off_untouched";
    char* b = simd_region_text(T, true, false);
    expect(T, b && !strstr(b, "nrv_v2d") && strstr(b, "f64 nF1 = ctx->fpr[1], nP1 = ctx->ps1[1];"), "flag off: scalar pairs");
    free(b);
}

int main(void) {
    test_overhang_copy_patched();
    test_partial_span_not_patched();
    test_two_inlines_distinct_labels();
    test_resume_case_enters_region();
    test_resume_stub_checks_fp();
    test_bail_still_original();
    test_cold_switch_untouched();
    test_fprf_d_region_native();
    test_frsqrte_region_native();
    test_ps_rsqrte_region_native();
    test_bclr_leaf_inlined();
    test_mfcr_region_native();
    test_two_level_chain_inlined();
    test_depth_limit_reports();
    test_x_region_call_exits();
    test_no_x_still_refuses();
    test_bail_at_block_head_refunds();
    test_bail_at_start_skips_region();
    test_call_exit_refunds();
    test_inlined_call_resyncs_twins();
    test_crlf_chunks_patched();
    test_indirect_call_native_in_region();
    test_indirect_call_dropped_flag_off();
    test_leaf_inlined_both_copies();
    test_leaf_loop_not_inlined();
    test_leaf_off_untouched();
    test_region_psq_direct();
    test_region_psq_quantised_untouched();
    test_region_psq_off_untouched();
    test_region_simd();
    test_region_simd_few_ops_scalar();
    test_region_simd_off_untouched();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("native_regions: all passed\n");
    return 0;
}
