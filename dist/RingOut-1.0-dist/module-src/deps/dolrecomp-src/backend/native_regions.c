/* native_regions.c -- --native-regions <file>: run listed hot guest regions as native C.
 *
 * A region is a range [start, end) of guest code -- a hot loop or a whole hot function -- named in a
 * per-disc list (module-src/profiles/<ID>.regions, chosen from a fight profile of that disc). Inside it,
 * the emitted statements are copied with every guest register, FPR/PS1 and CR field it touches held in a C
 * local, and the per-instruction bookkeeping removed:
 *   - each block's `ctx->pc = X;` store is dropped (every exit sets pc itself);
 *   - the MSR.FP tests go, with ONE test at region entry (MSR cannot change inside a region: no mtmsr/rfi
 *     is accepted, and any exception leaves the region first);
 *   - RAM accesses become direct, each guarded by one range test that, when it fails, writes the locals
 *     back and resumes at that instruction's ORIGINAL label -- so an MMIO or out-of-RAM access runs exactly
 *     the code it always did;
 *   - stmw/lmw are unrolled onto the locals with one guard for the whole span;
 *   - a direct call to a small function that calls nothing (a leaf: every branch stays inside, no
 *     indirect branch) is INLINED: the callee's emitted body is copied in with its labels renamed, and its
 *     return goes straight back to the call's return label. This is most of the win: the call no longer
 *     goes through the chunk entry switch (an indirect branch) in both directions.
 * Arithmetic statements are copied verbatim, so rounding cannot drift; cycle charging and every
 * DOLRECOMP_DC exit are kept. A region with anything the pass does not recognise -- a helper call that
 * takes ctx, a variable-index register access, a branch to an fpok label outside it -- is left exactly as
 * emitted, with a note. Never a guess.
 *
 * Steam Deck, US, both arms PGO-retrained, n=6: -2.89% cycles, -0.98% instructions; frame hashes
 * identical. Output is byte-identical to the prototype (work/multi/multi_region4.py --inline
 * --no-barriers) apart from the comments naming this file.
 *
 * Runs after --native-skin, on the written chunk files. The guest disassembly comes from the chunks'
 * own `// PC: op args` comments, so nothing else needs to ship. */
#include "backend/native_regions.h"
#include "platform/fs.h"

#include <stdarg.h>
#include <stdbool.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- small containers ---- */
typedef struct { char** v; size_t n, cap; } Lines;
static void lines_push(Lines* L, char* s) {
    if (L->n == L->cap) {
        L->cap = L->cap ? L->cap * 2 : 1024;
        L->v = realloc(L->v, L->cap * sizeof(char*));
    }
    L->v[L->n++] = s;
}
static void lines_free(Lines* L) {
    for (size_t i = 0; i < L->n; i++) free(L->v[i]);
    free(L->v);
    L->v = NULL;
    L->n = L->cap = 0;
}

typedef struct { char* p; size_t n, cap; } Str;
static void str_add(Str* s, const char* t, size_t k) {
    if (s->n + k + 1 > s->cap) {
        s->cap = (s->n + k + 1) * 2;
        s->p = realloc(s->p, s->cap);
    }
    memcpy(s->p + s->n, t, k);
    s->n += k;
    s->p[s->n] = 0;
}
static void str_puts(Str* s, const char* t) { str_add(s, t, strlen(t)); }
static void str_printf(Str* s, const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (k > 0) str_add(s, buf, (size_t)k);
}
static char* str_take(Str* s) {
    if (!s->p) str_puts(s, "");
    char* p = s->p;
    s->p = NULL;
    s->n = s->cap = 0;
    return p;
}

/* ---- the region list ---- */
typedef struct { u32 start, end; bool exits; } Region; /* exits: the optional `x` column */
static Region* s_regions = NULL;
static size_t s_region_count = 0;
static bool s_enabled = false;
/* --region-psq: float psq_l/psq_st (GQR0, not w, not indexed) move straight between guest RAM and the region's
 * nF/nP locals when the helper's own fast-path test holds; anything else takes the helper as before. */
static bool s_psq_direct = false;
void native_regions_set_psq_direct(bool on) { s_psq_direct = on; }
/* --region-indirect-calls: an --indirect-calls native call inside a region stays native (see transform). Off, the
 * region drops it and exits to the dispatcher there, as before the flag. */
static bool s_region_indirect = false;
void native_regions_set_indirect_calls(bool on) { s_region_indirect = on; }

bool native_regions_load(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) { fprintf(stderr, "error: cannot read --native-regions file %s\n", path); return false; }
    char line[256];
    size_t cap = 0;
    while (fgets(line, sizeof(line), f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == 0) continue;
        unsigned a, b;
        if (sscanf(p, "%x %x", &a, &b) != 2 || b <= a || (a & 3u) || (b & 3u)) {
            fprintf(stderr, "error: bad line in %s: %s", path, line);
            fclose(f);
            return false;
        }
        if (s_region_count == cap) {
            cap = cap ? cap * 2 : 64;
            s_regions = realloc(s_regions, cap * sizeof(Region));
        }
        s_regions[s_region_count].start = a;
        s_regions[s_region_count].end = b;
        /* optional third column `x`: a direct call the pass cannot inline EXITS the region (locals written
         * back, the original code makes the call) instead of refusing the whole region. Meant for calls the
         * profile says are cold; a hot one would throw away the region on every pass. */
        {
            char x[8] = {0};
            s_regions[s_region_count].exits = sscanf(p, "%*x %*x %7s", x) == 1 && strcmp(x, "x") == 0;
        }
        s_region_count++;
    }
    fclose(f);
    s_enabled = true;
    return true;
}
unsigned native_regions_count(void) { return (unsigned)s_region_count; }
void native_regions_reset(void) {
    free(s_regions);
    s_regions = NULL;
    s_region_count = 0;
    s_enabled = false;
}

