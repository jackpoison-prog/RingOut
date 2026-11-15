"""Xbox VMX model -> GameCube VMG model (throwaway spike, written for RingOut).

Layout facts used (both discs, measured from the shared cast):
  header: magic, u8 version (VMX 4, VMG 3), 0x10, 13 pointers, counts u16 at 0x0a..0x17
  pointers 0x18 tex bank, 0x1c materials (0x50), 0x20 stage records, 0x24 nodes (VMX 0x190, VMG 0x150),
  0x28 work (8 bytes: 0, pointer), 0x2c/0x30/0x34 mesh groups (VMX 0x28, VMG 0x38), 0x38 skin group
  (VMX 0x1c, VMG 0x24), 0x3c 0, 0x40 joints (0x40), 0x44 names, 0x48 stamp (0x28)
"""
import struct
import texconv
CLEAR_XBOX_FLAG = True

def _u32(d, at): return struct.unpack_from('<I', d, at)[0]

# Joint words kept as bytes / as two u16, by joint type (record byte 0x3f).
JOINT_RAW = {0x04: {14}, 0x07: {14}, 0x08: {14}, 0x09: {14}, 0x0b: {1, 2, 3, 13, 14}, 0x10: {13, 14}}
JOINT_U16 = {0x0b: {12}}

def joint(rec):
    out = bytearray(); kind = rec[0x3f]
    for w in range(16):
        word = rec[4*w:4*w+4]
        if w == 15 or w in JOINT_RAW.get(kind, ()): out += word
        elif w in JOINT_U16.get(kind, ()): out += word[1::-1] + word[3:1:-1]
        else: out += word[::-1]
    return bytes(out)

def sw32(d): return b''.join(d[i:i+4][::-1] for i in range(0, len(d) - len(d) % 4, 4)) + d[len(d) - len(d) % 4:]

def strip_tris(seq):
    out = []
    for i in range(len(seq) - 2):
        a, b, c = seq[i], seq[i+1], seq[i+2]
        if a == b or b == c or a == c: continue
        out.append((a, b, c) if i % 2 == 0 else (b, a, c))
    return out

def argb_to_rgba(v): return ((v << 8) & 0xffffffff) | (v >> 24)

class Out:
    def __init__(s): s.d = bytearray()
    def align(s, n): s.d += bytes(-len(s.d) % n); return len(s.d)
    def put(s, raw, a=4): at = s.align(a); s.d += raw; return at
    def p32(s, at, v): struct.pack_into('>I', s.d, at, v)

def s16(v, frac):
    q = int(round(v * (1 << frac)))
    return max(-32768, min(32767, q))

def uv_frac(uvs):
    m = max((abs(c) for uv in uvs for c in uv), default=0.0)
    f = 11
    while f > 0 and m * (1 << f) > 32767: f -= 1
    return f

def dl_list(tris, idx_sizes, lead_nop=False):
    """GX display list of triangle lists (0x90, VAT 0), indices big-endian, padded to 32 bytes."""
    out = bytearray()
    if lead_nop: out.append(0)
    fmt = ''.join({1: 'B', 2: 'H'}[s] for s in idx_sizes)
    MAXV = 0xfff0 // 3 * 3
    verts = [v for t in tris for v in t]
    for at in range(0, len(verts), MAXV):
        chunk = verts[at:at+MAXV]
        out += struct.pack('>BH', 0x90, len(chunk))
        for v in chunk: out += struct.pack('>' + fmt, *v)
    out += bytes(-len(out) % 32)
    return bytes(out)

