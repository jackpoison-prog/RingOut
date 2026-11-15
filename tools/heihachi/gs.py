"""GS memory model: replay a PS2 model's texture uploads, and read the textures back as RGBA.

The model uploads its textures as 32-bit data and reads them as 8- or 4-bit indexed pictures of the same
memory, so they can only be read by laying them out as the GS does. The tables are the GS's address swizzle."""
import struct

B32 = (
    (0, 1, 4, 5, 16, 17, 20, 21),
    (2, 3, 6, 7, 18, 19, 22, 23),
    (8, 9, 12, 13, 24, 25, 28, 29),
    (10, 11, 14, 15, 26, 27, 30, 31),
)
C32 = (
    (0, 1, 4, 5, 8, 9, 12, 13),
    (2, 3, 6, 7, 10, 11, 14, 15),
    (16, 17, 20, 21, 24, 25, 28, 29),
    (18, 19, 22, 23, 26, 27, 30, 31),
    (32, 33, 36, 37, 40, 41, 44, 45),
    (34, 35, 38, 39, 42, 43, 46, 47),
    (48, 49, 52, 53, 56, 57, 60, 61),
    (50, 51, 54, 55, 58, 59, 62, 63),
)
B8 = (
    (0, 1, 4, 5, 16, 17, 20, 21),
    (2, 3, 6, 7, 18, 19, 22, 23),
    (8, 9, 12, 13, 24, 25, 28, 29),
    (10, 11, 14, 15, 26, 27, 30, 31),
)
C8 = (
    (0, 4, 16, 20, 32, 36, 48, 52, 2, 6, 18, 22, 34, 38, 50, 54),
    (8, 12, 24, 28, 40, 44, 56, 60, 10, 14, 26, 30, 42, 46, 58, 62),
    (33, 37, 49, 53, 1, 5, 17, 21, 35, 39, 51, 55, 3, 7, 19, 23),
    (41, 45, 57, 61, 9, 13, 25, 29, 43, 47, 59, 63, 11, 15, 27, 31),
    (96, 100, 112, 116, 64, 68, 80, 84, 98, 102, 114, 118, 66, 70, 82, 86),
    (104, 108, 120, 124, 72, 76, 88, 92, 106, 110, 122, 126, 74, 78, 90, 94),
    (65, 69, 81, 85, 97, 101, 113, 117, 67, 71, 83, 87, 99, 103, 115, 119),
    (73, 77, 89, 93, 105, 109, 121, 125, 75, 79, 91, 95, 107, 111, 123, 127),
    (128, 132, 144, 148, 160, 164, 176, 180, 130, 134, 146, 150, 162, 166, 178, 182),
    (136, 140, 152, 156, 168, 172, 184, 188, 138, 142, 154, 158, 170, 174, 186, 190),
    (161, 165, 177, 181, 129, 133, 145, 149, 163, 167, 179, 183, 131, 135, 147, 151),
    (169, 173, 185, 189, 137, 141, 153, 157, 171, 175, 187, 191, 139, 143, 155, 159),
    (224, 228, 240, 244, 192, 196, 208, 212, 226, 230, 242, 246, 194, 198, 210, 214),
    (232, 236, 248, 252, 200, 204, 216, 220, 234, 238, 250, 254, 202, 206, 218, 222),
    (193, 197, 209, 213, 225, 229, 241, 245, 195, 199, 211, 215, 227, 231, 243, 247),
    (201, 205, 217, 221, 233, 237, 249, 253, 203, 207, 219, 223, 235, 239, 251, 255),
)
B4 = (
    (0, 2, 8, 10),
    (1, 3, 9, 11),
    (4, 6, 12, 14),
    (5, 7, 13, 15),
    (16, 18, 24, 26),
    (17, 19, 25, 27),
    (20, 22, 28, 30),
    (21, 23, 29, 31),
)
C4 = (
    (0, 8, 32, 40, 64, 72, 96, 104, 2, 10, 34, 42, 66, 74, 98, 106, 4, 12, 36, 44, 68, 76, 100, 108, 6, 14, 38, 46, 70, 78, 102, 110),
    (16, 24, 48, 56, 80, 88, 112, 120, 18, 26, 50, 58, 82, 90, 114, 122, 20, 28, 52, 60, 84, 92, 116, 124, 22, 30, 54, 62, 86, 94, 118, 126),
    (65, 73, 97, 105, 1, 9, 33, 41, 67, 75, 99, 107, 3, 11, 35, 43, 69, 77, 101, 109, 5, 13, 37, 45, 71, 79, 103, 111, 7, 15, 39, 47),
    (81, 89, 113, 121, 17, 25, 49, 57, 83, 91, 115, 123, 19, 27, 51, 59, 85, 93, 117, 125, 21, 29, 53, 61, 87, 95, 119, 127, 23, 31, 55, 63),
    (192, 200, 224, 232, 128, 136, 160, 168, 194, 202, 226, 234, 130, 138, 162, 170, 196, 204, 228, 236, 132, 140, 164, 172, 198, 206, 230, 238, 134, 142, 166, 174),
    (208, 216, 240, 248, 144, 152, 176, 184, 210, 218, 242, 250, 146, 154, 178, 186, 212, 220, 244, 252, 148, 156, 180, 188, 214, 222, 246, 254, 150, 158, 182, 190),
    (129, 137, 161, 169, 193, 201, 225, 233, 131, 139, 163, 171, 195, 203, 227, 235, 133, 141, 165, 173, 197, 205, 229, 237, 135, 143, 167, 175, 199, 207, 231, 239),
    (145, 153, 177, 185, 209, 217, 241, 249, 147, 155, 179, 187, 211, 219, 243, 251, 149, 157, 181, 189, 213, 221, 245, 253, 151, 159, 183, 191, 215, 223, 247, 255),
    (256, 264, 288, 296, 320, 328, 352, 360, 258, 266, 290, 298, 322, 330, 354, 362, 260, 268, 292, 300, 324, 332, 356, 364, 262, 270, 294, 302, 326, 334, 358, 366),
    (272, 280, 304, 312, 336, 344, 368, 376, 274, 282, 306, 314, 338, 346, 370, 378, 276, 284, 308, 316, 340, 348, 372, 380, 278, 286, 310, 318, 342, 350, 374, 382),
    (321, 329, 353, 361, 257, 265, 289, 297, 323, 331, 355, 363, 259, 267, 291, 299, 325, 333, 357, 365, 261, 269, 293, 301, 327, 335, 359, 367, 263, 271, 295, 303),
    (337, 345, 369, 377, 273, 281, 305, 313, 339, 347, 371, 379, 275, 283, 307, 315, 341, 349, 373, 381, 277, 285, 309, 317, 343, 351, 375, 383, 279, 287, 311, 319),
    (448, 456, 480, 488, 384, 392, 416, 424, 450, 458, 482, 490, 386, 394, 418, 426, 452, 460, 484, 492, 388, 396, 420, 428, 454, 462, 486, 494, 390, 398, 422, 430),
    (464, 472, 496, 504, 400, 408, 432, 440, 466, 474, 498, 506, 402, 410, 434, 442, 468, 476, 500, 508, 404, 412, 436, 444, 470, 478, 502, 510, 406, 414, 438, 446),
    (385, 393, 417, 425, 449, 457, 481, 489, 387, 395, 419, 427, 451, 459, 483, 491, 389, 397, 421, 429, 453, 461, 485, 493, 391, 399, 423, 431, 455, 463, 487, 495),
    (401, 409, 433, 441, 465, 473, 497, 505, 403, 411, 435, 443, 467, 475, 499, 507, 405, 413, 437, 445, 469, 477, 501, 509, 407, 415, 439, 447, 471, 479, 503, 511),
)