/* ---- text helpers (the prototype's regular expressions, by hand: MSVC has no regex.h) ---- */
static bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }
static bool is_word(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}
static bool is_digit(char c) { return c >= '0' && c <= '9'; }
static bool is_uhex(char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'); }
static const char* lskip(const char* s) { while (is_ws(*s)) s++; return s; }
static bool starts(const char* s, const char* p) { return strncmp(s, p, strlen(p)) == 0; }
static size_t indent_len(const char* l) { return (size_t)(lskip(l) - l); }
/* l.strip() == lit */
static bool strip_eq(const char* l, const char* lit) {
    const char* s = lskip(l);
    size_t k = strlen(lit);
    if (strncmp(s, lit, k) != 0) return false;
    return *lskip(s + k) == 0;
}
static bool hex8(const char* p, u32* v) {
    u32 x = 0;
    for (int i = 0; i < 8; i++) {
        if (!is_uhex(p[i])) return false;
        x = (x << 4) | (u32)(p[i] <= '9' ? p[i] - '0' : p[i] - 'A' + 10);
    }
    *v = x;
    return true;
}
/* re.match(r'(label|fpok)_([0-9A-F]{8}):$', l): 1 = label, 2 = fpok, 0 = no */
static int match_label(const char* l, u32* pc) {
    int kind = starts(l, "label_") ? 1 : starts(l, "fpok_") ? 2 : 0;
    if (!kind) return 0;
    const char* p = l + (kind == 1 ? 6 : 5);
    if (!hex8(p, pc) || p[8] != ':' || p[9] != 0) return 0;
    return kind;
}
/* re.match(r'\s*<pre>0x([0-9A-F]{8})u;$', l) (no trailing space allowed, as the regex) */
static bool match_set(const char* l, const char* pre, u32* v) {
    const char* p = lskip(l);
    size_t k = strlen(pre);
    if (strncmp(p, pre, k) != 0) return false;
    p += k;
    if (p[0] != '0' || p[1] != 'x' || !hex8(p + 2, v)) return false;
    return strcmp(p + 10, "u;") == 0;
}
/* re.sub(r'<pre>\[(\d+)\]', r'<to>\1', l), recording each N in set (when given) */
static char* sub_index(const char* l, const char* pre, const char* to, bool* set) {
    Str o = {0};
    size_t k = strlen(pre);
    const char* p = l;
    while (*p) {
        if (strncmp(p, pre, k) == 0 && p[k] == '[' && is_digit(p[k + 1])) {
            const char* q = p + k + 1;
            long n = 0;
            while (is_digit(*q)) { n = n * 10 + (*q - '0'); q++; }
            if (*q == ']') {
                if (set && n >= 0 && n < 32) set[n] = true;
                str_puts(&o, to);
                str_add(&o, p + k + 1, (size_t)(q - (p + k + 1)));
                p = q + 1;
                continue;
            }
        }
        str_add(&o, p, 1);
        p++;
    }
    return str_take(&o);
}
/* re.sub(r'\b<pre>(\d+)\b', r'<to>\1', l) (pre = dr_g / dr_c), recording each N in set (when given) */
static char* sub_word(const char* l, const char* pre, const char* to, bool* set) {
    Str o = {0};
    size_t k = strlen(pre);
    const char* p = l;
    while (*p) {
        if ((p == l || !is_word(p[-1])) && strncmp(p, pre, k) == 0 && is_digit(p[k])) {
            const char* q = p + k;
            long n = 0;
            while (is_digit(*q)) { n = n * 10 + (*q - '0'); q++; }
            if (!is_word(*q)) {
                if (set && n >= 0 && n < 32) set[n] = true;
                str_puts(&o, to);
                str_add(&o, p + k, (size_t)(q - (p + k)));
                p = q;
                continue;
            }
        }
        str_add(&o, p, 1);
        p++;
    }
    return str_take(&o);
}
static void collect_word(const char* l, const char* pre, bool* set) { free(sub_word(l, pre, "", set)); }
/* `nG4 = nG4 = (e);` -> `nG4 = (e);`. A write-through twin line `dr_g4 = ctx->gpr[4] = (e);` names ONE local on both
 * sides once renamed, and `x = x = e` modifies x twice with no sequence point between: undefined behaviour in C
 * (clang -Wunsequenced), even though every compiler simply stores e twice. */
static char* drop_self_assign(char* l) {
    Str o = {0};
    const char* p = l;
    bool changed = false;
    while (*p) {
        if ((p == l || !is_word(p[-1])) && p[0] == 'n') {
            const char* q = p;
            while (is_word(*q)) q++;
            size_t n = (size_t)(q - p);
            if (n >= 2 && strncmp(q, " = ", 3) == 0 && strncmp(q + 3, p, n) == 0 && !is_word(q[3 + n]) &&
                strncmp(q + 3 + n, " = ", 3) == 0) {
                str_add(&o, p, n + 3);   /* "nG4 = " */
                p = q + 6 + n;           /* past the second "nG4 = " */
                changed = true;
                continue;
            }
        }
        str_add(&o, p, 1);
        p++;
    }
    if (!changed) { free(o.p); return l; }
    free(l);
    return str_take(&o);
}
static void collect_index(const char* l, const char* pre, bool* set) { free(sub_index(l, pre, "", set)); }
static char* replace_all(const char* l, const char* from, const char* to) {
    Str o = {0};
    size_t k = strlen(from);
    const char* p = l;
    while (*p) {
        if (strncmp(p, from, k) == 0) { str_puts(&o, to); p += k; }
        else { str_add(&o, p, 1); p++; }
    }
    return str_take(&o);
}
/* ---- the guest disassembly, from the chunks' `// PC: op args` comments ---- */
typedef struct { char* op; char* args; } Insn;
static Insn* s_asm = NULL; /* indexed (pc - 0x80000000) >> 2 */
static size_t s_asm_n = 0;
static u32* s_fn = NULL; /* bl targets, sorted, unique */
static size_t s_fn_n = 0, s_fn_cap = 0;
#define ASM_BASE 0x80000000u

static const Insn* asm_at(u32 pc) {
    if (pc < ASM_BASE) return NULL;
    size_t i = (pc - ASM_BASE) >> 2;
    return i < s_asm_n && s_asm[i].op ? &s_asm[i] : NULL;
}
static void asm_line(const char* l) {
    /* "    // 80003100: mflr    r0" -> re.match(r'([0-9A-F]{8}): +(\S+)\s*(.*)', stripped) */
    if (!starts(l, "    // ")) return;
    const char* p = l + 7;
    u32 pc;
    if (!hex8(p, &pc) || p[8] != ':' || p[9] != ' ' || pc < ASM_BASE || pc >= 0x81800000u) return;
    size_t i = (pc - ASM_BASE) >> 2;
    if (i < s_asm_n && s_asm[i].op) return; /* the twin copies repeat it */
    p += 9;
    while (*p == ' ') p++;
    const char* o = p;
    while (*p && !is_ws(*p)) p++;
    if (p == o) return;
    size_t ol = (size_t)(p - o);
    p = lskip(p);
    size_t al = strlen(p);
    while (al && is_ws(p[al - 1])) al--;
    if (i >= s_asm_n) {
        size_t n = s_asm_n ? s_asm_n : 1u << 18;
        while (n <= i) n *= 2;
        s_asm = realloc(s_asm, n * sizeof(Insn));
        memset(s_asm + s_asm_n, 0, (n - s_asm_n) * sizeof(Insn));
        s_asm_n = n;
    }
    s_asm[i].op = malloc(ol + 1);
    memcpy(s_asm[i].op, o, ol);
    s_asm[i].op[ol] = 0;
    s_asm[i].args = malloc(al + 1);
    memcpy(s_asm[i].args, p, al);
    s_asm[i].args[al] = 0;
    if (strcmp(s_asm[i].op, "bl") == 0) {
        u32 t;
        if (starts(s_asm[i].args, "0x") && hex8(s_asm[i].args + 2, &t) && s_asm[i].args[10] == 0) {
            if (s_fn_n == s_fn_cap) {
                s_fn_cap = s_fn_cap ? s_fn_cap * 2 : 4096;
                s_fn = realloc(s_fn, s_fn_cap * sizeof(u32));
            }
            s_fn[s_fn_n++] = t;
        }
    }
}
static int cmp_u32(const void* a, const void* b) {
    u32 x = *(const u32*)a, y = *(const u32*)b;
    return x < y ? -1 : x > y;
}
/* bisect.bisect_right(s_fn, f) */
static size_t fn_after(u32 f) {
    size_t lo = 0, hi = s_fn_n;
    while (lo < hi) {
        size_t m = (lo + hi) / 2;
        if (s_fn[m] <= f) lo = m + 1; else hi = m;
    }
    return lo;
}
/* the trailing `0x([0-9A-F]{8})$` of a branch's operands */
static bool branch_target(const char* args, u32* t) {
    size_t k = strlen(args);
    return k >= 10 && args[k - 10] == '0' && args[k - 9] == 'x' && hex8(args + k - 8, t);
}
/* [f, end): the callee as far as it is REACHABLE from its entry -- fall-through and branch targets, each path
 * stopping at a blr -- so code after its last return (another function, reached only through a pointer) is
 * never copied in. Then every instruction in [f, end) must be simple: no call, no indirect branch, no
 * conditional return, no mtmsr/rfi, every branch inside. 0 = not a simple leaf. */
static u32 leaf_range(u32 f) { /* the callee's extent: simple (no indirect branch, nothing branches out) */
    size_t i = fn_after(f);
    u32 limit = f + 4u * 1500u;
    if (i < s_fn_n && s_fn[i] < limit) limit = s_fn[i];
    size_t span = (limit - f) / 4u;
    if (!span) return 0;
    unsigned char* seen = calloc(span, 1);
    u32* work = malloc(2 * span * sizeof(u32) + 8);
    size_t nw = 0, cap = 2 * span + 2;
    work[nw++] = f;
    u32 maxpc = 0;
    bool any_blr = false, ok = true;
    while (nw && ok) {
        u32 pc = work[--nw];
        if (pc < f || pc >= limit || (pc & 3u)) { ok = false; break; }
        size_t k = (pc - f) / 4u;
        if (seen[k]) continue;
        const Insn* in = asm_at(pc);
        if (!in) { ok = false; break; }
        seen[k] = 1;
        if (pc > maxpc) maxpc = pc;
        if (!strcmp(in->op, "blr")) { any_blr = true; continue; }
        /* a conditional return: a return point, and the fall-through is reachable. It is emitted exactly as
         * blr is (emit_dynamic_branch on LR inside the condition), so the inliner's return rewrite covers it. */
        if (!strcmp(in->op, "bclr")) {
            any_blr = true;
            if (nw < cap) work[nw++] = pc + 4;
            else { ok = false; break; }
            continue;
        }
        u32 t;
        bool has_t = branch_target(in->args, &t);
        if (!strcmp(in->op, "b")) {
            if (!has_t) { ok = false; break; }
            if (nw < cap) work[nw++] = t;
            continue;
        }
        if (!strcmp(in->op, "bc") && has_t && nw < cap) work[nw++] = t;
        if (nw < cap) work[nw++] = pc + 4;
        else { ok = false; break; }
    }
    free(seen);
    free(work);
    if (!ok || !any_blr) return 0;
    u32 end = maxpc + 4;
    for (u32 pc = f; pc < end; pc += 4) {
        const Insn* in = asm_at(pc);
        if (!in || strcmp(in->op, "?") == 0) return 0;
        const char* op = in->op;
        if (!strcmp(op, "bctrl") || !strcmp(op, "blrl") || !strcmp(op, "bctr") || !strcmp(op, "bcctr") ||
            starts(op, "rfi") || starts(op, "mtmsr"))
            return 0;
        if (starts(op, "bc") && strcmp(op, "bc") != 0 && strcmp(op, "bclr") != 0) return 0;
        /* bl is allowed: inline_calls inlines the callee's own calls recursively, or the call stays and the
         * region is refused with the reason */
        if (!strcmp(op, "b") || !strcmp(op, "bc")) {
            u32 t;
            if (branch_target(in->args, &t) && !(f <= t && t < end)) return 0;
        }
    }
    return end;
}

/* ---- the chunk files ---- */
typedef struct {
    u32 window;   /* from the file name: chunk_NNNN_textK_XXXXXXXX.c */
    char* path;
    Lines orig;   /* as written by the emitter (callee bodies are copied from here) */
    Lines work;   /* with this pass's blocks inserted; empty until the first region in the file */
    bool loaded, patched;
    bool simd;    /* --region-simd vectorised a region here: the file gets kSimdPrelude */
} Chunk;
static Chunk* s_chunks = NULL;
static size_t s_chunk_n = 0;

static bool read_lines(const char* path, Lines* L) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* text = malloc((size_t)size + 1);
    if (fread(text, 1, (size_t)size, f) != (size_t)size) { fclose(f); free(text); return false; }
    fclose(f);
    text[size] = 0;
    /* split on \n, keeping the final element (Python's str.split) */
    for (char *p = text, *q;; p = q + 1) {
        q = strchr(p, '\n');
        /* dolrecomp.exe writes the chunks with CRLF: drop the \r, or no label compare ever matches on
         * Windows (the 1.6.7 Windows build made 0 of 45 US regions and 0 skin loops native) */
        if (!q) { size_t k = strlen(p); if (k && p[k - 1] == '\r') p[k - 1] = 0; lines_push(L, strdup(p)); break; }
        *q = 0;
        if (q > p && q[-1] == '\r') q[-1] = 0;
        lines_push(L, strdup(p));
    }
    free(text);
    return true;
}
static Chunk* chunk_for(u32 pc) {
    Chunk* best = NULL;
    for (size_t i = 0; i < s_chunk_n; i++)
        if (s_chunks[i].window <= pc) best = &s_chunks[i];
    return best;
}
static bool chunk_load(Chunk* c) {
    if (c->loaded) return true;
    if (!read_lines(c->path, &c->orig)) { fprintf(stderr, "native-regions: cannot read %s\n", c->path); return false; }
    c->loaded = true;
    return true;
}
static int cmp_chunk(const void* a, const void* b) {
    u32 x = ((const Chunk*)a)->window, y = ((const Chunk*)b)->window;
    return x < y ? -1 : x > y;
}
/* index of the LAST line exactly `label_<pc>:`, or -1 */
static long last_label(const Lines* L, u32 pc) {
    char want[24];
    snprintf(want, sizeof(want), "label_%08X:", pc);
    for (size_t i = L->n; i-- > 0;)
        if (strcmp(L->v[i], want) == 0) return (long)i;
    return -1;
}

/* the callee's emitted body (its LAST copy, which is the twin hot copy where there is one): lines
 * [label_f, label_end) of the original file text. Returns false if it is not there. */
static bool callee_text(u32 f, u32 end, Lines* out) {
    Chunk* c = chunk_for(f);
    if (!c || !chunk_load(c)) return false;
    long a = last_label(&c->orig, f);
    if (a < 0) return false;
    char want[24];
    snprintf(want, sizeof(want), "label_%08X:", end);
    for (size_t i = (size_t)a; i < c->orig.n; i++)
        if (strcmp(c->orig.v[i], want) == 0) {
            for (size_t k = (size_t)a; k < i; k++) lines_push(out, strdup(c->orig.v[k]));
            return true;
        }
    return false;
}

static unsigned s_uid = 0;

/* natI<uid>_ renaming of one callee line: its own labels, and gotos to labels inside [f, end) */
static char* ren(const char* t, const char* pre, u32 f, u32 end) {
    u32 pc;
    int kind = match_label(t, &pc);
    if (kind) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s%s%08X:", pre, kind == 1 ? "" : "f", pc);
        return strdup(buf);
    }
    Str o = {0};
    const char* p = t;
    while (*p) {
        if (starts(p, "goto ")) {
            const char* q = p + 5;
            int k = starts(q, "label_") ? 1 : starts(q, "fpok_") ? 2 : 0;
            if (k) {
                const char* h = q + (k == 1 ? 6 : 5);
                if (hex8(h, &pc) && h[8] == ';') {
                    if (f <= pc && pc < end) str_printf(&o, "goto %s%s%08X;", pre, k == 1 ? "" : "f", pc);
                    else str_add(&o, p, (size_t)(h + 9 - p));
                    p = h + 9;
                    continue;
                }
            }
        }
        str_add(&o, p, 1);
        p++;
    }
    return str_take(&o);
}

/* inline every direct call in the region whose callee is a simple leaf; the rest stay as they are */
#define NREG_INLINE_DEPTH 4u      /* region -> callee -> callee's callee ... levels inlined */
#define NREG_INLINE_LINES 15000u   /* emitted lines inlined into one region, all levels (the prototype's cap) */
static size_t s_inl_lines;         /* per transform() */
static char s_refusal[160];        /* the first call left alone in this transform(), for the log */
static bool s_call_exits;          /* the region being transformed has the `x` column */
static void refuse(u32 site, u32 target, const char* reason) {
    if (!s_refusal[0]) snprintf(s_refusal, sizeof(s_refusal), "call %08X -> %08X not inlined: %s", site, target, reason);
}

