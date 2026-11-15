"""Xbox VXT texture bank -> GameCube VGT (DXT1 -> CMPR; DXT3 -> CMPR + I4 alpha; DXT5 -> CMPR + CMPR alpha)."""
import struct
CMPR, I4 = 0xe, 0x0
D3D_DXT1, D3D_DXT3, D3D_DXT5 = 0x0c, 0x0e, 0x0f

def rev_pairs(b): return ((b & 3) << 6) | ((b & 0xc) << 2) | ((b & 0x30) >> 2) | ((b & 0xc0) >> 6)

def fix4(c0, c1, rows):
    """A DXT3/5 colour block always decodes 4-colour; CMPR uses 3-colour+transparent when c0 <= c1."""
    if c0 > c1: return c0, c1, rows
    if c0 == c1: return c0, c1, bytes(4) if c0 else (c0 | 0, c1, bytes(4))[2]
    # swap endpoints: index 0<->1, 2<->3
    m = {0: 1, 1: 0, 2: 3, 3: 2}
    out = bytes(sum(m[(r >> (2*k)) & 3] << (2*k) for k in range(4)) for r in rows)
    return c1, c0, out

def cmpr_from_blocks(blocks, bw, bh):
    """blocks[by][bx] = (c0, c1, rows DXT order) -> CMPR bytes (8x8 tiles of 2x2 blocks)."""
    out = bytearray()
    for ty in range(0, max(bh, 2), 2):
        for tx in range(0, max(bw, 2), 2):
            for sub in range(4):
                bx, by = tx + (sub & 1), ty + (sub >> 1)
                if bx < bw and by < bh:
                    c0, c1, rows = blocks[by][bx]
                    out += struct.pack('>HH', c0, c1) + bytes(rev_pairs(r) for r in rows)
                else:
                    out += bytes(8)
    return bytes(out)

