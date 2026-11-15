/* --gated-pc gate groups. Each gated PC belongs to one of 8 groups and its label tests only that
 * group's bit of dolrecomp_gate_armed, so the chassis can arm one set of hooks (the ultrawide render
 * layers, group 0) without sending another set (Spawn's per-character sites, group 1) to the
 * dispatcher. A PC given without a group is group 0, so existing builds and the chassis's arming value
 * 1 keep their meaning. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/backend/emitter.h"
#include "../src/common/types.h"
#include "../src/frontend/decoder.h"

#define BASE 0x80003000u
static const u32 raws[] = {
    0x60000000u,  /* 80003000 nop   gated, group 0 */
    0x60000000u,  /* 80003004 nop   gated, group 1 */
    0x60000000u,  /* 80003008 nop                  */
    0x4E800020u,  /* 8000300C blr                  */
};
#define NRAW ((u32)(sizeof(raws) / sizeof(raws[0])))

static int failures;
static void expect(const char* what, int ok) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", what); failures++; }
}

static char* emit_to_string(void) {
    PPCInst insts[NRAW];
    for (u32 i = 0; i < NRAW; i++) insts[i] = ppc_decode(raws[i], BASE + i * 4u);
    FILE* f = tmpfile();
    if (!f) return NULL;
    emit_function(f, insts, NRAW, NRAW, BASE);
    long n = ftell(f);
    rewind(f);
    char* buf = (char*)calloc((size_t)n + 1u, 1u);
    if (buf && fread(buf, 1u, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    return buf;
}

/* The text between one label and the next. */
static const char* label_body(const char* text, const char* label, const char* next, char* out, size_t cap) {
    const char* a = strstr(text, label);
    if (!a) return "";
    const char* b = strstr(a, next);
    size_t n = b ? (size_t)(b - a) : strlen(a);
    if (n >= cap) n = cap - 1u;
    memcpy(out, a, n);
    out[n] = '\0';
    return out;
}

int main(void) {
    u32 pc = 0, group = 99;

    expect("plain address parses as group 0",
           emit_parse_gated_pc("0x80003000", &pc, &group) && pc == 0x80003000u && group == 0u);
    expect("@1 parses as group 1",
           emit_parse_gated_pc("0x80003004@1", &pc, &group) && pc == 0x80003004u && group == 1u);
    expect("@7 is the last group", emit_parse_gated_pc("0x80003004@7", &pc, &group) && group == 7u);
    expect("@8 is refused", !emit_parse_gated_pc("0x80003004@8", &pc, &group));
    expect("a non-number group is refused", !emit_parse_gated_pc("0x80003004@x", &pc, &group));
    expect("address 0 is refused", !emit_parse_gated_pc("0", &pc, &group));

    expect("no gated PCs: no groups", emit_gate_group_mask() == 0u);
    emit_add_gated_pc(0x80003000u);
    expect("group 0 only: mask 0x1", emit_gate_group_mask() == 0x1u);
    emit_add_gated_pc_group(0x80003004u, 1u);
    /* The module exports this as staticrecomp_gate_groups; the runtime keeps Spawn off without bit 1. */
    expect("groups 0 and 1: mask 0x3", emit_gate_group_mask() == 0x3u);
    char* text = emit_to_string();
    if (!text) return 1;
    char a[2048], b[2048], c[2048];
    label_body(text, "label_80003000:", "label_80003004:", a, sizeof a);
    label_body(text, "label_80003004:", "label_80003008:", b, sizeof b);
    label_body(text, "label_80003008:", "label_8000300C:", c, sizeof c);
    expect("group 0 label tests bit 0", strstr(a, "dolrecomp_gate_armed & 0x1u") != NULL);
    expect("group 1 label tests bit 1", strstr(b, "dolrecomp_gate_armed & 0x2u") != NULL);
    expect("group 1 label does not test bit 0", strstr(b, "& 0x1u") == NULL);
    expect("an ungated label has no gate", strstr(c, "dolrecomp_gate_armed") == NULL);
    expect("the bypass still names the PC", strstr(b, "dolrecomp_gate_bypass == 0x80003004u") != NULL);
    free(text);

    /* A gated PC must be enterable: the dispatcher re-enters there after its hook. Spawn's sites are
     * mid-block compares, not leaders, so under a reduced entry set they need a case of their own. */
    {
        PPCInst insts[NRAW];
        for (u32 i = 0; i < NRAW; i++) insts[i] = ppc_decode(raws[i], BASE + i * 4u);
        emit_set_leader_cases(true);
        emit_collect_entry_points(insts, NRAW, BASE);
        char path[] = "/tmp/gated_entries_XXXXXX";
        int fd = mkstemp(path);
        expect("entry file written", fd >= 0 && emit_write_entry_points(path));
        char buf[4096] = {0};
        FILE* f = fopen(path, "r");
        if (f) { size_t n = fread(buf, 1u, sizeof buf - 1u, f); buf[n] = '\0'; fclose(f); }
        remove(path);
        expect("a mid-block gated PC is an entry", strstr(buf, "80003004") != NULL);
        expect("an ungated mid-block PC is not", strstr(buf, "80003008") == NULL);
        emit_set_leader_cases(false);
    }

    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("gated_pc: ok\n");
    return 0;
}