static void inline_calls(char** region, size_t n, Lines* out, unsigned* n_inl, unsigned depth) {
    size_t i = 0;
    while (i < n) {
        const char* l = region[i];
        if (strip_eq(l, "if (dolrecomp_call_enter()) {") && i >= 2) {
            u32 ret, f;
            bool mlr = match_set(region[i - 2], "ctx->lr = ", &ret);
            bool mpc = match_set(region[i - 1], "ctx->pc = ", &f);
            size_t j = i + 1;
            long braces = 1;
            while (j < n && braces) {
                for (const char* p = region[j]; *p; p++) braces += (*p == '{') - (*p == '}');
                j++;
            }
            if (mlr && mpc && j < n && strip_eq(region[j], "DOLRECOMP_RETURN;")) {
                u32 end = depth < NREG_INLINE_DEPTH ? leaf_range(f) : 0;
                Lines body = {0}, raw = {0};
                bool got = end && callee_text(f, end, &raw);
                if (depth >= NREG_INLINE_DEPTH) refuse(ret - 4u, f, "too deep");
                else if (!end) refuse(ret - 4u, f, "indirect branch or branch out");
                else if (!got) refuse(ret - 4u, f, "callee text not found");
                else if (s_inl_lines + raw.n > NREG_INLINE_LINES) refuse(ret - 4u, f, "too long");
                if (got && depth < NREG_INLINE_DEPTH && s_inl_lines + raw.n <= NREG_INLINE_LINES) {
                    /* the callee's own calls first, while its labels are still label_X: their return checks
                     * name label_<ret> inside the callee, which the renaming below then maps with it */
                    s_inl_lines += raw.n;
                    inline_calls(raw.v, raw.n, &body, n_inl, depth + 1);
                }
                lines_free(&raw);
                if (!body.n && depth == 0 && s_call_exits) {
                    /* only the region's OWN calls can exit: their bl has a label in this function */
                    char buf[64];
                    snprintf(buf, sizeof(buf), "            NREG_CALL_EXIT(%08X);", ret - 4u);
                    lines_push(out, strdup(buf));
                    i = j + 1;
                    continue;
                }
                if (body.n && depth == 0) {
                    /* the call block being replaced resynced the host function's twin locals after the call
                     * (dr_gN = ctx->gpr[N] ...); keep their names so NAT_WB writes those twins back too */
                    Str tw = {0};
                    str_puts(&tw, "/*NREG_TWINS");
                    for (size_t q = i; q <= j && q < n; q++)
                        for (const char* p = strstr(region[q], "dr_"); p; p = strstr(p + 3, "dr_")) {
                            size_t m = 3;
                            while (is_word(p[m])) m++;
                            if ((p[3] == 'g' || p[3] == 'c') && m > 4 && strstr(p + m, " = ctx->") == p + m) {
                                str_puts(&tw, " ");
                                str_add(&tw, p, m);
                            }
                        }
                    str_puts(&tw, "*/");
                    lines_push(out, str_take(&tw));
                }
                if (body.n) {
                    char pre[32], buf[64];
                    snprintf(pre, sizeof(pre), "natI%u_", ++s_uid);
                    snprintf(buf, sizeof(buf), "/*NREG_INLINE_BEGIN %08X*/", f);
                    lines_push(out, strdup(buf));
                    size_t k = 0;
                    while (k < body.n) {
                        char* t = ren(body.v[k], pre, f, end);
                        bool prev_if = out->n && starts(lskip(out->v[out->n - 1]), "if (target == 0x");
                        if (strip_eq(t, "ctx->pc = target;") && k + 1 < body.n && strip_eq(body.v[k + 1], "DOLRECOMP_RETURN;") &&
                            !prev_if) {
                            size_t ind = indent_len(t);
                            Str o = {0};
                            str_add(&o, t, ind);
                            str_printf(&o, "if (target == 0x%08Xu) goto label_%08X;", ret, ret);
                            lines_push(out, str_take(&o));
                            lines_push(out, t);
                            lines_push(out, strdup(body.v[k + 1]));
                            k += 2;
                            continue;
                        }
                        lines_push(out, t);
                        k++;
                    }
                    lines_push(out, strdup("/*NREG_INLINE_END*/"));
                    lines_free(&body);
                    (*n_inl)++;
                    i = j + 1;
                    continue;
                }
                lines_free(&body);
            }
        }
        lines_push(out, strdup(l));
        i++;
    }
}

static const char* const kSafe[] = {"mem_read", "mem_write", "dolrecomp_fprf_s(ctx", "ppc_fprf_flush(ctx", "ppc_fprf_drop(",
                                    "ppc_fp_available(ctx", "ppc_psq_load(ctx", "ppc_psq_store(ctx",
                                    /* touch only FPSCR / the CR fields, neither of which a region keeps in locals */
                                    "ppc_fma(ctx", "ppc_fctiw(ctx",
                                    /* the double twin of dolrecomp_fprf_s: records the result for lazy FPRF (or
                                     * nothing, under RECOMP_NO_FPRF) -- FPSCR and the g_fprf_* globals only */
                                    "dolrecomp_fprf_d(ctx",
                                    /* the reciprocal estimates (cpu.c): operands by value, results through pointers
                                     * to C locals, and they write FPSCR only -- set_fp_exception raises nothing,
                                     * it sets FPSCR bits and recomputes FX/VX/FEX */
                                    "ppc_fres(ctx", "ppc_frsqrte(ctx", "ppc_ps_res(ctx", "ppc_ps_rsqrte(ctx"};
static bool any_safe(const char* l) {
    for (size_t k = 0; k < sizeof(kSafe) / sizeof(kSafe[0]); k++)
        if (strstr(l, kSafe[k])) return true;
    return false;
}
/* re.search(r'\b\w+\(ctx\s*[,)]', l): a call that takes ctx (not "(ctx->x") */
static bool has_ctx_call(const char* l) {
    for (const char* p = strstr(l, "(ctx"); p; p = strstr(p + 1, "(ctx")) {
        if (p == l || !is_word(p[-1])) continue;
        const char* q = lskip(p + 4);
        if (*q == ',' || *q == ')') return true;
    }
    return false;
}
/* re.search(r'\bppc_[a-z_0-9]+\(', l) */
static bool has_ppc_helper(const char* l) {
    for (const char* p = strstr(l, "ppc_"); p; p = strstr(p + 1, "ppc_")) {
        if (p != l && is_word(p[-1])) continue;
        const char* q = p + 4;
        const char* s = q;
        while ((*q >= 'a' && *q <= 'z') || *q == '_' || is_digit(*q)) q++;
        if (q > s && *q == '(') return true;
    }
    return false;
}
/* re.search(r'\bmem_(read|write)(8|16|32|64)\(ctx, ea', l): the access size in bytes, 0 = none */
static unsigned mem_access(const char* l) {
    for (const char* p = strstr(l, "mem_"); p; p = strstr(p + 1, "mem_")) {
        if (p != l && is_word(p[-1])) continue;
        const char* q = p + 4;
        if (starts(q, "read")) q += 4;
        else if (starts(q, "write")) q += 5;
        else continue;
        unsigned sz = starts(q, "8(") ? 1 : starts(q, "16(") ? 2 : starts(q, "32(") ? 4 : starts(q, "64(") ? 8 : 0;
        if (!sz) continue;
        q += sz == 1 ? 1 : 2;
        if (starts(q, "(ctx, ea")) return sz;
    }
    return 0;
}
/* mem_readN(ctx, ea) -> NREG_RDN(ea); mem_writeN(ctx, ea, -> NREG_WRN(ea,  (word-bounded, as the regex) */
static char* sub_mem(const char* l) {
    static const char* const W[4] = {"8", "16", "32", "64"};
    Str o = {0};
    const char* p = l;
    while (*p) {
        if ((p == l || !is_word(p[-1])) && starts(p, "mem_")) {
            bool done = false;
            for (int k = 0; k < 4 && !done; k++) {
                char rd[32], wr[32];
                snprintf(rd, sizeof(rd), "mem_read%s(ctx, ea)", W[k]);
                snprintf(wr, sizeof(wr), "mem_write%s(ctx, ea, ", W[k]);
                if (starts(p, rd)) { str_printf(&o, "NREG_RD%s(ea)", W[k]); p += strlen(rd); done = true; }
                else if (starts(p, wr)) { str_printf(&o, "NREG_WR%s(ea, ", W[k]); p += strlen(wr); done = true; }
            }
            if (done) continue;
        }
        str_add(&o, p, 1);
        p++;
    }
    return str_take(&o);
}

/* the `for (u32 r = N; r < 32; r++, ea += 4) ...` loop of stmw/lmw: N, and whether it is the load */
static bool match_mw(const char* l, unsigned* N, bool* load) {
    const char* p = strstr(l, "for (u32 r = ");
    for (; p; p = strstr(p + 1, "for (u32 r = ")) {
        const char* q = p + 13;
        if (!is_digit(*q)) continue;
        unsigned n = 0;
        while (is_digit(*q)) n = n * 10 + (unsigned)(*q++ - '0');
        if (!starts(q, "; r < 32; r++, ea += 4) ")) continue;
        q += 24;
        if (starts(q, "ctx->gpr[r] = mem_read32(ctx, ea);")) { *N = n; *load = true; return true; }
        if (starts(q, "mem_write32(ctx, ea, ctx->gpr[r]);")) { *N = n; *load = false; return true; }
    }
    return false;
}

/* in_list(labels, pc) */
static bool has_pc(const u32* v, size_t n, u32 pc) {
    for (size_t i = 0; i < n; i++)
        if (v[i] == pc) return true;
    return false;
}

/* rewrite every `goto (label|fpok)_X;`. false = a goto to an fpok label outside the region */
static char* fix_gotos(const char* l, const u32* labels, size_t nl, bool* bad) {
    Str o = {0};
    const char* p = l;
    while (*p) {
        if (starts(p, "goto ")) {
            const char* q = p + 5;
            int k = starts(q, "label_") ? 1 : starts(q, "fpok_") ? 2 : 0;
            u32 pc;
            if (k) {
                const char* h = q + (k == 1 ? 6 : 5);
                if (hex8(h, &pc) && h[8] == ';') {
                    if (has_pc(labels, nl, pc)) str_printf(&o, "goto nat%s_%08X;", k == 1 ? "" : "f", pc);
                    else if (k == 2) { *bad = true; str_add(&o, p, (size_t)(h + 9 - p)); }
                    else str_printf(&o, "{ NAT_WB(); goto label_%08X; }", pc);
                    p = h + 9;
                    continue;
                }
            }
        }
        str_add(&o, p, 1);
        p++;
    }
    return str_take(&o);
}

/* The native block for one region whose start label is line a of src (end label at b). NULL = left as
 * emitted, with the reason in *why. */
/* "DOLRECOMP_DC += n; " when an exit lands on the head of a block whose charge n the region already paid */
static const char* bail_refund(u32 inst_pc, u32 block_pc, u32 block_dc) {
    static char buf[48];
    if (inst_pc != block_pc || !block_dc) return "";
    snprintf(buf, sizeof(buf), "DOLRECOMP_DC += %u; ", block_dc);
    return buf;
}

/* resume/n_resume: PCs strictly inside the span that the hot function's entry switch can resume at (a
 * downcount exit sets pc to a loop head). Each one that is a label of this region gets an entry stub, and
 * stubbed[i] is set so the caller can point the switch case at it. */