def a32(block, width, x, y):
    w = width or 1; page = (block >> 5) + (y >> 5) * w + (x >> 6); bid = (block & 31) + B32[(y >> 3) & 3][(x >> 3) & 7]
    return (page << 13) + ((bid >> 5) << 13) + (bid & 31) * 256 + C32[y & 7][x & 7] * 4


def a8(block, width, x, y):
    w = (width >> 1) or 1; page = (block >> 5) + (y >> 6) * w + (x >> 7); bid = (block & 31) + B8[(y >> 4) & 3][(x >> 4) & 7]
    return (page << 13) + ((bid >> 5) << 13) + (bid & 31) * 256 + C8[y & 15][x & 15]


def a4(block, width, x, y):
    """Address in nibbles."""
    w = (width >> 1) or 1; page = (block >> 5) + (y >> 7) * w + (x >> 7); bid = (block & 31) + B4[(y >> 4) & 7][(x >> 5) & 3]
    return (page << 14) + ((bid >> 5) << 14) + (bid & 31) * 512 + C4[y & 15][x & 31]


PSMT8, PSMT4 = 19, 20
MEM = 4 << 20


class GS:
    def __init__(self): self.m = bytearray(MEM)

    def upload32(self, data, dbp, dbw, w, h):
        m = self.m
        for y in range(h):
            for x in range(w):
                a = a32(dbp, dbw, x, y) & (MEM - 4); s = (y * w + x) * 4
                m[a:a + 4] = data[s:s + 4]

    def clut(self, cbp, n):
        m, out = self.m, []
        if n == 256:
            for i in range(256):
                j = (i & 0xe7) | ((i & 8) << 1) | ((i & 16) >> 1)
                a = a32(cbp, 1, j & 15, j >> 4); out.append(bytes(m[a:a + 4]))
        else:
            for i in range(16):
                a = a32(cbp, 1, i & 7, i >> 3); out.append(bytes(m[a:a + 4]))
        return out

    def read(self, t):
        """Rows of (r, g, b, a) for a TEX0 dict. The GS's full alpha is 128."""
        w, h, psm, tbp, tbw, m = t['tw'], t['th'], t['psm'], t['tbp'], t['tbw'], self.m
        if psm == PSMT8:
            c = self.clut(t['cbp'], 256)
            get = lambda x, y: c[m[a8(tbp, tbw, x, y)]]
        elif psm == PSMT4:
            c = self.clut(t['cbp'], 16)
            def get(x, y):
                a = a4(tbp, tbw, x, y); v = m[a >> 1]
                return c[(v >> 4) if a & 1 else (v & 15)]
        else:
            def get(x, y):
                a = a32(tbp, tbw, x, y)
                return m[a:a + 4]
        rows = []
        for y in range(h):
            row = []
            for x in range(w):
                p = get(x, y); row.append((p[0], p[1], p[2], min(255, p[3] * 2)))
            rows.append(row)
        return rows


