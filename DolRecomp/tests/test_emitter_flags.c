/* The emitter flags, and the correctness invariants they carry that are not
 * obvious from the flag name.
 *
 * --chain-calls turns a local `bl` into a native goto. The invariant that makes
 * it safe is the negative one: a call whose TARGET the run loop recognises by
 * PC must never be chained, because a call that does not come back through the
 * dispatcher is a hook that never fires. There are seven such FMV-HLE
 * addresses, and --idle-pc registers an eighth. Chaining one of those would not
 * crash -- the movie would simply stop being intercepted.
 *
 * --ca-liveness claims in its own --help text that codegen is UNCHANGED; it
 * only counts provably-dead XER[CA] writes. That claim is worth pinning,
 * because the day someone wires the liveness result into emission is the day it
 * silently stops being true.
 *
 * --leader-cases (shipped) reduces the entry switch, and its invariant is also
 * the negative one: an instruction control CAN arrive at must keep its case.
 * Dropping the FP-guard resume sites shifted guest timing (685 frames of heap
 * drift); dropping cross-chunk and jump-table targets diverged on a second
 * route. Both would pass a "the switch got smaller" check.
 *
 * ORDER IS LOAD-BEARING: emit_add_dispatch_pc() accumulates into static state
 * with no way to remove an entry, so every case that needs an UNprotected
 * target must run before the one that registers it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/backend/emitter.h"
#include "../src/common/types.h"
#include "../src/frontend/decoder.h"

#define BASE   0x80003000u
#define TARGET 0x80003008u

/* bl +8 ; nop ; nop (the call target) ; blr */
static const u32 raws[] = {
    0x48000009u,  /* bl  0x80003008  -- forward, local, linked */
    0x60000000u,  /* nop                                        */
    0x60000000u,  /* nop   <- TARGET                            */
    0x4E800020u,  /* blr                                        */
};
#define NRAW ((u32)(sizeof(raws) / sizeof(raws[0])))

/* One block, no local branches: only its first instruction is a leader. */
#define EBASE 0x80004000u
static const u32 entry_raws[] = {
    0x60000000u,  /* 80004000 nop                 leader                     */
    0xC0030000u,  /* 80004004 lfs f0, 0(r3)       FP guard: lazy-FPU resume  */
    0x60000000u,  /* 80004008 nop                 target from ANOTHER chunk  */
    0x60000000u,  /* 8000400C nop                 nothing arrives here       */
    0x4E800020u,  /* 80004010 blr                                            */
};
#define NENTRY ((u32)(sizeof(entry_raws) / sizeof(entry_raws[0])))

/* --fp-check-once: two FP ops in a run, then mtmsr, then another FP op. */
#define FBASE 0x80006000u
static const u32 fp_raws[] = {
    0xC0030000u,  /* 80006000 lfs f0, 0(r3)   leader: must test MSR.FP       */
    0xC0230004u,  /* 80006004 lfs f1, 4(r3)   tested already: may skip it    */
    0x7C800124u,  /* 80006008 mtmsr r4        can turn FP off                */
    0xC0430008u,  /* 8000600C lfs f2, 8(r3)   after mtmsr: must test again   */
    0x4E800020u,  /* 80006010 blr                                            */
};
#define NFP ((u32)(sizeof(fp_raws) / sizeof(fp_raws[0])))

/* An FP op, then a mid-block entry that is NOT an FP op (a direct-call return
   point or a cross-chunk target), then another FP op. Code entering at the nop
   has not tested MSR.FP, so the second lfs must test on that path. */
#define EFBASE 0x80007000u
static const u32 fp_entry_raws[] = {
    0xC0030000u,  /* 80007000 lfs f0, 0(r3)                                  */
    0x60000000u,  /* 80007004 nop   <- an entry from another chunk           */
    0xC0230004u,  /* 80007008 lfs f1, 4(r3)                                  */
    0x4E800020u,  /* 8000700C blr                                            */
};
#define NFPE ((u32)(sizeof(fp_entry_raws) / sizeof(fp_entry_raws[0])))