static char* transform(char** src, size_t a, size_t b, u32 start, u32 end, const char** why, unsigned* n_inl_out,
                       const u32* resume, size_t n_resume, bool* stubbed, bool call_exits) {
    Lines region = {0};
    unsigned n_inl = 0;
    s_inl_lines = 0;
    s_refusal[0] = 0;
    s_call_exits = call_exits;
    inline_calls(src + a, b - a, &region, &n_inl, 0);
    *n_inl_out = n_inl;

    bool DRG[32] = {0}, DRC[32] = {0}, G[32] = {0}, F[32] = {0}, C[32] = {0};
    int depth = 0;
    for (size_t i = 0; i < region.n; i++) {
        const char* l = region.v[i];
        if (starts(l, "/*NREG_INLINE_BEGIN")) depth++;
        else if (starts(l, "/*NREG_INLINE_END")) depth--;
        else if (!depth) { collect_word(l, "dr_g", DRG); collect_word(l, "dr_c", DRC); }
    }
    u32* labels = malloc((region.n + 1) * sizeof(u32));
    size_t nl = 0;
    for (size_t i = 0; i < region.n; i++) {
        u32 pc;
        if (match_label(region.v[i], &pc)) labels[nl++] = pc;
    }

    Lines out = {0};
    bool fp = false, have_block = false, have_inst = false, prev_label = false;
    u32 block_pc = 0, inst_pc = 0, block_dc = 0;
    bool after_pc = false;
    bool psq_open = false;
    int inl = 0;
    char* result = NULL;
    *why = NULL;
    for (size_t i = 0; i < region.n && !*why; i++) {
        if (psq_open && !strstr(region.v[i], "ctx->exception")) { *why = "psq helper without its exception check"; break; }
        char* l = strdup(region.v[i]);
        const char* s = lskip(l);
        /* --indirect-calls: with --region-indirect-calls, make the native call the original code makes here, the
           same way. Dropping it (the exit after it: write back, return) is not equivalent: going back to the
           dispatcher moves when CoreTiming events run, and with that the game -- region 80037BBC alone changed
           both US hashes. Without the flag the line is still dropped, so a disc whose profile was trained on
           that keeps its exact code. The locals are written back first, since the callee runs on ctx. Its
           return lands on the region's resume entry when there is one (NAT_RL reloads every local), else on
           the original code; a call that comes back anywhere else returns WITHOUT a second write-back, ctx
           being newer than the locals. */
        if (starts(s, "DOLRECOMP_INDIRECT_CALL(")) {
            if (!s_region_indirect) { free(l); continue; }
            const char* a0 = s + 24;
            const char* c1 = strchr(a0, ',');
            const char* r = c1 ? lskip(c1 + 1) : NULL;
            u32 ret;
            if (!r || !starts(r, "0x") || !hex8(r + 2, &ret)) { *why = "unparsed indirect call"; free(l); break; }
            bool resumes = has_pc(labels, nl, ret) && has_pc(resume, n_resume, ret);
            Str o = {0};
            str_printf(&o, "%.*s{ NAT_WB(); DOLRECOMP_INDIRECT_CALL(%.*s, 0x%08Xu, %s_%08X); DOLRECOMP_RETURN; }",
                       (int)indent_len(l), l, (int)(c1 - a0), a0, ret, resumes ? "nreg_e" : "label", ret);
            lines_push(&out, str_take(&o));
            free(l);
            continue;
        }
        if (starts(l, "/*NREG_INLINE_BEGIN")) { inl++; lines_push(&out, l); continue; }
        if (starts(l, "/*NREG_INLINE_END")) { inl--; lines_push(&out, l); continue; }
        u32 pc;
        int kind = match_label(l, &pc);
        if (kind) {
            char buf[32];
            snprintf(buf, sizeof(buf), "nat%s_%08X:", kind == 1 ? "" : "f", pc);
            lines_push(&out, strdup(buf));
            prev_label = kind == 1;
            free(l);
            continue;
        }
        if (prev_label && match_set(s, "ctx->pc = ", &pc) && s == l + indent_len(l)) {
            /* re.match(r'ctx->pc = 0x...u;$', stripped): the block's pc store */
            block_pc = pc;
            have_block = true;
            prev_label = false;
            after_pc = true;
            block_dc = 0;
            free(l);
            continue;
        }
        prev_label = false;
        /* the block's charge, given back by any exit that lands on this block's head (whose original code
         * charges it again): `DOLRECOMP_DC -= n;` right after the pc store */
        if (after_pc && !inl) {
            unsigned dcn;
            if (sscanf(s, "DOLRECOMP_DC -= %u;", &dcn) == 1) block_dc = dcn;
            after_pc = false;
        }
        /* an exit to the region's own first instruction must skip the region block, which sits right at
         * label_START (it would re-enter and exit again forever); a block-head exit refunds the charge */
        if (strstr(s, "NREG_CALL_EXIT(")) {
            char pre[48] = "";
            if (inst_pc == block_pc && block_dc) snprintf(pre, sizeof(pre), "DOLRECOMP_DC += %u; ", block_dc);
            char from[40], to[96];
            snprintf(from, sizeof(from), "NREG_CALL_EXIT(%08X);", inst_pc);
            snprintf(to, sizeof(to), "%s%s(%08X);", pre, inst_pc == start ? "NREG_CALL_EXIT_START" : "NREG_CALL_EXIT", inst_pc);
            char* u = replace_all(l, from, to);
            free(l);
            l = u;
            s = lskip(l);
        }
        if (starts(s, "// ") && hex8(s + 3, &pc) && s[11] == ':' && s[12] == ' ') { inst_pc = pc; have_inst = true; }
        /* 'mtmsr' in s or 'rfi' in s.split(':')[-1][:6] */
        {
            const char* c = strrchr(s, ':');
            const char* tail = c ? c + 1 : s;
            char six[7] = {0};
            strncpy(six, tail, 6);
            if (strstr(s, "mtmsr") || strstr(six, "rfi")) { *why = "mtmsr/rfi"; free(l); break; }
        }
        if (strstr(s, "ppc_fp_available(ctx")) { fp = true; free(l); continue; }
        bool bad = false;
        char* t = fix_gotos(l, labels, nl, &bad);
        free(l);
        l = t;
        if (bad) { *why = "goto fpok out of region"; free(l); break; }
        if (strstr(l, "DOLRECOMP_RETURN;")) { t = replace_all(l, "DOLRECOMP_RETURN;", "{ NAT_WB(); DOLRECOMP_RETURN; }"); free(l); l = t; }
        size_t ind = indent_len(l);
        unsigned N;
        bool load;
        if (match_mw(l, &N, &load)) {
            if (!have_block || !have_inst) { *why = "stmw/lmw without pc"; free(l); break; }
            Str o = {0};
            str_add(&o, l, ind);
            if (!inl)
                str_printf(&o, "if (!nreg_in_ram(ea, %uu)) { NAT_WB(); ctx->pc = 0x%08Xu; %sgoto %s_%08X; } ", 4u * (32u - N), block_pc,
                           bail_refund(inst_pc, block_pc, block_dc), inst_pc == start ? "nreg_o" : "label", inst_pc);
            for (unsigned r = N, k = 0; r < 32; r++, k++) {
                char e[24];
                if (k == 0) snprintf(e, sizeof(e), "ea");
                else snprintf(e, sizeof(e), "(ea + %uu)", 4u * k);
                G[r] = true;
                if (k) str_puts(&o, " ");
                if (load) str_printf(&o, inl ? "nG%u = mem_read32(ctx, %s);" : "nG%u = NREG_RD32(%s);", r, e);
                else str_printf(&o, inl ? "mem_write32(ctx, %s, nG%u);" : "NREG_WR32(%s, nG%u);", e, r);
            }
            lines_push(&out, str_take(&o));
            free(l);
            continue;
        }
        if (strstr(l, "ctx->gpr[r]")) { *why = "unhandled variable-index gpr access"; free(l); break; }
        unsigned sz = mem_access(l);
        if (sz && !inl) {
            if (!have_block || !have_inst) { *why = "mem access without pc"; free(l); break; }
            char* m = sub_mem(l);
            Str o = {0};
            str_add(&o, l, ind);
            str_printf(&o, "if (!nreg_in_ram(ea, %uu)) { NAT_WB(); ctx->pc = 0x%08Xu; %sgoto %s_%08X; } ", sz, block_pc,
                       bail_refund(inst_pc, block_pc, block_dc), inst_pc == start ? "nreg_o" : "label", inst_pc);
            str_puts(&o, lskip(m));
            free(m);
            free(l);
            l = str_take(&o);
            ind = indent_len(l);
        }
        /* psq_l / psq_st: the helper stays, on ctx, with just its register synced */
        {
            const char* pl = strstr(l, "ppc_psq_load(ctx, ");
            const char* ps = strstr(l, "ppc_psq_store(ctx, ");
            const char* pq = pl && (!ps || pl < ps) ? pl : ps;
            if (pq) {
                const char* q = pq + (pq == pl ? 18 : 19);
                if (is_digit(*q)) {
                    unsigned n = 0;
                    while (is_digit(*q)) n = n * 10 + (unsigned)(*q++ - '0');
                    if (starts(q, "u, ") && n < 32) {
                        F[n] = true;
                        char* l2 = sub_index(l, "ctx->gpr", "nG", G);
                        char buf[192];
                        Str o = {0};
                        str_add(&o, l, ind);
                        /* --region-psq: only the exact float form; the helper call becomes the else branch, closed
                         * after the exception check that follows it */
                        if (s_psq_direct && starts(q, "u, ea, false, 0u, false, 0x")) {
                            Str f = {0};
                            str_add(&f, l, ind);
                            if (pq == pl)
                                snprintf(buf, sizeof(buf), "if (__builtin_expect(nreg_q0_ok(ctx, ea), 1)) NREG_PSQ_L2(ea, nF%u, nP%u); else {", n, n);
                            else
                                snprintf(buf, sizeof(buf), "if (__builtin_expect(nreg_q0_ok(ctx, ea), 1)) { u8* h = NREG_H(ea); dolrecomp_clear_reservation(ctx, ea); "
                                         "dolrecomp_journal_cached(ctx, h, 8u); NREG_PSQ_S2(h, nF%u, nP%u); } else {", n, n);
                            str_puts(&f, buf);
                            lines_push(&out, str_take(&f));
                            psq_open = true;
                        }
                        if (pq == pl) {
                            snprintf(buf, sizeof(buf), "nF%u = ctx->fpr[%u]; nP%u = ctx->ps1[%u];", n, n, n, n);
                            str_puts(&o, buf);
                            lines_push(&out, l2);
                            lines_push(&out, str_take(&o));
                        } else {
                            snprintf(buf, sizeof(buf), "ctx->fpr[%u] = nF%u; ctx->ps1[%u] = nP%u;", n, n, n, n);
                            str_puts(&o, buf);
                            lines_push(&out, str_take(&o));
                            lines_push(&out, l2);
                        }
                        free(l);
                        continue;
                    }
                }
            }
        }
        /* mfcr: cpu_cr_get(ctx) packs ctx->crf[], which is stale while the region holds CR fields in nC
         * locals. Spell it out field by field in cpu_cr_get's own order (cpu.h); the renaming below then reads
         * every field from its local. (mtcrf is emitted as plain ctx->crf[i] stores and needs nothing.) */
        if (strstr(l, "cpu_cr_get(ctx)")) {
            char* u = replace_all(l, "cpu_cr_get(ctx)",
                                  "(((u32)(ctx->crf[0] & 0xFu) << 28) | ((u32)(ctx->crf[1] & 0xFu) << 24) | "
                                  "((u32)(ctx->crf[2] & 0xFu) << 20) | ((u32)(ctx->crf[3] & 0xFu) << 16) | "
                                  "((u32)(ctx->crf[4] & 0xFu) << 12) | ((u32)(ctx->crf[5] & 0xFu) << 8) | "
                                  "((u32)(ctx->crf[6] & 0xFu) << 4) | ((u32)(ctx->crf[7] & 0xFu) << 0))");
            free(l);
            l = u;
        }
        if (has_ctx_call(l) && !any_safe(l)) { *why = s_refusal[0] ? s_refusal : "contains a call (barrier)"; free(l); break; }
        if (has_ppc_helper(l) && !any_safe(l)) { *why = "unknown ppc_ helper"; free(l); break; }
        collect_index(l, "ctx->gpr", G);
        collect_index(l, "ctx->fpr", F);
        collect_index(l, "ctx->ps1", F);
        collect_word(l, "dr_g", G);
        collect_word(l, "dr_c", C);
        collect_index(l, "ctx->crf", C);
        char* u;
        u = sub_index(l, "ctx->gpr", "nG", NULL); free(l); l = u;
        u = sub_index(l, "ctx->fpr", "nF", NULL); free(l); l = u;
        u = sub_index(l, "ctx->ps1", "nP", NULL); free(l); l = u;
        u = sub_word(l, "dr_g", "nG", NULL); free(l); l = u;
        u = sub_word(l, "dr_c", "nC", NULL); free(l); l = u;
        u = sub_index(l, "ctx->crf", "nC", NULL); free(l); l = u;
        l = drop_self_assign(l);
        lines_push(&out, l);
        if (psq_open && strstr(l, "ctx->exception")) {
            Str c = {0};
            str_add(&c, l, ind);
            str_puts(&c, "}");
            lines_push(&out, str_take(&c));
            psq_open = false;
        }
    }
    if (!*why && psq_open) *why = "psq helper without its exception check";
    if (!*why) {
        Str o = {0};
        str_printf(&o, "    /* NATIVE REGION %08X..%08X (native_regions.c) */\n", start, end - 4);
        str_printf(&o, "    if (%s) {\n        ", fp ? "(ctx->msr & 0x00002000u)" : "1");
        /* decl / wb / rl: three space-joined groups each, exactly as the prototype joins them */
        bool first = true;
        for (int r = 0; r < 32; r++) if (G[r]) { str_printf(&o, "%su32 nG%d = ctx->gpr[%d];", first ? "" : " ", r, r); first = false; }
        str_puts(&o, " ");
        first = true;
        for (int r = 0; r < 32; r++) if (F[r]) { str_printf(&o, "%sf64 nF%d = ctx->fpr[%d], nP%d = ctx->ps1[%d];", first ? "" : " ", r, r, r, r); first = false; }
        str_puts(&o, " ");
        first = true;
        for (int r = 0; r < 32; r++) if (C[r]) { str_printf(&o, "%su8 nC%d = ctx->crf[%d];", first ? "" : " ", r, r); first = false; }
        str_puts(&o, "\n#define NAT_WB() do { ");
        first = true;
        for (int r = 0; r < 32; r++) if (G[r]) { str_printf(&o, "%sctx->gpr[%d] = nG%d;", first ? "" : " ", r, r); first = false; }
        str_puts(&o, " ");
        first = true;
        for (int r = 0; r < 32; r++) if (F[r]) { str_printf(&o, "%sctx->fpr[%d] = nF%d; ctx->ps1[%d] = nP%d;", first ? "" : " ", r, r, r, r); first = false; }
        str_puts(&o, " ");
        first = true;
        for (int r = 0; r < 32; r++) if (C[r]) { str_printf(&o, "%sctx->crf[%d] = nC%d;", first ? "" : " ", r, r); first = false; }
        str_puts(&o, " ");
        first = true;
        for (int r = 0; r < 32; r++) if (DRG[r]) { str_printf(&o, "%sdr_g%d = nG%d;", first ? "" : " ", r, r); first = false; }
        str_puts(&o, " ");
        first = true;
        for (int r = 0; r < 32; r++) if (DRC[r]) { str_printf(&o, "%sdr_c%d = nC%d;", first ? "" : " ", r, r); first = false; }
        str_puts(&o, " } while (0)\n#define NAT_RL() do { ");
        first = true;
        for (int r = 0; r < 32; r++) if (G[r]) { str_printf(&o, "%snG%d = ctx->gpr[%d];", first ? "" : " ", r, r); first = false; }
        str_puts(&o, " ");
        first = true;
        for (int r = 0; r < 32; r++) if (F[r]) { str_printf(&o, "%snF%d = ctx->fpr[%d]; nP%d = ctx->ps1[%d];", first ? "" : " ", r, r, r, r); first = false; }
        str_puts(&o, " ");
        first = true;
        for (int r = 0; r < 32; r++) if (C[r]) { str_printf(&o, "%snC%d = ctx->crf[%d];", first ? "" : " ", r, r); first = false; }
        str_printf(&o, " } while (0)\n        goto nat_%08X;", start);
        /* Resume entries. The switch jumps here from outside this block, past the locals' initialisers, which
         * C allows; NAT_RL() then assigns every one of them from ctx, which is current at function entry. An
         * FP region re-checks MSR.FP first and otherwise takes the original code, which raises the
         * FP-unavailable exception at the right instruction. */
        for (size_t i = 0; i < n_resume; i++) {
            if (!has_pc(labels, nl, resume[i])) continue;
            if (fp)
                str_printf(&o, "\nnreg_e_%08X: if (!(ctx->msr & 0x00002000u)) goto label_%08X; NAT_RL(); goto nat_%08X;",
                           resume[i], resume[i], resume[i]);
            else
                str_printf(&o, "\nnreg_e_%08X: NAT_RL(); goto nat_%08X;", resume[i], resume[i]);
            stubbed[i] = true;
        }
        for (size_t i = 0; i < out.n; i++) { str_puts(&o, "\n"); str_puts(&o, out.v[i]); }
        str_printf(&o, "\n        NAT_WB(); goto label_%08X; /* falls out of the region */\n#undef NAT_WB\n#undef NAT_RL\n    }\n"
                   "nreg_o_%08X: __attribute__((unused)); /* the original code, for exits at the region's first instruction */",
                   end, start);
        result = str_take(&o);
    }
    lines_free(&out);
    lines_free(&region);
    free(labels);
    return result;
}

