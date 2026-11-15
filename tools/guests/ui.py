"""Character data outside the 39-entry character block (see MAP.md for how each was found).

Ending illustrations: group 2, entry 57 + character ID, on both discs (Talim 22 -> 79, Link 27 -> 84 on
GC, Spawn 29 -> 86 on Xbox; GC's record 86 exists and is empty). A two-part pack: part 0 a texture file
with the two ending pages, part 1 a 2 KB block that is byte-identical across platforms.
  Xbox: VXT, 2 x 512x512 P8, Morton-swizzled, D3D palette (u32 ARGB, little-endian).
  GC:   VGT, 2 x 640x448 C8 in 8x4 tiles; per page two 256-entry palette halves of u8 pairs, (G, R) then
        (A, B), page k's halves at 0xe0 + 0x400*k and 0x2e0 + 0x400*k.
The ending TEXT (group 2 entry 87) is identical on both discs and already holds Spawn's ending.
"""
import struct
from olk import pack
import texconv

ENDING_ART_BASE = 57
ENDING_TEXT = (2, 87)
GC_W, GC_H = 640, 448
XB_W, XB_H = 512, 512
_PIXELS = (0x8e0, 0x468e0)       # page offsets inside the GC texture file (template layout, asserted)


def locate(gc):
    """Where each per-character item lives on the GC disc."""
    return {'ending_art': lambda char_id: (2, ENDING_ART_BASE + char_id), 'ending_text': ENDING_TEXT}


def _parts(entry, be):
    n = struct.unpack_from('>I' if be else '<I', entry, 0)[0]
    offs = struct.unpack_from(('>' if be else '<') + '%dI' % (n + 1), entry, 4)
    return [(offs[i], offs[i + 1]) for i in range(n)]


def _template_layout(tex):
    """Check the GC template is the layout this converter writes (both pages C8 640x448)."""
    n = struct.unpack_from('>I', tex, 8)[0]
    assert n == 2, n
    for k in range(2):
        w = struct.unpack_from('>17I', tex, 0x18 + 0x44 * k)
        assert w[3] >> 20 == 9 and (w[3] & 0x3ff) + 1 == GC_W and ((w[3] >> 10) & 0x3ff) + 1 == GC_H, w
        assert w[4] == _PIXELS[k], hex(w[4])


# Images below are flat row-major lists of palette indices; palettes are lists of (r, g, b, a).

def _xbox_page(vxt, k):
    """(indices, palette) of Xbox page k (XB_W x XB_H)."""
    return _xbox_p8(vxt, k, XB_W, XB_H)


