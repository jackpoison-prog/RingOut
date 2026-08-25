#include "analysis/idle_loop.h"

/* Small-data base register. The scheduler keeps its wake flag in the SDA, so a
 * spin that loads through r13 is the idle loop; the others load through a
 * register some caller happened to set up. */
#define SDA_BASE_REG 13

int dol_find_idle_pc(const DOLFile* dol, u32* out_pc) {
    u32 found = 0;
    int count = 0;

    for (int i = 0; i < DOL_NUM_TEXT; ++i) {
        const u8* text = dol_get_text_section(dol, i);
        u32 size = dol->header.text_sizes[i];
        u32 addr = dol->header.text_addresses[i];
        if (!text || size < 12)
            continue;

        for (u32 off = 0; off + 12 <= size; off += 4) {
            u32 w0 = read_be32(text + off);
            u32 w1 = read_be32(text + off + 4);
            u32 w2 = read_be32(text + off + 8);

            /* lwz rD, d(r13) */
            if ((w0 >> 26) != 32 || ((w0 >> 16) & 31) != SDA_BASE_REG)
                continue;
            u32 rd = (w0 >> 21) & 31;
            /* cmplwi rD, 0. The signed cmpwi form is deliberately not accepted:
             * both discs measured carry a second r13-relative spin that uses it
             * (GRSEAF 0x80193184, GRSJAF 0x8018C6F0), and accepting it turns a
             * single answer into an ambiguous pair. The scheduler's flag is
             * compared unsigned. */
            if (w1 != (0x28000000u | (rd << 16)))
                continue;
            /* beq -8, i.e. back to the lwz */
            if (w2 != 0x4182FFF8u)
                continue;

            if (count == 0)
                found = addr + off;
            ++count;
        }
    }

    /* More than one candidate is as unusable as none: picking the wrong spin
     * costs the same half of the frame rate, and silently. */
    if (count != 1)
        return 0;
    *out_pc = found;
    return 1;
}