static const char kHelpers[] =
    "/* ---- NATIVE REGION helpers (native_regions.c) ---- */\n"
    "#define NREG_FN static inline __attribute__((always_inline, no_profile_instrument_function))\n"
    "NREG_FN bool nreg_in_ram(u32 ea, u32 size) { return (u32)(ea - GC_RAM_BASE) <= GC_MAIN_RAM_SIZE - size; }\n"
    "#define NREG_H(ea_) (DOLRECOMP_RAM_ARG(ctx) + ((ea_) - GC_RAM_BASE))\n"
    "#define NREG_RD8(ea_) (*NREG_H(ea_))\n"
    "#define NREG_RD16(ea_) read_be16(NREG_H(ea_))\n"
    "#define NREG_RD32(ea_) read_be32(NREG_H(ea_))\n"
    "#define NREG_RD64(ea_) read_be64(NREG_H(ea_))\n"
    "#define NREG_WR_(ea_, n, stmt) do { u32 nreg_ea = (ea_); u8* h = NREG_H(nreg_ea); \\\n"
    "    dolrecomp_clear_reservation(ctx, nreg_ea); dolrecomp_journal_cached(ctx, h, n); stmt; } while (0)\n"
    "#define NREG_WR8(ea_, v) NREG_WR_(ea_, 1u, *h = (u8)(v))\n"
    "#define NREG_WR16(ea_, v) NREG_WR_(ea_, 2u, write_be16(h, (u16)(v)))\n"
    "#define NREG_WR32(ea_, v) NREG_WR_(ea_, 4u, write_be32(h, (u32)(v)))\n"
    "#define NREG_WR64(ea_, v) NREG_WR_(ea_, 8u, write_be64(h, (u64)(v)))\n"
    "#define NREG_CALL_EXIT(pc) do { NAT_WB(); goto label_##pc; } while (0)\n"
    "#define NREG_CALL_EXIT_START(pc) do { NAT_WB(); goto nreg_o_##pc; } while (0)\n"
    "/* ---- end NATIVE REGION helpers ---- */\n";

static const char kPsqHelpers[] =
    "/* ---- --region-psq helpers (native_regions.c): the float fast path of dolrecomp_psq_*_inline_r ---- */\n"
    "NREG_FN bool nreg_q0_ok(const CPUState* c, u32 ea) {\n"
    "    return (c->gqr[0] & 0x00070007u) == 0u && (ea & 3u) == 0u &&\n"
    "           (c->hid2 & (PPC_HID2_PSE | PPC_HID2_LSQE)) == (PPC_HID2_PSE | PPC_HID2_LSQE) && nreg_in_ram(ea, 8u);\n"
    "}\n"
    "#if defined(DOLRECOMP_PSQ_SIMD_ON)\n"
    "#define NREG_PSQ_L2(ea_, a, b) dolrecomp_psq_load2(NREG_H(ea_), &(a), &(b))\n"
    "#define NREG_PSQ_S2(h, a, b) dolrecomp_psq_store2((h), (a), (b))\n"
    "#else\n"
    "#define NREG_PSQ_L2(ea_, a, b) do { const u8* h = NREG_H(ea_); (a) = dolrecomp_psq_single_value(read_be32(h)); \\\n"
    "    (b) = dolrecomp_psq_single_value(read_be32(h + 4)); } while (0)\n"
    "#define NREG_PSQ_S2(h, a, b) do { write_be32((h), dolrecomp_psq_single_bits(a)); write_be32((h) + 4, dolrecomp_psq_single_bits(b)); } while (0)\n"
    "#endif\n"
    "/* ---- end --region-psq helpers ---- */\n";

/* ---- --region-simd: two-lane paired singles inside native regions ----
 * In a region with at least kSimdMinOps paired-single ops, each FP local pair (f64 nFk, nPk) becomes ONE vector nVk
 * and the scalar names become lane macros (#define nFk (nVk[0])), so every line left alone keeps its exact per-lane
 * meaning. Then the op pairs that have a two-lane form with the SAME arithmetic become one vector statement: the
 * rounded ps_add/sub/mul/div forms, ps_mul with force25, the ps_madd family (still two statements, so nothing can be
 * contracted into an FMA), ps_muls0/1, ps_madds0/1, ps_sel, ps_mr, ps_merge00..11, and the direct psq_l (into
 * temporaries: a vector lane has no address). Everything else (ps_sum0/1, ps_neg/abs, compares, scalar FP, FPRF,
 * exits) stays as it was, through the lane macros. The module builds with -ffp-contract=off and packed conversions
 * obey the same MXCSR, so the lanes are bit-identical: the US frame hashes do not move. Measured on the Steam Deck
 * (US, both arms PGO-retrained, same work, n=8 twice): -0.98% and -1.17% cycles. A rewrite of each finished region
 * block, byte-identical to the measured prototype (regsimd.py --min 8). */
static bool s_region_simd = false;
void native_regions_set_simd(bool on) { s_region_simd = on; }
enum { kSimdMinOps = 8 };
static unsigned s_simd_regions, s_simd_pairs, s_simd_madd, s_simd_s01, s_simd_psql;

static const char kSimdPrelude[] =
    "/* ---- --region-simd: two-lane paired singles in native regions (native_regions.c) ---- */\n"
    "typedef double nrv_v2d __attribute__((vector_size(16)));\n"
    "typedef float nrv_v2f __attribute__((vector_size(8)));\n"
    "typedef u64 nrv_v2u __attribute__((vector_size(16)));\n"
    "typedef s32 nrv_v2i __attribute__((vector_size(8)));\n"
    "typedef long long nrv_v2l __attribute__((vector_size(16)));\n"
    "NREG_FN nrv_v2f nrv_f(nrv_v2d a) { return __builtin_convertvector(a, nrv_v2f); }\n"
    "NREG_FN nrv_v2d nrv_d(nrv_v2f a) { return __builtin_convertvector(a, nrv_v2d); }\n"
    "NREG_FN nrv_v2d nrv_round(nrv_v2d a) { return nrv_d(nrv_f(a)); }\n"
    "NREG_FN nrv_v2d nrv_force25(nrv_v2d a) {\n"
    "    nrv_v2u b = (nrv_v2u)a;\n"
    "    return (nrv_v2d)((b & 0xFFFFFFFFF8000000ull) + (b & 0x0000000008000000ull));\n"
    "}\n"
    "NREG_FN nrv_v2d nrv_sel(nrv_v2d a, nrv_v2d c, nrv_v2d b) {   /* per lane: (f32)a >= 0 ? c : b */\n"
    "    nrv_v2l m = __builtin_convertvector(nrv_f(a) >= 0.0f, nrv_v2l);\n"
    "    return (nrv_v2d)(((nrv_v2u)c & (nrv_v2u)m) | ((nrv_v2u)b & ~(nrv_v2u)m));\n"
    "}\n"
    "#define NRV_S0(v) __builtin_shufflevector((v), (v), 0, 0)\n"
    "#define NRV_S1(v) __builtin_shufflevector((v), (v), 1, 1)\n"
    "/* ---- end --region-simd ---- */\n";