def convert(x, drop_top=frozenset(), bank=None):
    """`drop_top`: texture-bank indices that lose their top mip level (texconv.vxt_to_vgt).
    `bank`: a finished GC texture bank to use instead of converting the model's own."""
    assert x[:4] == b'VMX.'
    n_nodes = struct.unpack_from('<H', x, 0x0a)[0]
    groups = struct.unpack_from('<3H', x, 0x0c)
    n_joints, n_mat, n_skin = struct.unpack_from('<3H', x, 0x12)
    P = lambda f: _u32(x, f)
    bank_x, mat_x, stage_x, node_x = P(0x18), P(0x1c), P(0x20), P(0x24)
    joint_x, names_x, stamp_x, skin_x = P(0x40), P(0x44), P(0x48), P(0x38)

    o = Out()
    o.put(b'VMG.' + bytes((3, 0x10, 13, 0)) + x[8:10] + struct.pack('>7H', n_nodes, *groups, n_joints, n_mat, n_skin)
          + bytes(0x4c - 0x18))
    recs = []
    for gi, f in enumerate((0x2c, 0x30, 0x34)):
        first = len(o.d)
        for r in range(groups[gi]): recs.append((gi, P(f) + r * 0x28, o.put(bytes(0x38))))
        o.p32(f, first)

    # stage records: u16 kind, u16 size, u32s; lists end with 0xffffffff
    stage_end = min(p for p in (P(0x28), skin_x, node_x, mat_x) if p > stage_x)
    st = bytearray(); at = stage_x
    while at + 4 <= stage_end:
        if x[at:at+4] == b'\xff' * 4: st += x[at:at+4]; at += 4; continue
        kind, size = struct.unpack_from('<2H', x, at)
        if size < 4 or at + size > stage_end: break
        st += struct.pack('>2H', kind, size) + sw32(x[at+4:at+size]); at += size
    st += x[at:stage_end]
    stage_g = o.put(bytes(st)); o.p32(0x20, stage_g)
    stage_of = lambda p: 0 if not p else stage_g + (p - stage_x)

    work_g = o.put(bytes(8)); o.p32(0x28, work_g)
    skin_g = o.put(bytes(0x24)) if n_skin else 0
    o.p32(0x38, skin_g or len(o.d))

    node_g = o.align(0x20)
    for i in range(n_nodes):
        a = node_x + i * 0x190
        m = struct.unpack_from('<16f', x, a + 0x10)
        t = [m[4*c + r] for r in range(4) for c in range(4)]
        o.d += x[a:a+0x10] + struct.pack('>16f', *t) + bytes(0x150 - 0x50)
    o.p32(0x24, node_g)

    mat_g = len(o.d); mat_fix = []
    for i in range(n_mat):
        a = mat_x + i * 0x50
        raw = bytearray(sw32(x[a:a+0x50])); words = struct.unpack_from('<8I', x, a)
        for k in range(3): struct.pack_into('>I', raw, 0x14 + 4*k, stage_of(words[5+k]))
        # the Xbox sets 0x08000000 in word 0 where the GameCube's own data has it clear (34 of 38 shared materials)
        if CLEAR_XBOX_FLAG: struct.pack_into('>I', raw, 0, words[0] & ~0x08000000)
        mat_fix.append((mat_g + i * 0x50, words[2:5])); o.d += raw
    o.p32(0x1c, mat_g)

    joint_g = o.align(0x20); jn = []
    for i in range(n_joints):
        a = joint_x + i * 0x40
        o.d += joint(x[a:a+0x40]); jn.append((joint_g + i * 0x40 + 0x2c, _u32(x, a + 0x2c)))
    o.p32(0x40, joint_g)

    # ---- geometry
    meshes = []
    for gi, rx, rg in recs:
        f = struct.unpack_from('<10I', x, rx)
        kind, count, node, mat, idx = f[:5]
        seq = struct.unpack_from('<%dH' % count, x, idx)
        if kind & 0x10000:
            tris = [tuple(seq[i:i+3]) for i in range(0, len(seq) - 2, 3)]
            tris = [t for t in tris if len(set(t)) == 3]
        else:
            tris = strip_tris(seq)
        meshes.append(dict(g=gi, rg=rg, kind=kind, node=node, mat=mat, f=f, tris=tris))

    skin_cnt = (0, 0, 0, 0); nskv = 0
    if n_skin:
        sk = struct.unpack_from('<7I', x, skin_x)
        skin_cnt = sk[:4]; infl_x, stream_x, stream2_x = sk[4:7]
        nskv = sum(skin_cnt)

    # shared skinned colour / uv arrays: GameCube indexes them separately
    colours, cidx = [], {}
    def col_index(v):
        if v not in cidx: cidx[v] = len(colours); colours.append(v)
        return cidx[v]
    sk_uv = []
    sk_col_of = []
    if nskv:
        cbase = meshes and next((m['f'][5] for m in meshes if m['kind'] & 4), 0)
        for v in range(nskv):
            c = _u32(x, cbase + 12*v); u, w = struct.unpack_from('<2f', x, cbase + 12*v + 4)
            sk_col_of.append(col_index(argb_to_rgba(c))); sk_uv.append((u, w))
    fr_sk = uv_frac(sk_uv)

    col_g = o.put(b''.join(struct.pack('>I', c) for c in colours) or bytes(4), 0x20)
    o.p32(work_g + 4, col_g)
    uv_g = o.put(b''.join(struct.pack('>2h', s16(u, fr_sk), s16(w, fr_sk)) for u, w in sk_uv) or bytes(4), 0x20)

    if nskv:
        pos = bytearray(); nrm = bytearray()
        for v in range(nskv):
            px, py, pz, _, nx, ny, nz, _ = struct.unpack_from('<8f', x, stream_x + 32*v)
            pos += struct.pack('>4f', px, py, pz, 1.0); nrm += struct.pack('>4f', nx, ny, nz, 0.0)
        pad = bytes(4 * 16)
        pos_g = o.put(bytes(pos) + pad, 0x20); pos2_g = o.put(bytes(pos) + pad, 0x20)
        nrm_g = o.put(bytes(nrm) + pad, 0x20); nrm2_g = o.put(bytes(nrm) + pad, 0x20)
        # influences: 32-byte floats -> 16-byte fixed point; the 4+ class keeps bone | more << 8
        inf = bytearray(); at = infl_x; more = 0; worst = 0.0
        for n, cnt in enumerate(skin_cnt, start=1):
            for _ in range(cnt):
                k = n + more if n == 4 else n
                for e in range(k):
                    px, py, pz, w, nx, ny, nz = struct.unpack_from('<7f', x, at)
                    bone = _u32(x, at + 28); at += 32
                    worst = max(worst, abs(px), abs(py), abs(pz))
                    inf += struct.pack('>3hH3hH', s16(px, 13), s16(py, 13), s16(pz, 13), max(0, min(0xffff, int(round(w * 8192)))),
                                       s16(nx, 14), s16(ny, 14), s16(nz, 14), bone & 0xffff)
                if n == 4: more += bone >> 8
        inf += b'\xff' * 16 + bytes(3 * 4 * 16)
        if worst > 4.0: print('WARNING: influence position', worst, 'exceeds s16/8192 range')
        inf_g = o.put(bytes(inf), 0x20)
        struct.pack_into('>9I', o.d, skin_g, *skin_cnt, inf_g, pos_g, pos2_g, nrm_g, nrm2_g)

    for m in meshes:
        gi = m['g']; f = m['f']
        if m['kind'] & 4:
            # skinned: position = normal = X vertex, colour palette, uv = X vertex
            idx_types = (3, 3, 3, 3) if gi == 2 else (3, 3, 2 if len(colours) <= 256 else 3, 3)
            sizes = [1 if t == 2 else 2 for t in idx_types]
            tris = [tuple((v, v, sk_col_of[v], v) for v in t) for t in m['tris']]
            tris = [t if gi == 2 else (t[1], t[0], t[2]) for t in tris]
            dl = dl_list(tris, sizes, lead_nop=(gi == 2))
            desc = bytes((2 if gi == 2 else 0, 0x10, 4, 0, 0x10, 4, 0, 4, 3, fr_sk)) + bytes(idx_types) + struct.pack('>H', len(dl) // 32)
            ptrs = [pos_g, pos2_g, nrm_g, nrm2_g, col_g, uv_g]; bounds = 0
        else:
            buf = f[5]; nv = max((v for t in m['tris'] for v in t), default=-1) + 1
            vs = [struct.unpack_from('<6fI2f', x, buf + 40*v) for v in range(nv)]
            fr = uv_frac([(v[7], v[8]) for v in vs])
            pos = b''.join(struct.pack('>3f', *v[:3]) for v in vs)
            nrm = b''.join(struct.pack('>3b', *[max(-128, min(127, int(round(c * 64)))) for c in v[3:6]]) for v in vs)
            cl = b''.join(struct.pack('>I', argb_to_rgba(v[6])) for v in vs)
            uvb = b''.join(struct.pack('>2h', s16(v[7], fr), s16(v[8], fr)) for v in vs)
            bounds = o.put(struct.pack('>4f', *struct.unpack_from('<4f', x, f[9])), 0x20) if f[9] else 0
            pg = o.put(pos, 0x20); ng = o.put(nrm or bytes(3), 0x20); cg = o.put(cl, 0x20); ug = o.put(uvb, 0x20)
            idx_types = (3, 3, 3, 3)
            tris = [tuple((v, v, v, v) for v in t) for t in m['tris']]
            tris = [t if gi == 2 else (t[1], t[0], t[2]) for t in tris]
            dl = dl_list(tris, [2, 2, 2, 2], lead_nop=(gi == 2))
            desc = bytes((2 if gi == 2 else 0, 0x0c, 4, 0, 3, 1, 6, 4, 3, fr)) + bytes(idx_types) + struct.pack('>H', len(dl) // 32)
            ptrs = [pg, 0, ng, 0, cg, ug]
        dl_g = o.put(dl, 0x20)
        node_p = node_g + (m['node'] - node_x) // 0x190 * 0x150
        mat_p = mat_g + (m['mat'] - mat_x)
        o.d[m['rg']:m['rg']+0x38] = desc + struct.pack('>10I', node_p, mat_p, *ptrs, dl_g, bounds)

    # ---- textures, stamp, names
    bank_end = stamp_x if stamp_x > bank_x else len(x)
    if bank is None: bank, entry_of = texconv.vxt_to_vgt(x[bank_x:bank_end], drop_top)
    bank_g = o.put(bank, 0x20); o.p32(0x18, bank_g)
    for at, ents in mat_fix:
        for k, p in enumerate(ents):
            if p:
                i = (p - bank_x - 0x14) // 0x24
                o.p32(at + 8 + 4*k, bank_g + 0x18 + 0x44 * i)
    stamp = bytearray(x[stamp_x:stamp_x+0x28]); stamp[2:4] = stamp[3:1:-1]
    sg = o.put(bytes(stamp), 0x20); o.p32(0x48, sg)
    ng = o.put(x[names_x:], 4); o.p32(0x44, ng)
    for at, p in jn: o.p32(at, ng + (p - names_x) if p else 0)
    o.align(0x80)
    return bytes(o.d)


def texture_sizes(x):
    """[(width, height, mip levels)] of a VMX model's texture bank, in bank order."""
    v = x[_u32(x, 0x18):]
    out = []
    for k in range(struct.unpack_from('<I', v, 8)[0]):
        e = v[0x14 + 0x24*k: 0x14 + 0x24*(k+1)]
        wh = struct.unpack_from('<I', e, 0x0c)[0]
        out.append((wh & 0xffff, wh >> 16, struct.unpack_from('<I', e, 0x1c)[0] or 1))
    return out