static int failures;

static char* emit_raws_to_string(const u32* words, u32 count, u32 base) {
    PPCInst insts[8];
    if (count > 8u) return NULL;
    for (u32 i = 0; i < count; i++) {
        insts[i] = ppc_decode(words[i], base + i * 4u);
        if (insts[i].op == PPC_OP_UNKNOWN) {
            fprintf(stderr, "raw 0x%08X decoded as unknown\n", words[i]);
            return NULL;
        }
    }
    FILE* f = tmpfile();
    if (!f) return NULL;
    emit_function(f, insts, count, count, base);
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);
    char* buf = (char*)calloc((size_t)n + 1u, 1u);
    if (buf && fread(buf, 1u, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    return buf;
}

static char* emit_to_string(void) {
    return emit_raws_to_string(raws, NRAW, BASE);
}

/* Scope to the CALL SITE. The chunk's entry switch lists `case 0x80003008u:
   goto label_80003008;` for every leader, so a whole-text search for that goto
   matches the prologue and reports a chained call that is not there. This cost
   a false failure once already, in test_idle_pc. */
static char* call_site(const char* text) {
    static char site[4096];
    const char* a = strstr(text, "label_80003000:");
    if (!a) return NULL;
    const char* b = strstr(a, "label_80003004:");
    size_t n = (b ? (size_t)(b - a) : strlen(a));
    if (n >= sizeof(site)) n = sizeof(site) - 1u;
    memcpy(site, a, n);
    site[n] = '\0';
    return site;
}

static void expect(const char* what, int cond, const char* text) {
    if (!cond) {
        fprintf(stderr, "%s\n--- call site was ---\n%s\n", what, text ? text : "(null)");
        failures++;
    }
}

static int has_case(const char* text, u32 address) {
    char needle[64];
    snprintf(needle, sizeof(needle), "    case 0x%08Xu: goto label_%08X;", address, address);
    return strstr(text, needle) != NULL;
}

int main(void) {
    char *off, *on, *hooked, *ca_off, *ca_on;
    char site[4096];

    /* 1. --ca-liveness must not move a single byte of output. Run first, while
          no dispatch PC is registered, so the two arms are otherwise identical. */
    ca_off = emit_to_string();
    if (!ca_off) return 1;
    emit_set_ca_liveness(true);
    ca_on = emit_to_string();
    if (!ca_on) { free(ca_off); return 1; }
    expect("--ca-liveness changed the emitted code; its --help says codegen is unchanged",
           strcmp(ca_off, ca_on) == 0, NULL);
    emit_set_ca_liveness(false);
    free(ca_off);
    free(ca_on);

    /* 2. Chaining off: a local `bl` costs a dispatcher round trip. */
    off = emit_to_string();
    if (!off) return 1;
    snprintf(site, sizeof(site), "%s", call_site(off) ? call_site(off) : "");
    expect("chaining off: local bl should return to the dispatcher",
           strstr(site, "ctx->pc = 0x80003008u;") != NULL, site);
    expect("chaining off: local bl must NOT be a goto",
           strstr(site, "goto label_80003008") == NULL, site);
    free(off);

    /* 3. Chaining on: the same call becomes a native goto. */
    emit_set_chain_calls(true);
    on = emit_to_string();
    if (!on) return 1;
    snprintf(site, sizeof(site), "%s", call_site(on) ? call_site(on) : "");
    expect("chaining on: local bl should become a native goto",
           strstr(site, "goto label_80003008") != NULL, site);
    free(on);

    /* 4. THE INVARIANT. Same flag, same call -- but the target is now a PC the
          run loop hooks. It must go back to dispatching, or the hook is dead. */
    emit_add_dispatch_pc(TARGET);
    hooked = emit_to_string();
    if (!hooked) return 1;
    snprintf(site, sizeof(site), "%s", call_site(hooked) ? call_site(hooked) : "");
    expect("chained a call to a hooked PC -- that hook can never fire again",
           strstr(site, "goto label_80003008") == NULL, site);
    expect("hooked call target should return to the dispatcher",
           strstr(site, "ctx->pc = 0x80003008u;") != NULL, site);
    free(hooked);
    emit_set_chain_calls(false);

    /* 5. --leader-cases reduces the entry switch: fewer cases than instructions,
          and never none. */
    {
        char* plain = emit_to_string();
        if (!plain) return 1;
        emit_set_leader_cases(true);
        char* leaders = emit_to_string();
        if (!leaders) { free(plain); return 1; }

        unsigned n_plain = 0, n_leaders = 0;
        for (const char* p = plain; (p = strstr(p, "    case 0x")) != NULL; p += 5) n_plain++;
        for (const char* p = leaders; (p = strstr(p, "    case 0x")) != NULL; p += 5) n_leaders++;

        if (!(n_leaders < n_plain)) {
            fprintf(stderr, "--leader-cases did not reduce the entry switch "
                            "(%u cases -> %u); every instruction is still a "
                            "switch-reachable join point\n", n_plain, n_leaders);
            failures++;
        }
        if (n_leaders == 0u) {
            fprintf(stderr, "--leader-cases emitted NO entry cases; the chunk "
                            "cannot be entered at all\n");
            failures++;
        }
        emit_set_leader_cases(false);
        free(plain);
        free(leaders);
    }

    /* 6. --leader-cases THE INVARIANT: every place control can arrive keeps its
          case -- the leader, the FP-guard resume site, and a target supplied by
          the program-wide pass -- and only a place nothing reaches loses it. */
    {
        static const u32 targets[] = { 0x80004008u };
        emit_set_leader_cases(true);
        emit_set_entry_targets(targets, 1u);
        char* text = emit_raws_to_string(entry_raws, NENTRY, EBASE);
        if (!text) return 1;
        expect("--leader-cases dropped the block leader's case",
               has_case(text, 0x80004000u), NULL);
        expect("--leader-cases dropped the FP-guard instruction's case; the lazy-FPU "
               "resume would fall to the interpreter and shift guest timing",
               has_case(text, 0x80004004u), NULL);
        expect("--leader-cases dropped a program-wide branch/data target's case",
               has_case(text, 0x80004008u), NULL);
        expect("--leader-cases kept a case nothing can reach; the switch is not reduced",
               !has_case(text, 0x8000400Cu), NULL);
        free(text);

        /* Without the target list, that address is no longer an entry. */
        emit_set_entry_targets(NULL, 0u);
        text = emit_raws_to_string(entry_raws, NENTRY, EBASE);
        if (!text) return 1;
        expect("an address with no reason to be an entry kept its case",
               !has_case(text, 0x80004008u), NULL);
        expect("the FP-guard case must not depend on the target list",
               has_case(text, 0x80004004u), NULL);
        free(text);
        emit_set_leader_cases(false);
    }

    /* 7. --direct-calls: a `bl` into ANOTHER chunk calls that chunk's function
          and resumes inline on the return address -- and a hooked target must
          NOT be called directly, or the run loop's hook never fires. Runs last:
          it registers a dispatch PC, which cannot be unregistered. */
    {
        static const u32 chunk_starts[] = { 0x80006000u, 0x80008000u };
        static const u32 call_raws[] = {
            0x48002011u,  /* 80006000 bl 0x80008010 -- into the next chunk */
            0x60000000u,  /* 80006004 nop  (the continuation)             */
            0x4E800020u,  /* 80006008 blr                                  */
        };
        emit_set_chunk_table(chunk_starts, 2u);
        emit_set_direct_calls(true);
        char* text = emit_raws_to_string(call_raws, 3u, 0x80006000u);
        if (!text) return 1;
        expect("--direct-calls: a cross-chunk bl should call the target chunk's function",
               strstr(text, "func_80008000(ctx);") != NULL, text);
        expect("--direct-calls: the call must be guarded by the depth counter",
               strstr(text, "dolrecomp_call_enter()") != NULL, text);
        expect("--direct-calls: it must resume inline only on the return address",
               strstr(text, "if (ctx->pc == 0x80006004u) goto label_80006004;") != NULL, text);
        free(text);

        emit_add_dispatch_pc(0x80008010u);
        text = emit_raws_to_string(call_raws, 3u, 0x80006000u);
        if (!text) return 1;
        expect("--direct-calls called a hooked PC directly -- that hook can never fire again",
               strstr(text, "func_80008000(ctx);") == NULL, text);
        free(text);

        emit_set_direct_calls(false);
        emit_set_chunk_table(NULL, 0u);
    }

    /* 8. Not a flag, but the same kind of invisible invariant: a single-precision
          multiply rounds its RIGHT operand to 25 bits first (Dolphin's
          Force25Bit). Dropping it changes nothing on single inputs, so no route
          hash would ever notice it went missing. */
    {
        static const u32 mul_raws[] = {
            0xEC2200F2u,  /* 80009000 fmuls f1, f2, f3 */
            0x102200F2u,  /* 80009004 ps_mul f1, f2, f3 */
            0x4E800020u,  /* 80009008 blr               */
        };
        char* text = emit_raws_to_string(mul_raws, 3u, 0x80009000u);
        if (!text) return 1;
        expect("fmuls must round its C operand to 25 bits",
               strstr(text, "(f64)(f32)(ctx->fpr[2] * dolrecomp_force25(ctx->fpr[3]))") != NULL, text);
        expect("ps_mul must round each C lane to 25 bits",
               strstr(text, "ctx->fpr[2] * dolrecomp_force25(ctx->fpr[3])") != NULL &&
               strstr(text, "ctx->ps1[2] * dolrecomp_force25(ctx->ps1[3])") != NULL, text);
        free(text);
    }

    /* 9. --self-calls and --tail-calls widen --direct-calls. A same-chunk bl
          calls the chunk's own function and resumes on the return address; a
          cross-chunk b calls the target chunk. Neither flag may change
          anything on its own, and a hooked callee still goes through the
          dispatcher. Registers a dispatch PC, so it runs last. */
    {
        static const u32 chunk_starts[] = { 0x8000A000u, 0x8000C000u };
        static const u32 raws9[] = {
            0x4800000Du,  /* 8000A000 bl 0x8000A00C -- same chunk */
            0x60000000u,  /* 8000A004 nop (continuation)          */
            0x4E800020u,  /* 8000A008 blr                         */
            0x60000000u,  /* 8000A00C nop (the local callee)      */
            0x48001FF0u,  /* 8000A010 b 0x8000C000 -- next chunk  */
        };
        emit_set_chunk_table(chunk_starts, 2u);
        emit_set_direct_calls(true);

        char* text = emit_raws_to_string(raws9, 5u, 0x8000A000u);
        if (!text) return 1;
        expect("without --self-calls a same-chunk bl must not call the chunk natively",
               strstr(text, "func_8000A000(ctx);") == NULL, text);
        expect("without --tail-calls a cross-chunk b must not call the target natively",
               strstr(text, "func_8000C000(ctx);") == NULL, text);
        free(text);

        emit_set_self_calls(true);
        emit_set_tail_calls(true);
        text = emit_raws_to_string(raws9, 5u, 0x8000A000u);
        if (!text) return 1;
        expect("--self-calls: a same-chunk bl should call its own chunk function",
               strstr(text, "func_8000A000(ctx);") != NULL, text);
        expect("--self-calls: it must resume inline only on the return address",
               strstr(text, "if (ctx->pc == 0x8000A004u) goto label_8000A004;") != NULL, text);
        expect("--tail-calls: a cross-chunk b should call the target chunk",
               strstr(text, "func_8000C000(ctx);") != NULL, text);
        free(text);

        emit_add_dispatch_pc(0x8000A00Cu);
        emit_add_dispatch_pc(0x8000C000u);
        text = emit_raws_to_string(raws9, 5u, 0x8000A000u);
        if (!text) return 1;
        expect("--self-calls called a hooked PC natively -- that hook can never fire again",
               strstr(text, "func_8000A000(ctx);") == NULL, text);
        expect("--tail-calls branched to a hooked PC natively -- that hook can never fire again",
               strstr(text, "func_8000C000(ctx);") == NULL, text);
        free(text);

        emit_set_self_calls(false);
        emit_set_tail_calls(false);
        emit_set_direct_calls(false);
        emit_set_chunk_table(NULL, 0u);
    }

    /* 10. DOLRECOMP_DOL_NO_RESERVATION lets a MEM_FAST module drop the per-write
           reservation test, which is only sound when no lwarx/stwcx exists. It
           must appear only when the pre-pass says so, and before cpu.h, or the
           accessors never see it. */
    {
        for (int none = 0; none <= 1; none++) {
            FILE* f = tmpfile();
            if (!f) return 1;
            emit_set_no_reservation(none != 0);
            emit_header_for_cpu(f, DOLRECOMP_CPU_GEKKO);
            long n = ftell(f);
            rewind(f);
            char* hdr = (char*)calloc((size_t)n + 1u, 1u);
            if (!hdr || fread(hdr, 1u, (size_t)n, f) != (size_t)n) { fclose(f); free(hdr); return 1; }
            fclose(f);
            const char* def = strstr(hdr, "#define DOLRECOMP_DOL_NO_RESERVATION 1");
            const char* inc = strstr(hdr, "#include \"cpu/cpu.h\"");
            if (none) {
                expect("no-reservation DOL: generated.h must define DOLRECOMP_DOL_NO_RESERVATION",
                       def != NULL, NULL);
                expect("DOLRECOMP_DOL_NO_RESERVATION must come before cpu.h is included",
                       def && inc && def < inc, NULL);
            } else {
                expect("a DOL that may use lwarx/stwcx must NOT get DOLRECOMP_DOL_NO_RESERVATION",
                       def == NULL, NULL);
            }
            free(hdr);
        }
        emit_set_no_reservation(false);
    }

    /* --chunk-overhang: code after the window is emitted as labels only. The
       window's switch must not grow a case for it (the dispatch table does not
       send those PCs here), and a branch from the overhang back INTO the window
       -- the straddling loop this exists for -- must become a local goto. The
       overhang must end at an exit, not run out of budget, and must never
       swallow a PC the run loop hooks. Runs LAST: it registers a dispatch PC. */
    {
        static const u32 ov_raws[] = {
            0x60000000u,  /* 80005000 nop                 window, leader      */
            0x60000000u,  /* 80005004 nop                 window, loop head   */
            0x60000000u,  /* 80005008 nop                 overhang            */
            0x4200FFF8u,  /* 8000500C bdnz 0x80005004     back into the window */
            0x4E800020u,  /* 80005010 blr                 the exit            */
        };
        PPCInst ov[5];
        for (u32 i = 0; i < 5u; i++)
            ov[i] = ppc_decode(ov_raws[i], 0x80005000u + i * 4u);

        emit_set_chunk_overhang(16u);
        expect("overhang runs to the first exit, inclusive",
               emit_overhang_length(ov, 2u, 3u, NULL, 0u) == 3u, NULL);
        emit_set_chunk_overhang(2u);
        expect("an overhang that runs out of budget before an exit is not taken",
               emit_overhang_length(ov, 2u, 3u, NULL, 0u) == 0u, NULL);
        emit_set_chunk_overhang(16u);

        FILE* f = tmpfile();
        if (!f) return 1;
        emit_function(f, ov, 5u, 2u, 0x80005000u);
        long n = ftell(f);
        rewind(f);
        char* text = (char*)calloc((size_t)n + 1u, 1u);
        if (!text || fread(text, 1u, (size_t)n, f) != (size_t)n) { fclose(f); free(text); return 1; }
        fclose(f);
        expect("the window keeps its case", has_case(text, 0x80005000u), text);
        expect("no case for an overhang PC (80005008)", !has_case(text, 0x80005008u), text);
        expect("no case for an overhang PC (8000500C)", !has_case(text, 0x8000500Cu), text);
        expect("the overhang is emitted as labels", strstr(text, "label_8000500C:") != NULL, text);
        const char* be = strstr(text, "label_8000500C:");
        expect("the back-edge into the window is a local goto",
               be && strstr(be, "goto label_80005004;") != NULL, text);
        free(text);

        emit_add_dispatch_pc(0x80005008u);
        expect("the overhang stops before a PC the run loop must see",
               emit_overhang_length(ov, 2u, 3u, NULL, 0u) == 0u, NULL);
        emit_set_chunk_overhang(0u);
    }

    /* --fp-check-once. Off: byte-identical, no fpok labels. On: the second FP
       op of a run jumps past its test on fall-through, but label_ (the switch
       case every FP op has, for the lazy-FPU resume) still tests; a leader and
       anything after mtmsr test on the straight-line path too. */
    {
        char* fp_off = emit_raws_to_string(fp_raws, NFP, FBASE);
        if (!fp_off) return 1;
        expect("--fp-check-once off: no fpok labels", strstr(fp_off, "fpok_") == NULL, fp_off);
        emit_set_fp_check_once(true);
        char* fp_on = emit_raws_to_string(fp_raws, NFP, FBASE);
        emit_set_fp_check_once(false);
        if (!fp_on) { free(fp_off); return 1; }
        expect("the run's second FP op skips its test on fall-through",
               strstr(fp_on, "    goto fpok_80006004;\nlabel_80006004:") != NULL, fp_on);
        const char* l4 = strstr(fp_on, "label_80006004:");
        const char* ok4 = strstr(fp_on, "fpok_80006004:");
        const char* t4 = strstr(fp_on, "ppc_fp_available(ctx, 0x80006004u)");
        expect("label_ still tests (switch entry after an FP exception)",
               l4 && t4 && ok4 && l4 < t4 && t4 < ok4, fp_on);
        expect("the switch still enters at label_, not past the test",
               has_case(fp_on, 0x80006004u), fp_on);
        expect("a leader is never skipped", strstr(fp_on, "goto fpok_80006000;") == NULL, fp_on);
        expect("mtmsr resets it", strstr(fp_on, "goto fpok_8000600C;") == NULL, fp_on);
        expect("every FP op keeps its test",
               strstr(fp_on, "ppc_fp_available(ctx, 0x80006000u)") &&
               strstr(fp_on, "ppc_fp_available(ctx, 0x8000600Cu)") && t4, fp_on);
        free(fp_off);
        free(fp_on);
    }
    {
        static const u32 mid_entry[] = { 0x80007004u };
        emit_set_leader_cases(true);
        emit_set_entry_targets(mid_entry, 1u);
        emit_set_fp_check_once(true);
        char* text = emit_raws_to_string(fp_entry_raws, NFPE, EFBASE);
        emit_set_fp_check_once(false);
        emit_set_entry_targets(NULL, 0u);
        emit_set_leader_cases(false);
        if (!text) return 1;
        expect("the mid-block entry is a switch case", has_case(text, 0x80007004u), text);
        expect("an FP op after a non-FP entry must test (FP may be off there)",
               strstr(text, "goto fpok_80007008;") == NULL, text);
        free(text);
    }

    return failures ? 1 : 0;
}