/* Match at s: literal bytes, except \1 = indentation (spaces/tabs, possibly none; its length into *ind), \2 = a decimal
 * number (into n[] in order), \3 = one of - + * / and \4 = F or P (both into c[] in order). The end of the match, or
 * NULL. Pattern literals must not put an octal digit right after an escape. */
static const char* pmatch_at(const char* s, const char* pat, size_t* ind, unsigned* n, char* c) {
    unsigned kn = 0, kc = 0;
    for (const char* p = pat; *p; p++) {
        if (*p == '\1') {
            const char* t = s;
            while (*s == ' ' || *s == '\t') s++;
            if (ind) *ind = (size_t)(s - t);
        } else if (*p == '\2') {
            if (!is_digit(*s)) return NULL;
            unsigned v = 0;
            while (is_digit(*s)) v = v * 10u + (unsigned)(*s++ - '0');
            n[kn++] = v;
        } else if (*p == '\3' || *p == '\4') {
            if (!*s || !strchr(*p == '\3' ? "-+*/" : "FP", *s)) return NULL;
            c[kc++] = *s++;
        } else if (*s != *p) {
            return NULL;
        } else {
            s++;
        }
    }
    return s;
}
/* the same, for the whole line */
static bool pmatch(const char* l, const char* pat, size_t* ind, unsigned* n, char* c) {
    const char* e = pmatch_at(l, pat, ind, n, c);
    return e && !*e;
}
static bool line_is(const char* l, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return k > 0 && (size_t)k < sizeof(buf) && strcmp(l, buf) == 0;
}
static bool strip_is(const char* l, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return k > 0 && (size_t)k < sizeof(buf) && strip_eq(l, buf);
}
static void push_fmt(Lines* L, const char* fmt, ...) {
    Str o = {0};
    va_list ap;
    va_start(ap, fmt);
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    str_puts(&o, buf);
    lines_push(L, str_take(&o));
}

/* `<ws>// XXXXXXXX: ps_` */
static bool is_ps_op(const char* l) {
    const char* s = lskip(l);
    if (!starts(s, "// ")) return false;
    for (int i = 3; i < 11; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'A' && s[i] <= 'F'))) return false;
    return starts(s + 11, ": ps_");
}
/* "f64 nFk = ctx->fpr[k], nPk = ctx->ps1[k];" at p (after a non-word character): its end, and k when all four agree */
static const char* fp_decl_at(const char* l, const char* p, unsigned* k, bool* same) {
    if (p > l && is_word(p[-1])) return NULL;
    unsigned n[4];
    const char* e = pmatch_at(p, "f64 nF\2 = ctx->fpr[\2], nP\2 = ctx->ps1[\2];", NULL, n, NULL);
    if (!e) return NULL;
    *k = n[0];
    *same = n[1] == n[0] && n[2] == n[0] && n[3] == n[0];
    return e;
}

/* ps_madd/msub/nmadd/nmsub: the scalar code's two-lane statement group at L[i] -> lines consumed (0 = not one) */
static size_t simd_madd(char** L, size_t n, size_t i, Lines* out) {
    size_t ind;
    unsigned v[2], b[1], d[1];
    char op[1];
    if (i + 5 >= n || !pmatch(L[i], "\1f32 ps0 = (f32)nF\2 * (f32)nF\2;", &ind, v, NULL)) return 0;
    if (!strip_is(L[i + 1], "f32 ps1 = (f32)nP%u * (f32)nP%u;", v[0], v[1])) return 0;
    if (!pmatch(L[i + 2], "\1ps0 \3= (f32)nF\2;", NULL, b, op) || (op[0] != '-' && op[0] != '+')) return 0;
    if (!strip_is(L[i + 3], "ps1 %c= (f32)nP%u;", op[0], b[0])) return 0;
    size_t j = i + 4;
    bool neg = false;
    if (strip_eq(L[j], "ps0 = -ps0;") && strip_eq(L[j + 1], "ps1 = -ps1;")) { neg = true; j += 2; }
    if (j + 1 >= n || !pmatch(L[j], "\1nF\2 = dolrecomp_ps_round(ps0);", NULL, d, NULL)) return 0;
    if (!strip_is(L[j + 1], "nP%u = dolrecomp_ps_round(ps1);", d[0])) return 0;
    push_fmt(out, "%.*snrv_v2f ps_ = nrv_f(nV%u) * nrv_f(nV%u);", (int)ind, L[i], v[0], v[1]);
    push_fmt(out, "%.*sps_ %c= nrv_f(nV%u);", (int)ind, L[i], op[0], b[0]);
    if (neg) push_fmt(out, "%.*sps_ = -ps_;", (int)ind, L[i]);
    push_fmt(out, "%.*snV%u = nrv_d(ps_);", (int)ind, L[i], d[0]);
    return j + 2 - i;
}

/* `<ws><head>X);` -> X (a copy), the prototype's greedy `(.*)\);$` */
static char* round_arg(const char* l, const char* head, size_t* ind) {
    const char* s = l;
    while (*s == ' ' || *s == '\t') s++;
    if (ind) *ind = (size_t)(s - l);
    if (!starts(s, head)) return NULL;
    s += strlen(head);
    size_t k = strlen(s);
    if (k < 2 || strcmp(s + k - 2, ");") != 0) return NULL;
    char* x = malloc(k - 1);
    memcpy(x, s, k - 2);
    x[k - 2] = 0;
    return x;
}

/* ps_merge00..11 / ps_muls0/1 / ps_madds0/1: `{ f64 d0 = dolrecomp_ps_round(X0);` + `f64 d1 = ...(X1);` +
 * `nFd = d0; nPd = d1; }` -> one vector statement; false = not one of those */
static bool simd_s01(char** L, size_t n, size_t i, Lines* out) {
    if (i + 2 >= n) return false;
    size_t ind;
    unsigned d[2];
    char* x0 = round_arg(L[i], "{ f64 d0 = dolrecomp_ps_round(", &ind);
    char* x1 = x0 ? round_arg(L[i + 1], "f64 d1 = dolrecomp_ps_round(", NULL) : NULL;
    bool ok = x1 && pmatch(L[i + 2], "\1nF\2 = d0; nP\2 = d1; }", NULL, d, NULL) && d[1] == d[0];
    unsigned a[3], b[3];
    char c0[1], c1[1];
    char buf[256];
    if (ok && pmatch(x0, "n\4\2", NULL, a, c0) && pmatch(x1, "n\4\2", NULL, b, c1)) {
        push_fmt(out, "%.*s{ nrv_v2d t_ = __builtin_shufflevector(nV%u, nV%u, %d, %d); nV%u = nrv_round(t_); }", (int)ind,
                 L[i], a[0], b[0], c0[0] == 'F' ? 0 : 1, 2 + (c1[0] == 'F' ? 0 : 1), d[0]);
    } else if (ok && pmatch(x0, "nF\2 * dolrecomp_force25(n\4\2)", NULL, a, c0) &&
               snprintf(buf, sizeof(buf), "nP%u * dolrecomp_force25(n%c%u)", a[0], c0[0], a[1]) > 0 && strcmp(x1, buf) == 0) {
        push_fmt(out, "%.*snV%u = nrv_round(nV%u * nrv_force25(%s(nV%u)));", (int)ind, L[i], d[0], a[0],
                 c0[0] == 'F' ? "NRV_S0" : "NRV_S1", a[1]);
    } else if (ok && pmatch(x0, "(f32)nF\2 * (f32)n\4\2 + (f32)nF\2", NULL, a, c0) &&
               snprintf(buf, sizeof(buf), "(f32)nP%u * (f32)n%c%u + (f32)nP%u", a[0], c0[0], a[1], a[2]) > 0 &&
               strcmp(x1, buf) == 0) {
        push_fmt(out, "%.*s{ nrv_v2f t_ = nrv_f(nV%u) * nrv_f(%s(nV%u)) + nrv_f(nV%u); nV%u = nrv_d(t_); }", (int)ind, L[i],
                 a[0], c0[0] == 'F' ? "NRV_S0" : "NRV_S1", a[1], a[2], d[0]);
    } else {
        ok = false;
    }
    free(x0);
    free(x1);
    return ok;
}

/* The --region-simd rewrite of one finished region block; NULL = left scalar (too few paired-single ops, or no
 * single FP-local declaration line to rewrite). */
static char* simd_region(const char* block) {
    Lines L = {0}, out = {0};
    for (const char *p = block, *q;; p = q + 1) {
        q = strchr(p, '\n');
        if (!q) { lines_push(&L, strdup(p)); break; }
        char* l = malloc((size_t)(q - p) + 1);
        memcpy(l, p, (size_t)(q - p));
        l[q - p] = 0;
        lines_push(&L, l);
    }
    unsigned nps = 0;
    size_t end = L.n, decl = 0, ndecl = 0;
    for (size_t i = 0; i < L.n; i++) {
        if (starts(L.v[i], "#undef NAT_WB")) { end = i; break; }
        if (is_ps_op(L.v[i])) nps++;
        unsigned k;
        bool same;
        for (const char* p = strstr(L.v[i], "f64 nF"); p; p = strstr(p + 1, "f64 nF"))
            if (fp_decl_at(L.v[i], p, &k, &same)) { decl = i; ndecl++; break; }
    }
    if (nps < kSimdMinOps || ndecl != 1 || end == L.n) { lines_free(&L); return NULL; }

    unsigned regs[32];
    size_t nregs = 0;
    for (size_t i = 0; i < decl; i++) lines_push(&out, strdup(L.v[i]));
    {
        Str o = {0};
        const char* l = L.v[decl];
        const char* s = l;
        for (const char* p = strstr(s, "f64 nF"); p; p = strstr(p, "f64 nF")) {
            unsigned k;
            bool same;
            const char* e = fp_decl_at(l, p, &k, &same);
            if (!e || !same || nregs == 32) { p++; continue; }
            str_add(&o, s, (size_t)(p - s));
            str_printf(&o, "nrv_v2d nV%u = { ctx->fpr[%u], ctx->ps1[%u] };", k, k, k);
            regs[nregs++] = k;
            s = p = e;
        }
        str_puts(&o, s);
        lines_push(&out, str_take(&o));
    }
    for (size_t r = 0; r < nregs; r++) {
        push_fmt(&out, "#define nF%u (nV%u[0])", regs[r], regs[r]);
        push_fmt(&out, "#define nP%u (nV%u[1])", regs[r], regs[r]);
    }
    for (size_t i = decl + 1; i < L.n;) {
        const char* l = L.v[i];
        if (i >= end) {
            lines_push(&out, strdup(l));
            if (i == end)
                for (size_t r = 0; r < nregs; r++) {
                    push_fmt(&out, "#undef nF%u", regs[r]);
                    push_fmt(&out, "#undef nP%u", regs[r]);
                }
            i++;
            continue;
        }
        const char* nx = i + 1 < L.n ? L.v[i + 1] : "";
        size_t ind;
        unsigned v[4];
        char op[1];
        if (pmatch(l, "\1nF\2 = dolrecomp_ps_round((f32)nF\2 \3 (f32)nF\2);", &ind, v, op) &&
            line_is(nx, "%.*snP%u = dolrecomp_ps_round((f32)nP%u %c (f32)nP%u);", (int)ind, l, v[0], v[1], op[0], v[2])) {
            push_fmt(&out, "%.*snV%u = nrv_d(nrv_f(nV%u) %c nrv_f(nV%u));", (int)ind, l, v[0], v[1], op[0], v[2]);
        } else if (pmatch(l, "\1nF\2 = dolrecomp_ps_round(nF\2 * dolrecomp_force25(nF\2));", &ind, v, NULL) &&
                   line_is(nx, "%.*snP%u = dolrecomp_ps_round(nP%u * dolrecomp_force25(nP%u));", (int)ind, l, v[0], v[1], v[2])) {
            push_fmt(&out, "%.*snV%u = nrv_round(nV%u * nrv_force25(nV%u));", (int)ind, l, v[0], v[1], v[2]);
        } else if (pmatch(l, "\1nF\2 = ((f32)nF\2 >= 0.0f) ? nF\2 : nF\2;", &ind, v, NULL) &&
                   line_is(nx, "%.*snP%u = ((f32)nP%u >= 0.0f) ? nP%u : nP%u;", (int)ind, l, v[0], v[1], v[2], v[3])) {
            push_fmt(&out, "%.*snV%u = nrv_sel(nV%u, nV%u, nV%u);", (int)ind, l, v[0], v[1], v[2], v[3]);
        } else if (pmatch(l, "\1nF\2 = nF\2;", &ind, v, NULL) && line_is(nx, "%.*snP%u = nP%u;", (int)ind, l, v[0], v[1])) {
            push_fmt(&out, "%.*snV%u = nV%u;", (int)ind, l, v[0], v[1]);
        } else {
            size_t used = simd_madd(L.v, end, i, &out);
            if (used) { s_simd_madd++; i += used; continue; }
            if (simd_s01(L.v, end, i, &out)) { s_simd_s01++; i += 3; continue; }
            if (pmatch(l, "\1if (__builtin_expect(nreg_q0_ok(ctx, ea), 1)) NREG_PSQ_L2(ea, nF\2, nP\2); else {", &ind, v, NULL)) {
                push_fmt(&out, "%.*sif (__builtin_expect(nreg_q0_ok(ctx, ea), 1)) { f64 a_, b_; NREG_PSQ_L2(ea, a_, b_); "
                               "nrv_v2d t_ = { a_, b_ }; nV%u = t_; } else {", (int)ind, l, v[0]);
                s_simd_psql++;
                i++;
                continue;
            }
            lines_push(&out, strdup(l));
            i++;
            continue;
        }
        s_simd_pairs++;
        i += 2;
    }
    s_simd_regions++;
    Str o = {0};
    for (size_t i = 0; i < out.n; i++) {
        if (i) str_puts(&o, "\n");
        str_puts(&o, out.v[i]);
    }
    lines_free(&L);
    lines_free(&out);
    return str_take(&o);
}

