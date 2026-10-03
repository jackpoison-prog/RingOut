/* Twin chunks (--twin-hot / --twin-regs).
 *
 * Every chunk is emitted twice into its file:
 *
 *   func_X_cold  the ordinary chunk, verbatim, renamed; noinline + cold.
 *   func_X       a FAST copy: its entry switch keeps only the "hot" entry PCs
 *                (a list recorded from a training run), anything else is
 *                handed whole to func_X_cold, and chosen guest registers live
 *                in write-through locals.
 *
 * Why both halves are needed. Nearly every block leader is an entry-switch
 * case, so every leader is a join with the switch edge and clang must reload
 * guest state from ctx there; locals then cost a reconciliation at thousands
 * of joins. Measured on the US disc (arcade route): only 3.9% of the 182k cases
 * are ever entered, and with the rest pruned, locals for the rarely-written
 * base registers {r1,r2,r13,r28-r31} were -6.08% cycles on the Deck, where
 * without pruning locals lost. Pruning alone is not shippable (other modes enter
 * other cases); the cold copy makes any entry correct, and never slower than
 * before but for one call.
 *
 * Write-through: a write stores to ctx AND the local, so ctx is always current
 * and exits, exceptions and calls need no flush. Only code that writes guest
 * registers behind the chunk's back forces a reload: a directly called chunk
 * and the instruction-fallback hook (both already followed by
 * DOLRECOMP_DC_RELOAD), and lmw. A chunk with any other dynamic-index register
 * write (lswi/lswx) keeps its registers in ctx.
 *
 * At the fast copy's default no guest state has been touched, so the cold call
 * needs no flush and ends in a plain `return` -- DOLRECOMP_RETURN would write
 * the fast copy's stale downcount over the cold copy's charge. */
#include "backend/twin.h"
#include "backend/emitter.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static u32* s_hot = NULL;
static u32 s_hot_count = 0;
static u32 s_regs = 0;            /* bit i = keep guest ri in a local */
static int s_ratio = 0;           /* --twin-regs ratio: choose per chunk */
static int s_enabled = 0;
static int s_cr = 0;              /* --twin-cr: CR fields in locals too */

static int cmp_u32(const void* a, const void* b) {
    u32 x = *(const u32*)a, y = *(const u32*)b;
    return x < y ? -1 : x > y;
}

int twin_load_hot(const char* path) {
    FILE* f = fopen(path, "r");
    char line[256];
    u32 cap = 1024;
    if (!f) {
        fprintf(stderr, "error: can't open --twin-hot file '%s'\n", path);
        return 0;
    }
    s_hot = (u32*)malloc(cap * sizeof(u32));
    while (fgets(line, sizeof line, f)) {
        char* end = NULL;
        unsigned long pc = strtoul(line, &end, 16);
        unsigned long long hits = 1;
        if (end == line)
            continue;
        while (*end == ' ' || *end == '\t') end++;
        if (isdigit((unsigned char)*end))
            hits = strtoull(end, NULL, 10);
        if (!hits)
            continue;                       /* "PC 0" lines: never entered */
        if (s_hot_count == cap) {
            cap *= 2;
            s_hot = (u32*)realloc(s_hot, cap * sizeof(u32));
        }
        s_hot[s_hot_count++] = (u32)pc;
    }
    fclose(f);
    qsort(s_hot, s_hot_count, sizeof(u32), cmp_u32);
    s_enabled = 1;
    return 1;
}

int twin_set_regs(const char* list) {
    const char* p = list;
    s_regs = 0;
    s_ratio = 0;
    /* "ratio": per chunk, keep each register the chunk reads at least 20 times
     * and at least 3x as often as it writes it -- rarely-written bases (stack,
     * small-data, loop-invariant pointers) whose reloads sit on address paths.
     * Measured -0.68% cycles over the fixed pointer set on the US disc. */
    if (strcmp(list, "ratio") == 0) {
        s_ratio = 1;
        s_regs = 0xFFFFFFFFu;
        return 1;
    }
    while (*p) {
        char* end = NULL;
        long r = strtol(p, &end, 10);
        if (end == p || r < 0 || r > 31) {
            fprintf(stderr, "error: bad --twin-regs list '%s'\n", list);
            return 0;
        }
        s_regs |= 1u << r;
        p = end;
        while (*p == ',' || *p == ' ') p++;
    }
    return 1;
}