def clamp(lo, hi):
    """CLAMP_1/2: wrap modes (0 repeat, 1 clamp, 2 region clamp, 3 region repeat) and the region."""
    v = lo | (hi << 32)
    return dict(wms=v & 3, wmt=(v >> 2) & 3, minu=(v >> 4) & 0x3ff, maxu=(v >> 14) & 0x3ff, minv=(v >> 24) & 0x3ff,
                maxv=(v >> 34) & 0x3ff)


def region_clamped(rows, c):
    """A picture as the GS samples it under REGION_CLAMP: texels outside the region repeat its edge."""
    h, w = len(rows), len(rows[0])
    u0, u1 = (c['minu'], min(c['maxu'], w - 1)) if c['wms'] == 2 else (0, w - 1)
    v0, v1 = (c['minv'], min(c['maxv'], h - 1)) if c['wmt'] == 2 else (0, h - 1)
    if (u0, u1, v0, v1) == (0, w - 1, 0, h - 1): return rows
    return [[rows[min(max(y, v0), v1)][min(max(x, u0), u1)] for x in range(w)] for y in range(h)]


def tex0(lo, hi):
    v = lo | (hi << 32)
    return dict(tbp=v & 0x3fff, tbw=(v >> 14) & 63, psm=(v >> 20) & 63, tw=1 << ((v >> 26) & 15), th=1 << ((v >> 30) & 15),
                cbp=(v >> 37) & 0x3fff)