static bool write_chunk(Chunk* c) {
    FILE* w = fopen(c->path, "wb");
    if (!w) { fprintf(stderr, "native-regions: cannot write %s\n", c->path); return false; }
    bool header = false;
    for (size_t i = 0; i < c->work.n; i++) {
        fputs(c->work.v[i], w);
        if (i + 1 < c->work.n) fputs("\n", w);
        if (!header && i + 1 < c->work.n && strcmp(c->work.v[i], "#include \"../generated.h\"") == 0) {
            fputs(kHelpers, w);
            if (s_psq_direct) fputs(kPsqHelpers, w);
            if (c->simd) fputs(kSimdPrelude, w);
            header = true;
        }
    }
    fclose(w);
    if (!header) { fprintf(stderr, "native-regions: %s has no generated.h include\n", c->path); return false; }
    return true;
}

static bool list_chunks(const char* chunks_dir) {
#ifdef _WIN32
    char pattern[2048];
    if (!join_path(pattern, sizeof(pattern), chunks_dir, "chunk_*.c")) return false;
    WIN32_FIND_DATAA data;
    HANDLE find = FindFirstFileA(pattern, &data);
    if (find == INVALID_HANDLE_VALUE) { fprintf(stderr, "native-regions: cannot list %s\n", chunks_dir); return false; }
    do {
        const char* name = data.cFileName;
#else
    DIR* d = opendir(chunks_dir);
    if (!d) { fprintf(stderr, "native-regions: cannot list %s\n", chunks_dir); return false; }
    for (struct dirent* e; (e = readdir(d));) {
        const char* name = e->d_name;
#endif
        size_t k = strlen(name);
        u32 win;
        if (k > 12 && starts(name, "chunk_") && strcmp(name + k - 2, ".c") == 0 && name[k - 11] == '_' && hex8(name + k - 10, &win)) {
            char path[2048];
            if (join_path(path, sizeof(path), chunks_dir, name)) {
                s_chunks = realloc(s_chunks, (s_chunk_n + 1) * sizeof(Chunk));
                memset(&s_chunks[s_chunk_n], 0, sizeof(Chunk));
                s_chunks[s_chunk_n].window = win;
                s_chunks[s_chunk_n].path = strdup(path);
                s_chunk_n++;
            }
        }
#ifdef _WIN32
    } while (FindNextFileA(find, &data));
    FindClose(find);
#else
    }
    closedir(d);
#endif
    qsort(s_chunks, s_chunk_n, sizeof(Chunk), cmp_chunk);
    return true;
}

/* The entry switch of the hot function `void func_<window>(CPUState* ctx) {`: every
 * `    case 0xXu: goto label_X;` with start < X < end, and the line it is on. Lines before the first
 * label of the function, so inserting a region block later in the file never moves them. */
static size_t hot_cases(const Lines* L, u32 window, u32 start, u32 end, u32* pcs, long* lines, size_t cap) {
    char head[64];
    snprintf(head, sizeof(head), "void func_%08X(CPUState* ctx) {", window);
    size_t i = 0, n = 0;
    while (i < L->n && strcmp(L->v[i], head) != 0) i++;
    for (; i < L->n && !starts(L->v[i], "    default:"); i++) {
        const char* l = L->v[i];
        u32 pc, pc2;
        if (!starts(l, "    case 0x") || !hex8(l + 11, &pc) || strncmp(l + 19, "u: goto label_", 14) != 0) continue;
        if (!hex8(l + 33, &pc2) || pc2 != pc || strcmp(l + 41, ";") != 0) continue;
        if (pc > start && pc < end && n < cap) { pcs[n] = pc; lines[n] = (long)i; n++; }
    }
    return n;
}

bool native_regions_apply(const char* chunks_dir) {
    if (!s_enabled) return true;
    if (!list_chunks(chunks_dir)) return false;
    /* the disassembly and the call targets, from every chunk */
    for (size_t i = 0; i < s_chunk_n; i++) {
        if (!chunk_load(&s_chunks[i])) return false;
        for (size_t k = 0; k < s_chunks[i].orig.n; k++) asm_line(s_chunks[i].orig.v[k]);
    }
    qsort(s_fn, s_fn_n, sizeof(u32), cmp_u32);
    size_t u = 0;
    for (size_t i = 0; i < s_fn_n; i++)
        if (!u || s_fn[u - 1] != s_fn[i]) s_fn[u++] = s_fn[i];
    s_fn_n = u;

    /* A span can exist twice: in its home chunk, and at the end of the PREVIOUS chunk when --chunk-overhang
     * emitted it there as well. Execution reaches either copy (falling through from the previous window stays
     * in that chunk), so every hot copy that holds the whole span gets the region. An overhang never reaches
     * past one window, so the home chunk and the one before it are the only candidates. */
    unsigned made = 0, skipped = 0, calls = 0, copies = 0, resumes = 0;
    for (size_t r = 0; r < s_region_count; r++) {
        u32 start = s_regions[r].start, end = s_regions[r].end;
        Chunk* home = chunk_for(start);
        if (!home) { printf("native-regions: %08X: no chunk; skipped\n", start); skipped++; continue; }
        unsigned here = 0;
        const char* home_why = NULL;
        for (Chunk* c = home; c >= s_chunks && c >= home - 1; c--) {
            if (!c->patched) {
                for (size_t k = 0; k < c->orig.n; k++) lines_push(&c->work, strdup(c->orig.v[k]));
                c->patched = true;
            }
            long a = last_label(&c->work, start), b = last_label(&c->work, end);
            if (a < 0 || b < 0 || b <= a) {
                if (c == home) home_why = "labels not found in one copy";
                continue;
            }
            const char* why = NULL;
            unsigned n_inl = 0;
            /* the hot function's entry switch: resumable PCs strictly inside the span (home copy only -- the
             * dispatcher resolves a pc to its home chunk, so the overhang copy's switch never sees them) */
            u32 rpc[512];
            long rline[512];
            bool stubbed[512] = {false};
            size_t nr = c == home ? hot_cases(&c->work, c->window, start, end, rpc, rline, 512) : 0;
            char* block = transform(c->work.v, (size_t)a, (size_t)b, start, end, &why, &n_inl, rpc, nr, stubbed,
                                    s_regions[r].exits);
            if (!block) {
                if (c == home) home_why = why;
                else printf("native-regions: %08X: overhang copy left as emitted: %s\n", start, why);
                continue;
            }
            if (s_region_simd) {
                char* v = simd_region(block);
                if (v) { free(block); block = v; c->simd = true; }
            }
            lines_push(&c->work, NULL);
            memmove(&c->work.v[a + 2], &c->work.v[a + 1], (c->work.n - (size_t)a - 2) * sizeof(char*));
            c->work.v[a + 1] = block;
            for (size_t i = 0; i < nr; i++) {
                if (!stubbed[i]) continue;
                char buf[80];
                snprintf(buf, sizeof(buf), "    case 0x%08Xu: goto nreg_e_%08X;", rpc[i], rpc[i]);
                free(c->work.v[rline[i]]);
                c->work.v[rline[i]] = strdup(buf);
                resumes++;
            }
            here++;
            calls += n_inl;
        }
        if (home_why) printf("native-regions: %08X: %s%s\n", start, home_why, here ? " (home copy); overhang copy made native" : "; skipped");
        if (here) { made++; copies += here; }
        else skipped++;
    }
    bool ok = true;
    unsigned files = 0;
    for (size_t i = 0; i < s_chunk_n; i++) {
        Chunk* c = &s_chunks[i];
        bool changed = c->patched && c->work.n != c->orig.n;
        if (changed) { ok = write_chunk(c) && ok; files++; }
        lines_free(&c->work);
        lines_free(&c->orig);
        free(c->path);
    }
    free(s_chunks);
    s_chunks = NULL;
    s_chunk_n = 0;
    for (size_t i = 0; i < s_asm_n; i++) { free(s_asm[i].op); free(s_asm[i].args); }
    free(s_asm);
    s_asm = NULL;
    s_asm_n = 0;
    free(s_fn);
    s_fn = NULL;
    s_fn_n = s_fn_cap = 0;
    printf("native-regions: %u of %u regions made native (%u copies, %u resume entries, %u calls inlined, %u skipped), "
           "%u chunk files\n", made, (unsigned)s_region_count, copies, resumes, calls, skipped, files);
    if (s_region_simd)
        printf("native-regions: --region-simd: %u region copies vectorised (%u op pairs, %u madd-family, %u s0/s1/merge, "
               "%u psq_l)\n", s_simd_regions, s_simd_pairs, s_simd_madd, s_simd_s01, s_simd_psql);
    return ok;
}