def _tiles8(img, w, h):
    """w x h C8 indices -> GX 8x4 tile order."""
    return bytes(img[(ty * 4 + r) * w + tx * 8 + c]
                 for ty in range(h // 4) for tx in range(w // 8) for r in range(4) for c in range(8))


def _untile8(raw, w, h):
    """GX 8x4 C8 tiles -> flat row-major indices."""
    img = [0] * (w * h); i = 0
    for ty in range(h // 4):
        for tx in range(w // 8):
            for r in range(4):
                for c in range(8):
                    img[(ty * 4 + r) * w + tx * 8 + c] = raw[i]; i += 1
    return img


def _gr_ab(pal):
    """GC palette halves: (G, R) pairs and (A, B) pairs."""
    return bytes(v for p in pal for v in (p[1], p[0])), bytes(v for p in pal for v in (p[3], p[2]))


def _gc_rgb(gr, ab, c):
    """The first c colours of a GC split palette as (r, g, b)."""
    return [(gr[2 * i + 1], gr[2 * i], ab[2 * i + 1]) for i in range(c)]


def _rows(img, w, pix):
    """Flat index image -> rows of pix[index]."""
    return [[pix[v] for v in img[y * w:(y + 1) * w]] for y in range(len(img) // w)]


def ending_art(xbox_entry, gc_template):
    """Convert an Xbox ending-art entry into GC form, reusing `gc_template` (any GC ending-art entry)
    for every header byte. Index images are resampled nearest-neighbour, so the palette stays exact."""
    xp, gp = _parts(xbox_entry, False), _parts(gc_template, True)
    assert len(xp) == len(gp) == 2
    out = bytearray(gc_template)
    t0 = gp[0][0]
    _template_layout(out[t0:gp[0][1]])
    vxt = xbox_entry[xp[0][0]:xp[0][1]]
    for k in range(2):
        idx, pal = _xbox_page(vxt, k)
        img = [idx[(y * XB_H // GC_H) * XB_W + x * XB_W // GC_W] for y in range(GC_H) for x in range(GC_W)]
        at = t0 + _PIXELS[k]; out[at:at + GC_W * GC_H] = _tiles8(img, GC_W, GC_H)
        gr, ab = _gr_ab(pal)
        p = t0 + 0xe0 + 0x400 * k; out[p:p + 512] = gr; out[p + 0x200:p + 0x400] = ab
    text = xbox_entry[xp[1][0]:xp[1][1]]
    assert len(text) == gp[1][1] - gp[1][0]
    out[gp[1][0]:gp[1][1]] = text
    return bytes(out)


def decode_ending(gc_entry, page):
    """GC ending-art page -> rows of (r, g, b) (for tests and checks)."""
    t0 = _parts(gc_entry, True)[0][0]
    at = t0 + _PIXELS[page]
    idx = _untile8(gc_entry[at:at + GC_W * GC_H], GC_W, GC_H)
    p = t0 + 0xe0 + 0x400 * page
    return _rows(idx, GC_W, _gc_rgb(gc_entry[p:p + 512], gc_entry[p + 0x200:p + 0x400], 256))


# Select-screen face portraits (MAP.md, Task 8): group 2 entry 5 is a pack (big-endian on GC); part 2 is the
# face bank, one 64x64 texture per index, and a select cell's "A" bytes are indices into it. GC: VGT of C8
# textures, each with a 24-byte TLUT descriptor (u32 0, u32 offset, u16 count, u16 0) x2 pointing at the
# (G, R) and (A, B) palette halves, the same split as the ending art. Xbox: VXT of 64x64 P8.
FACE_ENTRY = (2, 5)
FACE_PART = 2
FACE_W = 64
XB_SPAWN_FACES = (51, 52)          # his record on Xbox: A = 51, 52 (one per costume)
_REC, _DESC = 0x44, 0x18


def _xbox_p8(vxt, k, w, h):
    """(indices, palette) of Xbox P8 texture k: w*h row-major indices, 256 (r, g, b, a)."""
    e = struct.unpack_from('<9I', vxt, 0x14 + 0x24 * k)
    palhdr, off, fmt = e[0], e[4], e[5]
    assert fmt == 0x0b and e[3] == (h << 16) | w, (hex(fmt), hex(e[3]))
    pal_off = struct.unpack_from('<I', vxt, palhdr)[0]
    pal = [((v >> 16) & 255, (v >> 8) & 255, v & 255, v >> 24) for v in struct.unpack_from('<256I', vxt, pal_off)]
    raw = vxt[off:off + w * h]
    assert len(raw) == w * h
    return [raw[texconv.unswizzle_index(x, y, w, h)] for y in range(h) for x in range(w)], pal


def xbox_face(vxt, k):
    """Xbox face k -> rows of (r, g, b)."""
    idx, pal = _xbox_p8(vxt, k, FACE_W, FACE_W)
    return _rows(idx, FACE_W, [p[:3] for p in pal])


def _face_rec(vgt, k):
    w = struct.unpack_from('>17I', vgt, struct.unpack_from('>I', vgt, 0x10)[0] + _REC * k)
    assert w[3] >> 20 == 9 and (w[3] & 0x3ff) + 1 == FACE_W and ((w[3] >> 10) & 0x3ff) + 1 == FACE_W, hex(w[3])
    d = struct.unpack_from('>6I', vgt, w[0])
    assert 0 < d[2] >> 16 <= 256 and d[5] >> 16 == d[2] >> 16, (hex(d[2]), hex(d[5]))
    return w, d


def decode_face(gc_entry, k):
    """GC face k (in entry 5's face bank) -> rows of (r, g, b) (for tests and checks)."""
    vgt = olk_pack(gc_entry)[FACE_PART]
    w, d = _face_rec(vgt, k)
    idx = _untile8(vgt[w[4]:w[4] + FACE_W * FACE_W], FACE_W, FACE_W)
    c = d[2] >> 16
    return _rows(idx, FACE_W, _gc_rgb(vgt[d[1]:d[1] + 2 * c], vgt[d[4]:d[4] + 2 * c], c))


def olk_pack(entry):
    return pack(entry, True)


def face_items(xbox_entry, gc_entry, faces=XB_SPAWN_FACES):
    """Xbox faces `faces` as items for append_textures on the GC face bank."""
    xvgt = pack(xbox_entry, False)[FACE_PART]
    gvgt = olk_pack(gc_entry)[FACE_PART]
    items = []
    for xk in faces:
        idx, pal = _xbox_p8(xvgt, xk, FACE_W, FACE_W)
        items.append((_face_rec(gvgt, 0)[0], _tiles8(idx, FACE_W, FACE_W)) + _gr_ab(pal) + (256,))
    return items


def face_bank(xbox_entry, gc_entry, faces=XB_SPAWN_FACES):
    """Append Xbox faces `faces` to the GC face bank. Returns (new GC entry 5, index of the first new face)."""
    out, n = append_textures(gc_entry, FACE_PART, face_items(xbox_entry, gc_entry, faces))
    _face_rec(olk_pack(out)[FACE_PART], 0)
    return out, n


def append_textures(gc_entry, part, items):
    """Append textures to the VGT bank in pack part `part` of a GC entry. Each item is (template record
    words, pixel bytes, GR palette half, AB palette half, palette entries); the template gives format and
    size. Returns (new entry, index of the first new texture).
    The loader reads records from 0x18 whatever the header's table offset says, and walks the TLUT
    descriptors as one array straight after the table (one per record, ending at header +0x14); it
    converts both in place, and converts only palettes in the block before the first pixel data. So the
    new records, descriptors and palettes each go after the old ones, and every offset behind each
    insertion moves (palettes and pixels by a multiple of 32). The new pixels go at the end."""
    gparts = olk_pack(gc_entry)
    old = bytes(gparts[part])
    assert old[:4] == b'VGT.', old[:4]
    n, table, tail = struct.unpack_from('>I', old, 8)[0], struct.unpack_from('>I', old, 0x10)[0], struct.unpack_from('>I', old, 0x14)[0]
    k_new = len(items)
    end = table + _REC * n
    assert table == 0x18 and tail == end + _DESC * n, (hex(table), hex(tail))
    s_rec = _REC * k_new
    s_all = s_rec + _DESC * k_new; s_all += -s_all % 32
    pix0 = min(struct.unpack_from('>I', old, table + _REC * k + 16)[0] for k in range(n))
    s_pal = sum(len(gr) + len(ab) for _, _, gr, ab, _ in items)   # the new palettes join the block before the pixels
    assert all(len(gr) % 32 == 0 and len(gr) == len(ab) == 2 * c for _, _, gr, ab, c in items)
    def mv(o):
        if o >= pix0: return o + s_all + s_pal
        if o >= tail: return o + s_all
        return o + s_rec if o >= end else o
    recs = []
    for k in range(n):
        w = list(struct.unpack_from('>17I', old, table + _REC * k))
        for i in (0, 4, 12):
            if w[i]: w[i] = mv(w[i])
        recs.append(w)
    descs = []
    for k in range(n):
        d = list(struct.unpack_from('>6I', old, end + _DESC * k))
        for i in (1, 4):
            if d[i]: d[i] = mv(d[i])
        descs.append(d)
    vgt = bytearray(old[:table]) + bytes(s_all + tail - table) + old[tail:pix0] + bytes(s_pal) + old[pix0:]
    at = s_all + pix0
    def put(b):
        vgt.extend(bytes(-len(vgt) % 32)); o = len(vgt); vgt.extend(b); return o
    for template, pixels, gr, ab, c in items:
        vgt[at:at + len(gr)] = gr; vgt[at + len(gr):at + 2 * len(gr)] = ab
        descs.append([0, at, c << 16, 0, at + len(gr), c << 16]); at += 2 * len(gr)
        w = list(template); w[4] = put(pixels); recs.append(w)
    new_end = table + _REC * len(recs)
    for k, w in enumerate(recs): w[0] = new_end + _DESC * k
    for k, d in enumerate(descs): struct.pack_into('>6I', vgt, new_end + _DESC * k, *d)
    for k, w in enumerate(recs): struct.pack_into('>17I', vgt, table + _REC * k, *w)
    struct.pack_into('>I', vgt, 8, len(recs)); struct.pack_into('>I', vgt, 0x14, new_end + _DESC * len(recs))
    vgt.extend(bytes(-len(vgt) % 32))
    # Re-pack, keeping the original header size and each later part's alignment (the growth is padded to
    # a multiple of 0x800, the largest alignment any part offset has).
    offs = struct.unpack_from('>%dI' % (len(gparts) + 1), gc_entry, 4)
    grow = len(vgt) - len(old); grow += -grow % 0x800
    vgt.extend(bytes(grow - (len(vgt) - len(old))))
    new_offs = [o + (grow if i > part else 0) for i, o in enumerate(offs)]
    out = bytearray(gc_entry[:offs[0]])
    struct.pack_into('>%dI' % len(new_offs), out, 4, *new_offs)
    for i, p in enumerate(gparts):
        out.extend(bytes(new_offs[i] - len(out)))
        out.extend(vgt if i == part else p)
    out.extend(bytes(new_offs[-1] - len(out)))
    out.extend(gc_entry[offs[-1]:])
    return bytes(out), n


# ---- Select-screen silhouettes: group 2 entry 10 part 0, a VGT bank of 128x256 C4 figures, one per
# costume, shown while a fighter's model loads. A cell's bytes +16..+23 index it (GC Link 100-103; the
# Xbox bank has 6 more early textures, so Mitsurugi is 0x32 on GC and 0x38 on Xbox). His cell was Link's,
# so he showed Link's figure. His Xbox figures are P8 using palette entries 0-15 only: exact as C4.
SILHOUETTE_ENTRY = (2, 10)
SILHOUETTE_PART = 0
SIL_W, SIL_H = 128, 256
XB_SPAWN_SILHOUETTES = (106, 107)  # his Xbox cell's +16 bytes (one per costume)
GC_SIL_TEMPLATE = 100              # Link's first figure: a 128x256 C4 record


def silhouette_template(gc_entry):
    """The record words of a GC figure, a 128x256 C4 texture."""
    gvgt = olk_pack(gc_entry)[SILHOUETTE_PART]
    tw = struct.unpack_from('>17I', gvgt, 0x18 + _REC * GC_SIL_TEMPLATE)
    assert tw[3] >> 20 == 8 and (tw[3] & 0x3ff) + 1 == SIL_W and ((tw[3] >> 10) & 0x3ff) + 1 == SIL_H, hex(tw[3])
    return tw


def tiles4(idx, w, h):
    """w x h C4 indices -> GX 8x8 tile order, two to a byte."""
    t = [idx[(ty * 8 + r) * w + tx * 8 + c] for ty in range(h // 8) for tx in range(w // 8) for r in range(8) for c in range(8)]
    return bytes(t[i] << 4 | t[i + 1] for i in range(0, len(t), 2))


def silhouette_items(xbox_entry, gc_entry, sils=XB_SPAWN_SILHOUETTES):
    """Xbox silhouettes `sils` as items for append_textures on the GC bank."""
    xvgt = pack(xbox_entry, False)[SILHOUETTE_PART]
    tw = silhouette_template(gc_entry)
    items = []
    for xk in sils:
        idx, pal = _xbox_p8(xvgt, xk, SIL_W, SIL_H)
        if max(idx) > 15:
            raise ValueError('Xbox silhouette %d uses more than 16 colours' % xk)
        items.append((tw, tiles4(idx, SIL_W, SIL_H)) + _gr_ab(pal[:16]) + (16,))
    return items


def silhouette_bank(xbox_entry, gc_entry, sils=XB_SPAWN_SILHOUETTES):
    """Append Xbox silhouettes `sils` to the GC bank. Returns (new GC entry 10, index of the first new one)."""
    return append_textures(gc_entry, SILHOUETTE_PART, silhouette_items(xbox_entry, gc_entry, sils))


def decode_silhouette(gc_entry, k):
    """GC silhouette k -> flat row-major palette indices, SIL_W x SIL_H (for tests and checks)."""
    vgt = olk_pack(gc_entry)[SILHOUETTE_PART]
    w = struct.unpack_from('>17I', vgt, 0x18 + _REC * k)
    px = [v for b in vgt[w[4]:w[4] + SIL_W * SIL_H // 2] for v in (b >> 4, b & 15)]
    img = [0] * (SIL_W * SIL_H); i = 0
    for ty in range(SIL_H // 8):
        for tx in range(SIL_W // 8):
            for r in range(8):
                for c in range(8):
                    img[(ty * 8 + r) * SIL_W + tx * 8 + c] = px[i]; i += 1
    return img


# ---- Character Profile camera: GC group 2 entry 503, Xbox 516 (517-520 are the same for Spawn).
# A 96-part pack, three 32-slot sections indexed by character ID (slot 29 + 32k is Spawn; the GC has
# Link in 27 + 32k and nothing in 29 + 32k, so the profile camera's goal stays zero and it aims at his
# feet). Section 0 (parts 1-31) is f32 data, plain u32 swap; sections 1 and 2 are a u32 count followed
# by u16 data. Both rules reproduce every fighter the discs share, except two whose content differs.
PROFILE_CAMERA = (2, 503)
XB_PROFILE_CAMERA = (2, 516)
PROFILE_SLOTS = 32


def _profile_part(x, section):
    out = bytearray(len(x))
    if section == 0:
        for o in range(0, len(x) // 4 * 4, 4): out[o:o + 4] = x[o:o + 4][::-1]
        return bytes(out)
    struct.pack_into('>I', out, 0, struct.unpack_from('<I', x, 0)[0])
    for o in range(4, len(x) // 2 * 2, 2): out[o:o + 2] = x[o:o + 2][::-1]
    return bytes(out)


def profile_camera(xbox_entry, gc_entry, char_id=29):
    """The GC entry with the Xbox's camera parts for char_id converted into its three empty slots."""
    import xb2gc
    g = pack(gc_entry, True); x = pack(xbox_entry, False)
    assert len(g) == len(x) == 3 * PROFILE_SLOTS, (len(g), len(x))
    out = list(g)
    for s in range(3):
        k = char_id + PROFILE_SLOTS * s
        if g[k]: raise ValueError('profile camera slot %d is not empty on this disc' % k)
        if not x[k]: raise ValueError('the Xbox has no profile camera slot %d' % k)
        out[k] = _profile_part(x[k], s)
    return xb2gc.build_pack(out)