def _run_call(g, P, bank, c):
    """Apply one DMA call's uploads. Returns their (dbp, dbw, w, h)."""
    u = lambda a: struct.unpack_from('<I', P, a)[0]
    a = bank + c; cur = None; done = []
    while True:
        t = u(a); ty, q = t >> 28, t & 0xffff
        if ty == 6: return done
        if ty == 3:
            d = bank + u(a + 4); dbp, dbw, w, h = cur
            g.upload32(P[d:d + q * 16], dbp, dbw, w, h); done.append(cur)
            a += 16; continue
        x, e = a + 16, a + 16 + q * 16   # a GIF packet in line: only the registers of the next upload matter
        while x < e:
            lo, hi, r = u(x), u(x + 4), u(x + 8)
            if r == 0x50: dbp, dbw = hi & 0x3fff, (hi >> 16) & 63
            elif r == 0x52: cur = (dbp, dbw, lo, hi)
            x += 16
        a = e


def _group_calls(P, bank):
    """A model's upload area (file offset `bank`): u8 group count at +4, then from +0x20 one DMA call list per group,
    each ended by a RET tag. A draw list reads the group of its own number; the groups are loaded in turn at the same
    blocks, so each needs a GS of its own."""
    u = lambda a: struct.unpack_from('<I', P, a)[0]
    a, out = bank + 0x20, []
    for _ in range(P[bank + 4]):
        calls = []
        while u(a) >> 28 == 5:
            calls.append(u(a + 4)); a += 16
        if u(a) >> 28 != 6: raise ValueError('upload list: DMA tag 0x%08X at 0x%X' % (u(a), a))
        out.append(calls); a += 16
    return out


def replay(P, bank):
    """One GS per upload group."""
    out = []
    for calls in _group_calls(P, bank):
        g = GS(); out.append(g)
        for c in calls: _run_call(g, P, bank, c)
    return out


def upload_groups(P, bank):
    """Per group, its uploads' (dbp, dbw, w, h)."""
    out = []
    for calls in _group_calls(P, bank):
        g = GS(); out.append([x for c in calls for x in _run_call(g, P, bank, c)])
    return out


def bank_calls(part):
    """A texture bank's upload calls in texture order. u8 group count at +4, u8 texture count at +5, a group table at
    +8 of (u32 offset of its call list, u32 0, u16 count, u16 base block)."""
    u = lambda a: struct.unpack_from('<I', part, a)[0]
    groups, textures, table = part[4], part[5], u(8)
    calls = []
    for k in range(groups):
        at, count = u(table + 12 * k), struct.unpack_from('<H', part, table + 12 * k + 8)[0]
        calls += [u(at + 16 * i + 4) for i in range(count)]
    if len(calls) != textures:
        raise ValueError('texture bank lists %d textures, its groups hold %d' % (textures, len(calls)))
    return calls


def indexed_texture(part, k, psm, w, h):
    """Texture k of a bank, whose uploads are its pixels and then its palette: (flat row-major indices, palette of
    (r, g, b, a)). Each texture is uploaded alone, so the bank must be replayed one call at a time."""
    g = GS(); ups = _run_call(g, part, 0, bank_calls(part)[k])
    if len(ups) != 2: raise ValueError('texture %d has %d uploads, expected pixels and palette' % (k, len(ups)))
    tbp, cbp, tbw = ups[0][0], ups[1][0], max(1, w // 64)
    if psm == PSMT8:
        idx = [g.m[a8(tbp, tbw, x, y)] for y in range(h) for x in range(w)]; n = 256
    else:
        idx = []
        for y in range(h):
            for x in range(w):
                a = a4(tbp, tbw, x, y); v = g.m[a >> 1]; idx.append((v >> 4) if a & 1 else (v & 15))
        n = 16
    return idx, [(c[0], c[1], c[2], min(255, c[3] * 2)) for c in g.clut(cbp, n)]