/* ---- --inline-leaves N: small leaf callees inlined at their native call sites ----
 *
 * Every native direct call (`ctx->lr = RET; ctx->pc = T; if (dolrecomp_call_enter()) { ... func_X(ctx); ... }
 * DOLRECOMP_RETURN;`) whose callee is a small straight-line leaf becomes a copy of the callee's emitted body,
 * with its labels renamed per site and its blr sending a return to RET straight to label_RET. Every other exit
 * of the copy leaves the guest exactly as the call would have (LR already set, ctx up to date, the cycle charge
 * taken block by block), so the frame hash cannot move.
 *
 * A callee qualifies when, from its entry, it reaches a blr within N instructions with every branch forward and
 * inside, and contains nothing that touches LR/CTR/MSR/SPRs, syncs, reserves or moves many registers. Its body
 * is the COLD copy's text (every register in ctx; the caller reloads its write-through locals at the return),
 * or, when the site is in a fast copy that holds the callee too, that copy's own text. Region and skin code is
 * never copied.
 *
 * Steam Deck, US, idle VS route, both arms PGO-retrained, n=8: -1.08% and -1.19% cycles in two runs, -1.38%
 * instructions, frame hashes identical (prototype: inline.py, all sites, N=64). Inlining only the hottest 800
 * sites LOST (+0.41%), and inlining region callees too LOST (+1.70%: the module grew from 55 to 76 MB). */
static unsigned s_leaf_max = 0;
void native_leaves_set(unsigned max_insns) { s_leaf_max = max_insns; }

static const char* const kLeafForbid[] = {
    "bl", "bctrl", "blrl", "bctr", "bcctr", "bclr", "mtlr", "mflr", "mtctr", "mfctr", "sc", "rfi", "mtmsr",
    "mfmsr", "mtspr", "mfspr", "isync", "sync", "eieio", "icbi", "dcbi", "dcbf", "dcbst", "dcbz", "dcbz_l",
    "lwarx", "stwcx.", "mtsr", "mtfsf", "mtfsfi", "mtfsb0", "mtfsb1", "mffs", "tw", "twi", "stmw", "lmw", NULL};

/* [f, end) or 0: straight-line forward code from f to a blr no branch jumps past, within s_leaf_max. */
static u32 leaf_extent(u32 f) {
    u32 maxfwd = 0;
    for (u32 n = 0, pc = f; n < s_leaf_max; n++, pc += 4) {
        const Insn* in = asm_at(pc);
        if (!in || !strcmp(in->op, "?")) return 0;
        for (const char* const* x = kLeafForbid; *x; x++)
            if (!strcmp(in->op, *x)) return 0;
        if (!strcmp(in->op, "blr")) {
            if (maxfwd <= pc) return pc + 4;
            continue;
        }
        if (in->op[0] == 'b') {   /* b, bc, bdnz, ...: forward, to a known pc */
            u32 t;
            if (!branch_target(in->args, &t) || t <= pc) return 0;
            if (t > maxfwd) maxfwd = t;
        }
    }
    return 0;
}
static bool in_any_region(u32 a, u32 b) {
    for (size_t r = 0; r < s_region_count; r++)
        if (s_regions[r].start < b && a < s_regions[r].end) return true;
    return false;
}
static long fast_line(const Lines* L) {
    for (size_t i = 0; i < L->n; i++)
        if (strcmp(L->v[i], "/* ---- fast copy ---- */") == 0) return (long)i;
    return (long)L->n;
}
/* `label_<pc>:` in L[from, to), or -1 */
static long label_in(const Lines* L, u32 pc, size_t from, size_t to) {
    char want[24];
    snprintf(want, sizeof(want), "label_%08X:", pc);
    for (size_t i = from; i < to && i < L->n; i++)
        if (strcmp(L->v[i], want) == 0) return (long)i;
    return -1;
}
/* the token `goto NAME;` at p: NAME's length, or 0 */
static size_t word_len(const char* p) { size_t k = 0; while (is_word(p[k])) k++; return k; }

typedef struct { Lines body; char (*labels)[16]; size_t nlabels; bool ok; bool fast; } LeafBody;

/* Lines [label_f, first line at or past end) of L starting at line a. false if anything in it is not plain
 * emitted code or a goto leaves it. */
static bool leaf_span(const Lines* L, long a, u32 end, LeafBody* out) {
    size_t e = (size_t)a + 1;
    for (; e < L->n; e++) {
        const char* l = L->v[e];
        u32 pc;
        if (match_label(l, &pc) == 1 && pc >= end) break;
        if (starts(l, "    // ") && hex8(l + 7, &pc) && l[15] == ':' && pc >= end) break;
        if (strcmp(l, "}") == 0) break;
    }
    out->nlabels = 0;
    out->labels = malloc((e - (size_t)a) * sizeof(*out->labels));
    for (size_t i = (size_t)a; i < e; i++) {
        const char* l = L->v[i];
        if (strstr(l, "NAT_") || strstr(l, "nreg") || strstr(l, "nskin") || strstr(l, "NREG") ||
            strstr(l, "dolrecomp_call_enter"))
            return false;
        u32 pc;
        if (match_label(l, &pc)) {
            size_t k = (size_t)(strchr(l, ':') - l);
            memcpy(out->labels[out->nlabels], l, k);
            out->labels[out->nlabels++][k] = 0;
        }
    }
    for (size_t i = (size_t)a; i < e; i++)
        for (const char* p = strstr(L->v[i], "goto "); p; p = strstr(p + 5, "goto ")) {
            size_t k = word_len(p + 5);
            bool found = false;
            for (size_t j = 0; j < out->nlabels && !found; j++)
                found = strlen(out->labels[j]) == k && strncmp(out->labels[j], p + 5, k) == 0;
            if (!found) return false;
        }
    for (size_t i = (size_t)a; i < e; i++) lines_push(&out->body, strdup(L->v[i]));
    return true;
}
static void leaf_free(LeafBody* b) { lines_free(&b->body); free(b->labels); b->labels = NULL; }

/* One body line for site `sfx` returning to `ret`: labels renamed, `ctx->pc = target;` (the blr) turned into
 * a goto when the return address is the call's own. */
static char* leaf_line(const char* l, const LeafBody* b, const char* sfx, u32 ret, const char* reload, unsigned* n_ret) {
    Str o = {0};
    const char* p = l;
    while (*p) {
        if ((p == l || !is_word(p[-1])) && (starts(p, "label_") || starts(p, "fpok_"))) {
            size_t k = word_len(p);
            bool hit = false;
            for (size_t j = 0; j < b->nlabels && !hit; j++)
                hit = strlen(b->labels[j]) == k && strncmp(b->labels[j], p, k) == 0;
            str_add(&o, p, k);
            if (hit) str_puts(&o, sfx);
            p += k;
            continue;
        }
        if (starts(p, "ctx->pc = target;")) {
            str_printf(&o, "if (target == 0x%08Xu) { %s%sgoto label_%08X; } ", ret, reload, *reload ? " " : "", ret);
            (*n_ret)++;
        }
        str_add(&o, p, 1);
        p++;
    }
    return str_take(&o);
}

/* the call block at line i: the `}` closing its enter-if (j) and the DOLRECOMP_RETURN after it (k) */
static bool call_block(const Lines* L, size_t i, size_t* j_out, size_t* k_out, const char** reload) {
    size_t j = i + 3, k;
    *reload = NULL;
    for (; j < L->n && !strip_eq(L->v[j], "}"); j++)
        if (strstr(L->v[j], "DOLRECOMP_DC_RELOAD(ctx);")) *reload = L->v[j];
    for (k = j + 1; k < L->n && !strip_eq(L->v[k], "DOLRECOMP_RETURN;"); k++) {}
    *j_out = j;
    *k_out = k;
    return j < L->n && k < L->n && *reload;
}
/* the callee's body for a site at line i of chunk c: that fast copy's own text when the site is in a fast copy
 * that holds the callee, else the cold copy in the callee's home chunk */
static bool leaf_body(Chunk* c, size_t i, u32 t, u32 end, LeafBody* b) {
    long fast = fast_line(&c->orig);
    memset(b, 0, sizeof(*b));
    if ((long)i > fast) {
        long a = label_in(&c->orig, t, (size_t)fast, c->orig.n);
        if (a >= 0) {
            b->fast = true;
            if (leaf_span(&c->orig, a, end, b)) return true;
        }
        leaf_free(b);
        memset(b, 0, sizeof(*b));
    }
    Chunk* h = chunk_for(t);
    if (!h || !chunk_load(h)) return false;
    long a = label_in(&h->orig, t, 0, (size_t)fast_line(&h->orig));
    return a >= 0 && leaf_span(&h->orig, a, end, b);
}
static bool write_lines(const char* path, const Lines* L) {
    FILE* w = fopen(path, "wb");
    if (!w) { fprintf(stderr, "inline-leaves: cannot write %s\n", path); return false; }
    for (size_t i = 0; i < L->n; i++) {
        fputs(L->v[i], w);
        if (i + 1 < L->n) fputs("\n", w);
    }
    return fclose(w) == 0;
}

bool native_leaves_apply(const char* chunks_dir) {
    if (!s_leaf_max) return true;
    if (!list_chunks(chunks_dir)) return false;
    for (size_t i = 0; i < s_chunk_n; i++) {
        if (!chunk_load(&s_chunks[i])) return false;
        for (size_t k = 0; k < s_chunks[i].orig.n; k++) asm_line(s_chunks[i].orig.v[k]);
    }
    unsigned sites = 0, refused = 0, files = 0;
    bool ok = true;
    for (size_t ci = 0; ci < s_chunk_n; ci++) {
        Chunk* c = &s_chunks[ci];
        const Lines* L = &c->orig;
        unsigned here = 0;
        for (size_t i = 0; i < L->n; i++) {
            const char* l = L->v[i];
            u32 ret, t, end;
            size_t j, k;
            const char* reload;
            LeafBody b = {0};
            if (i + 2 >= L->n || !match_set(l, "ctx->lr = ", &ret) || !match_set(L->v[i + 1], "ctx->pc = ", &t) ||
                !strstr(L->v[i + 2], "dolrecomp_call_enter()") || !(end = leaf_extent(t)) || in_any_region(t, end)) {
                lines_push(&c->work, strdup(l));
                continue;
            }
            if (!call_block(L, i, &j, &k, &reload) || !leaf_body(c, i, t, end, &b)) {
                leaf_free(&b);
                refused++;
                lines_push(&c->work, strdup(l));
                continue;
            }
            /* the caller's write-through locals, reloaded at the return (the DC was never flushed) */
            char* locals = b.fast ? strdup("") : replace_all(lskip(reload), "DOLRECOMP_DC_RELOAD(ctx);", "");
            char sfx[16], cm[96];
            snprintf(sfx, sizeof(sfx), "_i%u", here);
            snprintf(cm, sizeof(cm), "%.*s/* inlined leaf call: %08X (--inline-leaves) */", (int)indent_len(l), l, t);
            size_t mark = c->work.n;
            unsigned n_ret = 0;
            lines_push(&c->work, strdup(l));
            lines_push(&c->work, strdup(cm));
            for (size_t q = 0; q < b.body.n; q++)
                lines_push(&c->work, leaf_line(b.body.v[q], &b, sfx, ret, lskip(locals), &n_ret));
            free(locals);
            leaf_free(&b);
            if (!n_ret) {   /* no return in the copy: keep the call */
                while (c->work.n > mark) free(c->work.v[--c->work.n]);
                refused++;
                lines_push(&c->work, strdup(l));
                continue;
            }
            here++;
            i = k;   /* the whole call block is replaced */
        }
        if (here) { ok = write_lines(c->path, &c->work) && ok; files++; sites += here; }
    }
    for (size_t i = 0; i < s_chunk_n; i++) {
        lines_free(&s_chunks[i].work);
        lines_free(&s_chunks[i].orig);
        free(s_chunks[i].path);
    }
    free(s_chunks);
    s_chunks = NULL;
    s_chunk_n = 0;
    for (size_t i = 0; i < s_asm_n; i++) { free(s_asm[i].op); free(s_asm[i].args); }
    free(s_asm);
    s_asm = NULL;
    s_asm_n = 0;
    free(s_fn);
    s_fn = NULL;
    s_fn_n = s_fn_cap = 0;
    printf("inline-leaves: %u call sites inlined (%u refused), %u chunk files\n", sites, refused, files);
    return ok;
}
