#!/usr/bin/env python3
"""List per-character code sites in a GameCube DOL: places that compare a character ID against Link's
(27) or the empty IDs (28, 29), and jump tables bounded at 27..35 entries, near a load of a
character-ID field. These are the decisions a new character 29 can fall through (tools/spawn/MAP.md,
"Per-character code sites").

    spawn-id-scan.py <main.dol> [--all]      ->  lines "pc kind value anchor"

An anchor is the nearest load, at most WINDOW instructions earlier, of a halfword/byte/word at one of
the ID fields (player struct +0x58/+0x5A/+0x5C, select default +0x90, descriptor model IDs
+0x82/+0x86/+0x88). --all also lists compares with no anchor (IDs often arrive in a register)."""
import struct, sys
from collections import namedtuple

ID_VALUES = (27, 28, 29)
ID_FIELDS = (0x58, 0x5A, 0x5C, 0x82, 0x86, 0x88, 0x90)
LOAD_OPS = (32, 34, 40, 42)          # lwz, lbz, lhz, lha
WINDOW = 32
BCTR = 0x4E800420

Site = namedtuple('Site', 'pc kind value anchor')


def text_words(path):
    with open(path, 'rb') as f: d = f.read()
    off, addr, size = (struct.unpack_from('>7I', d, o) for o in (0x00, 0x48, 0x90))
    for o, a, s in zip(off, addr, size):
        if s:
            yield a, struct.unpack_from('>%dI' % (s // 4), d, o)


def scan(path, all_sites=False):
    sites = []
    for base, w in text_words(path):
        for i, x in enumerate(w):
            op, imm = x >> 26, x & 0xffff
            if op not in (10, 11):                      # cmplwi, cmpwi
                continue
            ra = (x >> 16) & 31
            kind = None
            if op == 10 and 26 <= imm <= 34 and BCTR in w[i + 1:i + 13]:
                kind, value = 'switch', imm + 1
            elif imm in ID_VALUES:
                kind, value = 'cmp', imm
            if kind is None:
                continue
            anchor = None
            for j in range(i - 1, max(-1, i - 1 - WINDOW), -1):
                y = w[j]
                if y >> 26 in LOAD_OPS and (y >> 21) & 31 == ra and (y & 0xffff) in ID_FIELDS:
                    anchor = base + 4 * j; break
            if anchor is not None or all_sites:
                sites.append(Site(base + 4 * i, kind, value, anchor))
    return sites


if __name__ == '__main__':
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for s in scan(sys.argv[1], '--all' in sys.argv[2:]):
        print('%08X %s %d %s' % (s.pc, s.kind, s.value, '%08X' % s.anchor if s.anchor else '-'))
