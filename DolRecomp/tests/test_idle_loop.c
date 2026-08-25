/* The idle-loop scan behind --idle-pc auto.
 *
 * Getting this wrong is silent: the module builds and runs, and is about half
 * as fast, because back-edges to an unmarked idle loop are compiled as native
 * gotos and the host never sees the guest idling. So the scan has to be right
 * about which spin it picked, and honest when it cannot tell. */
#include "analysis/idle_loop.h"
#include "frontend/container/dol.h"
#include "common/types.h"
#include <stdio.h>
#include <string.h>

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
        return 0; \
    } \
} while (0)

#define TEXT_BASE 0x80003100u

static int write_text_dol(const char* path, const u32* words, u32 word_count) {
    u8 file[0x100 + 64];
    u32 size = word_count * 4;

    CHECK(size <= sizeof(file) - 0x100, "test text section too large");
    memset(file, 0, sizeof(file));
    write_be32(file + 0x00, 0x100);
    write_be32(file + 0x48, TEXT_BASE);
    write_be32(file + 0x90, size);
    write_be32(file + 0xE0, TEXT_BASE);
    for (u32 i = 0; i < word_count; ++i)
        write_be32(file + 0x100 + i * 4, words[i]);

    FILE* out = fopen(path, "wb");
    if (!out)
        return 0;
    int ok = fwrite(file, 1, 0x100 + size, out) == 0x100 + size;
    ok = fclose(out) == 0 && ok;
    return ok;
}

/* Returns 1 when a loop was found, and writes it to *pc. */
static int scan_words(const u32* words, u32 word_count, u32* pc) {
    const char* path = "test_idle_loop.dol";
    DOLFile dol;
    int found;

    CHECK(write_text_dol(path, words, word_count), "failed to write test DOL");
    CHECK(dol_load(&dol, path), "failed to load test DOL");
    found = dol_find_idle_pc(&dol, pc);
    dol_free(&dol);
    remove(path);
    return found;
}

static int test_finds_the_loop(void) {
    /* The shape SOULCALIBUR II's scheduler parks in, preceded by two spins that
     * look like it and are not: one reads its flag through a general register
     * rather than the small-data base, and one compares signed. Both discs
     * measured carry the signed variant (GRSEAF 0x80193184, GRSJAF 0x8018C6F0),
     * which is why it has to be excluded rather than merely tie-broken. */
    static const u32 words[] = {
        0x80030000u, 0x28000000u, 0x4182FFF8u,  /* lwz r0, 0(r3)      spin */
        0x800D8BF0u, 0x2C000000u, 0x4182FFF8u,  /* cmpwi, not cmplwi  spin */
        0x800D8BF0u, 0x28000000u, 0x4182FFF8u,  /* the idle loop           */
    };
    u32 pc = 0;
    CHECK(scan_words(words, 9, &pc), "idle loop not found");
    CHECK(pc == TEXT_BASE + 24, "found 0x%08X, expected 0x%08X", pc, TEXT_BASE + 24);
    return 1;
}

static int test_no_loop(void) {
    static const u32 words[] = {0x38600001u, 0x4E800020u, 0x60000000u};
    u32 pc = 0xDEADBEEFu;
    CHECK(!scan_words(words, 3, &pc), "reported a loop in code that has none");
    CHECK(pc == 0xDEADBEEFu, "clobbered the output on failure");
    return 1;
}

static int test_ambiguous(void) {
    /* Two of the real shape: no answer, because picking the wrong one costs the
     * same frame rate as picking none and hides the fact that it happened. */
    static const u32 words[] = {
        0x800D8BF0u, 0x28000000u, 0x4182FFF8u,
        0x800D8C50u, 0x28000000u, 0x4182FFF8u,
    };
    u32 pc = 0;
    CHECK(!scan_words(words, 6, &pc), "picked one of two equal candidates");
    return 1;
}

int main(void) {
    int ok = 1;
    ok &= test_finds_the_loop();
    ok &= test_no_loop();
    ok &= test_ambiguous();

    if (!ok)
        return 1;

    printf("idle loop scan tests passed\n");
    return 0;
}