void twin_set_cr(int enable) { s_cr = enable; }
int twin_enabled(void) { return s_enabled; }
u32 twin_hot_count(void) { return s_hot_count; }

static int is_hot(u32 pc) {
    return bsearch(&pc, s_hot, s_hot_count, sizeof(u32), cmp_u32) != NULL;
}

/* The GNU memmem is missing on MinGW, so this is its portable stand-in. */
static const char* find_in(const char* h, size_t hn, const char* n, size_t nn) {
    if (nn == 0 || hn < nn) return NULL;
    for (size_t i = 0; i + nn <= hn; i++)
        if (h[i] == n[0] && memcmp(h + i, n, nn) == 0) return h + i;
    return NULL;
}

/* ---- a tiny growable string ---- */
typedef struct { char* p; size_t n, cap; } Buf;
static void bput(Buf* b, const char* s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2;
        b->p = (char*)realloc(b->p, b->cap);
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void bputs(Buf* b, const char* s) { bput(b, s, strlen(s)); }

/* A guest state array the fast copy can hold in locals: "ctx->gpr[N]" as
 * dr_gN, and under --twin-cr "ctx->crf[N]" (one CR field per byte) as dr_cN. */
typedef struct { const char* pre; size_t pre_len; int max; const char* local; } Field;
static const Field kGpr = {"ctx->gpr[", 9, 31, "dr_g"};
static const Field kCr = {"ctx->crf[", 9, 7, "dr_c"};

/* Parse "<pre>N]" at s; returns N or -1, *len = length matched. */
static int field_at(const Field* f, const char* s, size_t* len) {
    int n = 0, digits = 0;
    const char* q;
    if (strncmp(s, f->pre, f->pre_len) != 0)
        return -1;
    q = s + f->pre_len;
    while (isdigit((unsigned char)*q)) { n = n * 10 + (*q - '0'); q++; digits++; }
    if (!digits || *q != ']' || n > f->max)
        return -1;
    *len = (size_t)(q + 1 - s);
    return n;
}
static int gpr_at(const char* s, size_t* len) { return field_at(&kGpr, s, len); }

/* Copy [s, e) replacing reads of kept fields with their locals. */
static void put_reads(Buf* o, const char* s, const char* e, u32 keep, const Field* f) {
    while (s < e) {
        size_t len;
        int r = field_at(f, s, &len);
        if (r >= 0 && s + len <= e && (keep >> r & 1u)) {
            char tmp[16];
            snprintf(tmp, sizeof tmp, "%s%d", f->local, r);
            bputs(o, tmp);
            s += len;
        } else {
            bput(o, s, 1);
            s++;
        }
    }
}

/* One line of the fast copy: writes `<pre>N] = E;` of a kept field become
 * `<pre>N] = <local>N = (E');` (write-through, so ctx stays current for exits,
 * calls and exceptions), every other kept read becomes <local>N. */
static void rewrite_line(Buf* o, const char* s, const char* e, u32 keep, const Field* f) {
    while (s < e) {
        size_t len;
        int r = field_at(f, s, &len);
        if (r >= 0 && s + len + 3 <= e && strncmp(s + len, " = ", 3) == 0) {
            const char* ex = s + len + 3;
            const char* semi = memchr(ex, ';', (size_t)(e - ex));
            if (semi) {
                char tmp[48];
                if (keep >> r & 1u) {
                    snprintf(tmp, sizeof tmp, "%s%d] = %s%d = (", f->pre, r, f->local, r);
                    bputs(o, tmp);
                    put_reads(o, ex, semi, keep, f);
                    bputs(o, ")");
                } else {
                    bput(o, s, len + 3);
                    put_reads(o, ex, semi, keep, f);
                }
                s = semi;               /* the ';' is copied by the loop */
                continue;
            }
        }
        if (r >= 0 && (keep >> r & 1u)) {
            char tmp[16];
            snprintf(tmp, sizeof tmp, "%s%d", f->local, r);
            bputs(o, tmp);
            s += len;
            continue;
        }
        bput(o, s, 1);
        s++;
    }
}

static void put_cr_reload(Buf* o, u32 fields) {
    char tmp[48];
    for (int r = 0; r < 8; r++)
        if (fields >> r & 1u) {
            snprintf(tmp, sizeof tmp, " dr_c%d = ctx->crf[%d];", r, r);
            bputs(o, tmp);
        }
}

static void put_reload(Buf* o, u32 regs, int from) {
    char tmp[48];
    for (int r = from; r < 32; r++)
        if (regs >> r & 1u) {
            snprintf(tmp, sizeof tmp, " dr_g%d = ctx->gpr[%d];", r, r);
            bputs(o, tmp);
        }
}

int twin_write(FILE* out, const char* text, size_t len, u32 func_addr) {
    char sig[64], cold_sig[128], dflt[192];
    const char* p = text;
    const char* end = text + len;
    Buf fast = {0};
    u32 used = 0, cr_used = 0;
    int locals, gpr_locals, cr_locals;

    const char* cc = emit_chunk_cc();
    snprintf(sig, sizeof sig, "%svoid func_%08X(CPUState* ctx) {", cc, func_addr);
    snprintf(cold_sig, sizeof cold_sig,
             "__attribute__((noinline, cold)) %svoid func_%08X_cold(CPUState* ctx) {", cc, func_addr);
    snprintf(dflt, sizeof dflt,
             "    default: { DOLRECOMP_TWIN_MISS(ctx->pc); %svoid func_%08X_cold(CPUState* ctx); "
             "func_%08X_cold(ctx); return; }",
             cc, func_addr, func_addr);

    /* cold copy */
    {
        const char* at = strstr(text, sig);
        if (!at) {
            fprintf(stderr, "error: twin: no signature for func_%08X\n", func_addr);
            return 0;
        }
        fwrite(text, 1, (size_t)(at - text), out);
        fputs(cold_sig, out);
        fwrite(at + strlen(sig), 1, (size_t)(end - (at + strlen(sig))), out);
    }

    /* which kept registers does this chunk use; any dynamic-index write
     * other than lmw's loop disables locals for the chunk */
    for (const char* q = text; (q = strstr(q, "ctx->gpr[")) != NULL; q++) {
        size_t l;
        int r = gpr_at(q, &l);
        if (r >= 0)
            used |= 1u << r;
    }
    used &= s_regs;
    if (s_ratio) {
        u32 reads[32] = {0}, writes[32] = {0};
        for (const char* q = text; (q = strstr(q, "ctx->gpr[")) != NULL; q++) {
            size_t l;
            int r = gpr_at(q, &l);
            if (r < 0)
                continue;
            if (strncmp(q + l, " = ", 3) == 0)
                writes[r]++;
            else
                reads[r]++;
        }
        used = 0;
        for (int r = 0; r < 32; r++)
            if (reads[r] >= 20u && reads[r] >= 3u * (writes[r] ? writes[r] : 1u))
                used |= 1u << r;
        if (!used)
            used = 1u << 1;     /* as measured: a chunk with no qualifier keeps r1 */
    }
    /* the mask travels as a parameter: chunks are written on -jN worker
     * threads, and a shared static mask raced (a chunk got another's set) */
    gpr_locals = used && !strstr(text, "ctx->gpr[reg]");
    /* --twin-cr: every CR field the chunk names with a literal index. A chunk
     * that indexes the CR dynamically keeps it in ctx; so far none does -- mtcrf
     * and mcrf are emitted one literal field at a time. Nothing outside the
     * chunk text writes crf except a called chunk and the instruction fallback,
     * both reload points below; cpu_cr_get (mfcr) reads ctx, which write-through
     * keeps current. */
    if (s_cr) {
        int dynamic = 0;
        for (const char* q = text; (q = strstr(q, "ctx->crf[")) != NULL; q++) {
            size_t l;
            int r = field_at(&kCr, q, &l);
            if (r >= 0)
                cr_used |= 1u << r;
            else
                dynamic = 1;
        }
        if (dynamic)
            cr_used = 0;
    }
    cr_locals = cr_used != 0;
    if (!gpr_locals)
        used = 0;
    locals = gpr_locals || cr_locals;

    fputs("\n/* ---- fast copy ---- */\n", out);
    while (p < end) {
        const char* nl = memchr(p, '\n', (size_t)(end - p));
        const char* le = nl ? nl : end;
        size_t ll = (size_t)(le - p);
        unsigned pc;
        if (sscanf(p, "    case 0x%8Xu: goto label_", &pc) == 1 &&
            strncmp(p, "    case 0x", 11) == 0) {
            if (is_hot(pc))
                bput(&fast, p, ll + (nl ? 1 : 0));
        } else if (strncmp(p, "    default:", 12) == 0 && find_in(p, ll, "DOLRECOMP_RETURN", 16)) {
            bputs(&fast, dflt);
            bputs(&fast, "\n");
        } else if (!locals) {
            bput(&fast, p, ll + (nl ? 1 : 0));
        } else if (strncmp(p, "    DOLRECOMP_DC_LOCAL(ctx);", 28) == 0) {
            char tmp[48];
            int first = 1;
            bput(&fast, p, ll + 1);
            if (used) {
                bputs(&fast, "    u32");
                for (int r = 0; r < 32; r++)
                    if (used >> r & 1u) {
                        snprintf(tmp, sizeof tmp, "%s dr_g%d = ctx->gpr[%d]", first ? "" : ",", r, r);
                        bputs(&fast, tmp);
                        first = 0;
                    }
                bputs(&fast, ";\n");
            }
            if (cr_used) {
                first = 1;
                bputs(&fast, "    u8");
                for (int r = 0; r < 8; r++)
                    if (cr_used >> r & 1u) {
                        snprintf(tmp, sizeof tmp, "%s dr_c%d = ctx->crf[%d]", first ? "" : ",", r, r);
                        bputs(&fast, tmp);
                        first = 0;
                    }
                bputs(&fast, ";\n");
            }
        } else if (used && find_in(p, ll, "r < 32; r++, ea += 4) ctx->gpr[r] = mem_read32(ctx, ea);", 56)) {
            int from = 0;
            const char* f = find_in(p, ll, "for (u32 r = ", 13);
            if (f) from = atoi(f + 13);
            bput(&fast, p, ll);
            put_reload(&fast, used, from);
            bputs(&fast, "\n");
        } else if (find_in(p, ll, "DOLRECOMP_DC_RELOAD(ctx);", 25)) {
            bput(&fast, p, ll);
            put_reload(&fast, used, 0);
            put_cr_reload(&fast, cr_used);
            bputs(&fast, "\n");
        } else if ((used && find_in(p, ll, "ctx->gpr[", 9)) || (cr_used && find_in(p, ll, "ctx->crf[", 9))) {
            /* GPRs first, then CR fields over the result */
            Buf g = {0};
            rewrite_line(&g, p, le, used, &kGpr);
            if (cr_used) {
                rewrite_line(&fast, g.p, g.p + g.n, cr_used, &kCr);
            } else {
                bput(&fast, g.p, g.n);
            }
            free(g.p);
            bputs(&fast, "\n");
        } else {
            bput(&fast, p, ll + (nl ? 1 : 0));
        }
        p = nl ? nl + 1 : end;
    }
    fwrite(fast.p, 1, fast.n, out);
    free(fast.p);
    return 1;
}