def grey_block(vals):
    lo, hi = min(vals), max(vals)
    def c565(v): r = v >> 3; g = v >> 2; return (r << 11) | (g << 5) | r
    def dec(c): r = (c >> 11) & 31; return (r << 3) | (r >> 2)
    c0, c1 = c565(hi), c565(lo)
    if c0 == c1:
        return (c0, c1, bytes(4)) if c0 else (0x0020, 0, bytes([0x55] * 4))  # c0>c1 needed; all index 1 = black
    a, b = dec(c0), dec(c1); pal = [a, b, (2*a + b)//3, (a + 2*b)//3]
    rows = []
    for y in range(4):
        r = 0
        for xx in range(4):
            v = vals[4*y + xx]; k = min(range(4), key=lambda i: abs(pal[i] - v)); r |= k << (2*xx)
        rows.append(r)
    return c0, c1, bytes(rows)

def dxt5_alpha(blk):
    a0, a1 = blk[0], blk[1]; bits = int.from_bytes(blk[2:8], 'little')
    if a0 > a1: pal = [a0, a1] + [((6 - i) * a0 + (1 + i) * a1) // 7 for i in range(6)]
    else: pal = [a0, a1] + [((4 - i) * a0 + (1 + i) * a1) // 5 for i in range(4)] + [0, 255]
    return [pal[(bits >> (3*i)) & 7] for i in range(16)]

def level_dims(w, h, l): return max(1, w >> l), max(1, h >> l)

def convert_level(data, at, w, h, fmt):
    bw, bh = max(1, (w + 3)//4), max(1, (h + 3)//4)
    bs = 8 if fmt == D3D_DXT1 else 16
    col = [[None]*bw for _ in range(bh)]; alpha = [[None]*bw for _ in range(bh)]
    for by in range(bh):
        for bx in range(bw):
            blk = data[at + (by*bw + bx)*bs: at + (by*bw + bx + 1)*bs]
            cb = blk[-8:]; c0, c1 = struct.unpack_from('<HH', cb, 0); rows = cb[4:8]
            if fmt != D3D_DXT1: c0, c1, rows = fix4(c0, c1, rows)
            col[by][bx] = (c0, c1, rows)
            if fmt == D3D_DXT3:
                alpha[by][bx] = [((blk[i//2] >> (4*(i & 1))) & 15) for i in range(16)]
            elif fmt == D3D_DXT5:
                alpha[by][bx] = dxt5_alpha(blk[:8])
    colour = cmpr_from_blocks(col, bw, bh)
    a = b''
    if fmt == D3D_DXT5:
        a = cmpr_from_blocks([[grey_block(alpha[by][bx]) for bx in range(bw)] for by in range(bh)], bw, bh)
    elif fmt == D3D_DXT3:
        # I4: 8x8 tiles, rows of 4 bytes, high nibble first
        out = bytearray()
        tw, th = (w + 7)//8, (h + 7)//8
        def texel(xx, yy):
            if xx >= w or yy >= h: return 0
            return alpha[yy//4][xx//4][(yy % 4)*4 + xx % 4]
        for ty in range(th):
            for tx in range(tw):
                for yy in range(8):
                    for xx in range(0, 8, 2):
                        X, Y = tx*8 + xx, ty*8 + yy
                        out.append((texel(X, Y) << 4) | texel(X + 1, Y))
        a = bytes(out)
    return colour, a, bw*bh*bs

def vxt_to_vgt(v, drop_top=frozenset()):
    """Xbox VXT -> GC VGT. Textures whose index is in `drop_top` lose their top mip level: the next level,
    already in the file, becomes the base (half the size, no re-encoding; see build_spawn.GC_MODEL_MAX)."""
    assert v[:4] == b'VXT.', v[:4]
    n = struct.unpack_from('<I', v, 8)[0]
    ents = []
    for k in range(n):
        e = v[0x14 + 0x24*k: 0x14 + 0x24*(k+1)]
        wh = struct.unpack_from('<I', e, 0x0c)[0]; w, h = wh & 0xffff, wh >> 16
        off, fmt = struct.unpack_from('<II', e, 0x10); mips = struct.unpack_from('<I', e, 0x1c)[0] or 1
        ents.append((w, h, off, fmt, mips, struct.unpack_from('<I', e, 0)[0]))
    table_end = 0x18 + 0x44*n
    data = bytearray(); recs = []
    base = (table_end + 0x1f) & ~0x1f
    for k, (w, h, off, fmt, mips, palhdr) in enumerate(ents):
        if fmt not in (D3D_DXT1, D3D_DXT3, D3D_DXT5, 0x0b):
            raise ValueError('texture format %#x not handled' % fmt)
        cols = []; alps = []; at = off
        if k in drop_top:
            if mips < 2:
                raise ValueError('texture %d has one mip level: nothing to drop' % k)
            bw, bh = max(1, (w + 3)//4), max(1, (h + 3)//4)
            at += w*h if fmt == 0x0b else bw*bh*(8 if fmt == D3D_DXT1 else 16)
            w, h, mips = max(1, w // 2), max(1, h // 2), mips - 1
        if fmt == 0x0b:
            # P8 (swizzled, D3D palette object at palhdr: u32 data offset, u32 entries) -> CMPR (+ CMPR alpha)
            pal_off = struct.unpack_from('<I', v, palhdr)[0]
            imgs = []
            for l in range(mips):
                lw, lh = level_dims(w, h, l); imgs.append((p8_rgba(v, at, pal_off, lw, lh), lw, lh)); at += lw*lh
            has_alpha = any(p[3] < 250 for img, _, _ in imgs for row in img for p in row)
            for img, lw, lh in imgs:
                cols.append(rgba_to_cmpr(img, lw, lh))
                if has_alpha: alps.append(rgba_to_cmpr(img, lw, lh, alpha=True))
            fmt = D3D_DXT5 if has_alpha else D3D_DXT1
        else:
          for l in range(mips):
            lw, lh = level_dims(w, h, l)
            c, a, used = convert_level(v, at, lw, lh, fmt); at += used; cols.append(c); alps.append(a)
        data += bytes(-len(data) % 32); coff = base + len(data); data += b''.join(cols)
        aoff = 0
        if fmt != D3D_DXT1:
            data += bytes(-len(data) % 32); aoff = base + len(data); data += b''.join(alps)
        mode0 = 0x190 if mips == 1 else 0x1d0
        mode1 = ((mips - 1) * 16) << 8
        img = lambda f: (f << 20) | ((h - 1) << 10) | (w - 1)
        size = ((w*h)//64 << 16) | (mips + 1)
        if fmt == D3D_DXT1:
            words = [0, mode0, mode1, img(CMPR), coff, 0x0e, 0x0e, 0, size, 0, 0, 0, 0, (w << 16) | h, 0, 0, 0]
        elif fmt == D3D_DXT5:
            words = [0, mode0, mode1, img(CMPR), coff, 0x0002000c, 0x0e, 0, size, mode0, mode1, img(CMPR), aoff, 0, 0x0e, 0, size]
        else:
            words = [0, mode0, mode1, img(CMPR), coff, 0x0022000d, 0x0e, 0, size, mode0, mode1, img(I4), aoff, 0, 0, 0, size | 0x100]
        recs.append(struct.pack('>17I', *words))
    hdr = b'VGT.' + bytes((4, 0, 0, 0)) + struct.pack('>IIII', n, 0, 0x18, table_end)
    out = bytearray(hdr + b''.join(recs)); out += bytes(base - len(out)); out += data
    return bytes(out), None

def unswizzle_index(x, y, w, h):
    """Xbox (Morton) swizzle: interleave x and y bits while both have bits left."""
    off = 0; bit = 0; mw, mh = w - 1, h - 1; sx = sy = 1
    i = 0
    while mw or mh:
        if mw: off |= (x & 1) << i; i += 1; x >>= 1; mw >>= 1
        if mh: off |= (y & 1) << i; i += 1; y >>= 1; mh >>= 1
    return off

def p8_rgba(v, off, pal_off, w, h):
    pal = [struct.unpack_from('<I', v, pal_off + 4*i)[0] for i in range(256)]
    out = []
    for y in range(h):
        row = []
        for xx in range(w):
            c = pal[v[off + unswizzle_index(xx, y, w, h)]]
            row.append(((c >> 16) & 255, (c >> 8) & 255, c & 255, c >> 24))
        out.append(row)
    return out

def enc565(r, g, b): return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
def dec565(c):
    r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))

def colour_block(px):
    """DXT1 4-colour block from 16 RGB tuples: endpoints along the luminance extremes."""
    lum = [p[0]*3 + p[1]*6 + p[2] for p in px]
    hi = px[max(range(16), key=lambda i: lum[i])]; lo = px[min(range(16), key=lambda i: lum[i])]
    c0, c1 = enc565(*hi[:3]), enc565(*lo[:3])
    if c0 == c1:
        if c0 == 0: return 1, 0, bytes([0x55]*4)  # all colour 1 (black), 4-colour mode
        return c0, c1, bytes(4)
    if c0 < c1: c0, c1 = c1, c0
    a, b = dec565(c0), dec565(c1)
    pal = [a, b, tuple((2*x + y)//3 for x, y in zip(a, b)), tuple((x + 2*y)//3 for x, y in zip(a, b))]
    rows = []
    for yy in range(4):
        r = 0
        for xx in range(4):
            p = px[4*yy + xx]
            k = min(range(4), key=lambda i: sum((p[j] - pal[i][j])**2 for j in range(3)))
            r |= k << (2*xx)
        rows.append(r)
    return c0, c1, bytes(rows)

def rgba_to_cmpr(img, w, h, alpha=False):
    bw, bh = max(1, (w + 3)//4), max(1, (h + 3)//4)
    blocks = []
    for by in range(bh):
        row = []
        for bx in range(bw):
            px = [img[min(h-1, by*4 + yy)][min(w-1, bx*4 + xx)] for yy in range(4) for xx in range(4)]
            if alpha: row.append(grey_block([p[3] for p in px]))
            else: row.append(colour_block(px))
        blocks.append(row)
    return cmpr_from_blocks(blocks, bw, bh)
