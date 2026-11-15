"""PS2 model -> GC VMG. The geometry is written out as a synthetic Xbox VMX, which tools/spawn/vmx2vmg converts.

Traps, each of which hung or broke the game: the stage list ends in 0xffffffff; a rigid-only model has n_skin 0; a
rigid vertex is 40 bytes. Strips carry no consistent winding, so it comes from the vertex normals and spreads across
shared edges. A texture's alpha is transparency in a blended list and a reflection mask where a second pass follows;
elsewhere it means nothing and is forced to 255, unless it is 0 throughout: the alpha test hides that chunk
(his Tekken weapon, bare hands), so it goes to the blended list and stays invisible. A chunk with cnt 9 draws its base chunk's packets again with a reflection
map: on GC that is one mesh with two textures."""
import collections, hashlib, math, os, struct, sys
_HERE = os.path.dirname(os.path.abspath(__file__))
for d in ('guests', 'spawn'): sys.path.insert(0, os.path.join(_HERE, '..', d))
import gs, texconv, vmp, vmx2vmg

MIP_LEVELS = 3
_cmpr_cache = {}


def half(img):
    """The next mip level: each 2x2 box averaged."""
    h, w = len(img), len(img[0]); nw, nh = max(1, w // 2), max(1, h // 2); sx, sy = w // nw, h // nh; n = sx * sy
    return [[tuple((sum(img[y * sy + j][x * sx + i][c] for j in range(sy) for i in range(sx)) + n // 2) // n for c in range(4))
             for x in range(nw)] for y in range(nh)]


def _encode(img, alpha):
    h = hashlib.sha1(bytes(c for row in img for px in row for c in px) + bytes([alpha, len(img) & 255, len(img[0]) & 255])).digest()
    if h not in _cmpr_cache:
        _cmpr_cache[h] = texconv.rgba_to_cmpr(img, len(img[0]), len(img), alpha=bool(alpha))
    return _cmpr_cache[h]


def vgt(images, mips):
    """A GC texture bank: CMPR, with a second alpha texture where the picture has any. mips[k] levels of image k."""
    n = len(images); table_end = 0x18 + 0x44 * n; base = (table_end + 0x1f) & ~0x1f; data = bytearray(); recs = []
    for k, top in enumerate(images):
        h, w = len(top), len(top[0]); lv = [top]
        for _ in range(mips[k] - 1): lv.append(half(lv[-1]))
        has_alpha = any(px[3] < 250 for row in top for px in row)
        data += bytes(-len(data) % 32); coff = base + len(data); data += b''.join(_encode(l, 0) for l in lv)
        aoff = 0
        if has_alpha:
            data += bytes(-len(data) % 32); aoff = base + len(data); data += b''.join(_encode(l, 1) for l in lv)
        mode0 = 0x190 if mips[k] == 1 else 0x1d0; mode1 = ((mips[k] - 1) * 16) << 8
        fmt = (texconv.CMPR << 20) | ((h - 1) << 10) | (w - 1); size = ((w * h) // 64 << 16) | (mips[k] + 1)
        if has_alpha: words = [0, mode0, mode1, fmt, coff, 0x0002000c, 0x0e, 0, size, mode0, mode1, fmt, aoff, 0, 0x0e, 0, size]
        else: words = [0, mode0, mode1, fmt, coff, 0x0e, 0x0e, 0, size, 0, 0, 0, 0, (w << 16) | h, 0, 0, 0]
        recs.append(struct.pack('>17I', *words))
    out = bytearray(b'VGT.' + bytes((4, 0, 0, 0)) + struct.pack('>IIII', n, 0, 0x18, table_end) + b''.join(recs))
    out += bytes(base - len(out)); out += data
    return bytes(out)


def tris_of(vs, nokick):
    t = []; st = 0
    for i, v in enumerate(vs):
        if nokick(v):
            if i == 0 or not nokick(vs[i - 1]): st = i
            continue
        if i >= 2:
            a, b, c = i - 2, i - 1, i
            t.append((a, b, c) if (i - st) % 2 == 0 else (b, a, c))
    return t


def _sub(a, b): return (a[0] - b[0], a[1] - b[1], a[2] - b[2])
def _cross(a, b): return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def _face_sign(p, n):
    d = sum(x * y for x, y in zip(_cross(_sub(p[1], p[0]), _sub(p[2], p[0])), n))
    return (d > 0) - (d < 0)


def _tri_sign(m, t, verts):
    if m['skin']:
        L = [verts[i][0] for i in t]; common = set(r[7] for r in L[0]) & set(r[7] for r in L[1]) & set(r[7] for r in L[2])
        if not common: return 0
        j = min(common); R = [next(r for r in l if r[7] == j) for l in L]
        return _face_sign([r[:3] for r in R], [sum(r[k] for r in R) for k in (4, 5, 6)])
    V = [m['vb'][i] for i in t]
    return _face_sign([v['pos'] for v in V], [sum(v['nrm'][k] for v in V) for k in range(3)])


def fix_winding(meshes, verts):
    """Flip triangles in place so each faces along its normals; a triangle the normals say nothing about takes the
    order of a neighbour across a shared edge."""
    pid = {}
    def point(k, m, i):
        key = verts[i][0] if m['skin'] else (k, m['vb'][i]['pos'])
        return pid.setdefault(key, len(pid))
    tri = [(k, j) for k, m in enumerate(meshes) for j in range(len(m['tris']))]
    edges = collections.defaultdict(list); pts = []; state = []
    for i, (k, j) in enumerate(tri):
        m = meshes[k]; a, b, c = (point(k, m, v) for v in m['tris'][j]); pts.append((a, b, c))
        for e in ((a, b), (b, c), (c, a)): edges[frozenset(e)].append((i, e))
        state.append(_tri_sign(m, m['tris'][j], verts))
    todo = collections.deque(i for i, s in enumerate(state) if s)
    while todo:
        i = todo.popleft(); a, b, c = pts[i]
        for e in ((a, b), (b, c), (c, a)):
            ee = e if state[i] > 0 else (e[1], e[0])
            for n, ne in edges[frozenset(e)]:
                if n != i and not state[n]:
                    state[n] = 1 if ne == (ee[1], ee[0]) else -1   # a neighbour runs the shared edge the other way
                    todo.append(n)
    for i, (k, j) in enumerate(tri):
        if state[i] < 0:
            t = meshes[k]['tris'][j]; meshes[k]['tris'][j] = (t[1], t[0], t[2])


Geometry = collections.namedtuple('Geometry', 'P meshes verts nodes images single_level')
SECOND_PASS = 9


def geometry(P, place=None):
    """Everything drawn in the first pass: meshes with triangles in order, the shared skin vertices, node joints, RGBA pictures.
    place: {joint: (R, t)}, a rigid frame per joint a node hangs from; each rigid vertex v of that node becomes R^T (v - t), its normal R^T n."""
    m = vmp.Model(P); limit = struct.unpack_from('<I', P, 0x10)[0]; bank = struct.unpack_from('<I', P, 0x14)[0]
    groups = gs.replay(P, bank)
    tex, images, skv, verts, meshes, nodes, node_of, single = {}, [], {}, [], [], [0], {}, set()
    def tex_index(c, kind, keep_alpha):
        """A chunk's picture: TEX0 at its texture block +0x20, CLAMP_1 at +0x30."""
        t = gs.tex0(*struct.unpack_from('<II', P, c['tex'] + 0x20))
        cl = gs.clamp(*struct.unpack_from('<II', P, c['tex'] + 0x30)) if P[c['tex'] + 0x38] == 8 else None
        key = (kind, t['tbp'], t['cbp'], t['psm'], t['tw'], t['th'], keep_alpha, tuple(cl.values()) if cl else None)
        if key not in tex:
            im = groups[kind].read(t)
            if cl: im = gs.region_clamped(im, cl)
            if not keep_alpha: im = [[(p[0], p[1], p[2], 255) for p in row] for row in im]
            tex[key] = len(images); images.append(im)
        return tex[key]
    def unseen(c):
        t = gs.tex0(*struct.unpack_from('<II', P, c['tex'] + 0x20))
        return all(p[3] == 0 for row in groups[0].read(t) for p in row)
    def region(c):
        """UV -> texture UV. Under REGION_CLAMP the UVs run 0..1 over the region, not the whole texture."""
        if P[c['tex'] + 0x38] != 8: return lambda uv: uv
        t = gs.tex0(*struct.unpack_from('<II', P, c['tex'] + 0x20)); cl = gs.clamp(*struct.unpack_from('<II', P, c['tex'] + 0x30))
        su = ((cl['minu'], cl['maxu'] + 1 - cl['minu']) if cl['wms'] == 2 else (0, t['tw']))
        sv = ((cl['minv'], cl['maxv'] + 1 - cl['minv']) if cl['wmt'] == 2 else (0, t['th']))
        return lambda uv: ((su[0] + uv[0] * su[1]) / t['tw'], (sv[0] + uv[1] * sv[1]) / t['th'])
    def sk(v, pal, uvf):
        infl = tuple((r[0], r[1], r[2], r[3], r[4], r[5], r[6], pal[r[7]]) for r in v['infl'])
        k = (infl, uvf(v['uv']), tuple(v['col'][:3]))
        if k not in skv: skv[k] = len(verts); verts.append(k)
        return skv[k]
    for b in m.batches:
        for kind, a in b['lists']:
            chunks = m.chunks(a, limit)
            for ci, c in enumerate(chunks):
                if c['cnt'] == SECOND_PASS: continue
                nxt = chunks[ci + 1] if ci + 1 < len(chunks) else None
                second = nxt if nxt and nxt['cnt'] == SECOND_PASS and m.packets_of(nxt) == m.packets_of(c) else None
                hidden = kind == 0 and second is None and unseen(c)   # alpha 0 throughout: the alpha test draws none of it
                ti = tex_index(c, kind, kind > 0 or second is not None or hidden); lst = 1 if hidden else min(kind, 1)
                env = None
                if second:
                    env = tex_index(second, kind, True); single.add(env)
                ups = [m.vif(o, q) for o, q in m.packets_of(c)]; tris = []; uvf = region(c)
                if b['type'] == 1:
                    for u in ups:
                        vs = m.skinned_verts(u); ids = [sk(v, b['palette'], uvf) for v in vs]
                        tris += [tuple(ids[i] for i in t) for t in tris_of(vs, lambda v: v['kick'] == 1)]
                    meshes.append(dict(skin=True, tris=tris, tex=ti, env=env, list=lst, chunk=c))
                else:
                    vb = []
                    for u in ups:
                        vs = m.rigid_verts(u); base = len(vb); vb += [dict(v, uv=uvf(v['uv'])) for v in vs]
                        tris += [tuple(base + i for i in t) for t in tris_of(vs, lambda v: v['kick'] & 0x8000)]
                    j = b['palette'][0]
                    if j not in node_of: node_of[j] = len(nodes); nodes.append(j)
                    meshes.append(dict(skin=False, tris=tris, vb=vb, tex=ti, env=env, list=lst, node=node_of[j], chunk=c))
    fix_winding(meshes, verts)
    for m in meshes:
        if m['skin'] or not place or nodes[m['node']] not in place: continue
        R, t = place[nodes[m['node']]]
        back = lambda v: tuple(sum(R[k][i] * v[k] for k in range(3)) for i in range(3))
        m['vb'] = [dict(v, pos=back(tuple(v['pos'][k] - t[k] for k in range(3))), nrm=back(v['nrm'])) for v in m['vb']]
    return Geometry(P, meshes, verts, nodes, images, single)


def _c8(v): return min(255, int(v * 255 / 128))


def power(v):
    """A chunk's specular curve term -> the GC's specular power. Measured on the shared cast: 5.087, 7.279, 9.469 are
    10, 15, 20."""
    return float(round(10 + (v - 5.087) * 5 / 2.191))


def _material(m, stage, env_stage):
    """80 bytes: a chunk's 8 floats of 0x30 are ambient and diffuse; 0x50 adds specular and its curve."""
    f = m['chunk']['floats']; lit = m['chunk']['sz'] == 0x50
    flags = (1 if m['list'] else 0) | (0x20 if m['env'] is not None else 0)
    spec, pw = (f[8:11], power(f[14])) if lit else ((0.0, 0.0, 0.0), 0.0)
    return struct.pack('<8I', 2 if lit else 1, flags, 0, 0, 0, stage, env_stage, 0) + struct.pack('<12f', *f[:3], 0, 1, 1, 1, 1, *spec, pw)


def synth_vmx(geo):
    """The geometry as an Xbox VMX. Its texture bank is empty apart from one fake entry per picture; `convert`
    gives vmx2vmg the real one."""
    P, verts, nodes, images = geo.P, geo.verts, geo.nodes, geo.images
    u = lambda a: struct.unpack_from('<I', P, a)[0]
    joint_at, stamp, nj = u(0x10), u(0x24), P[10]
    order = sorted(range(len(verts)), key=lambda i: min(len(verts[i][0]), 4) * 1000 + len(verts[i][0]))
    remap = {o: i for i, o in enumerate(order)}
    counts = [sum(1 for v in verts if min(len(v[0]), 4) == n) for n in (1, 2, 3, 4)]
    w = bytearray(0x4c)
    def put(b, a=4):
        w.extend(bytes(-len(w) % a)); at = len(w); w.extend(b); return at
    n0 = [m for m in geo.meshes if m['list'] == 0]; n1 = [m for m in geo.meshes if m['list'] == 1]; meshes = n0 + n1
    stage_at = len(w)
    stage_ptrs = [put(struct.pack('<2H2f6I', 1, 0x24, 1.0, 1.0, *[0] * 6)) for _ in meshes]
    env_ptrs = [put(struct.pack('<2H2f6I', 5, 0x24, 1.0, 1.0, *[0] * 6)) if m['env'] is not None else 0 for m in meshes]
    put(b'\xff\xff\xff\xff' + bytes(4))
    work = put(bytes(8)); skin_at = put(bytes(0x1c)); node_at = put(b'', 0x20)
    I = (1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)
    for i, j in enumerate(nodes):
        w.extend((bytes(16) if i == 0 else bytes((2, j, 0, 0)) + bytes(12)) + struct.pack('<16f', *I) + bytes(0x190 - 0x50))
    mat_at = len(w)
    for k, m in enumerate(meshes):
        w.extend(_material(m, stage_ptrs[k], env_ptrs[k]))
    idx = []
    for m in meshes:
        ii = [remap[v] if m['skin'] else v for t in m['tris'] for v in t]
        idx.append(put(struct.pack('<%dH' % len(ii), *ii) + b'\0\0', 2)); m['n'] = len(ii)
    infl_at = put(b'', 0x10)
    for pos, i in enumerate(order):   # the 4+ class: last influence's high byte = next vertex's count - this one's
        L = verts[i][0]; delta = len(verts[order[pos + 1]][0]) - len(L) if len(L) >= 4 and pos + 1 < len(order) else 0
        for e, r in enumerate(L):
            w.extend(struct.pack('<7fI', *r[:7], r[7] | ((delta << 8) if e == len(L) - 1 else 0)))
    stream_at = put(bytes(32 * len(order)), 0x10); stream2_at = len(w)
    cbase = put(b''.join(struct.pack('<I2f', 0xff000000 | (_c8(verts[i][2][0]) << 16) | (_c8(verts[i][2][1]) << 8) | _c8(verts[i][2][2]),
                                     *verts[i][1]) for i in order))
    recs = []
    for k, m in enumerate(meshes):
        if m['skin']:
            recs.append(struct.pack('<10I', 4 | 0x10000, m['n'], node_at, mat_at + k * 0x50, idx[k], cbase, 0, 0, 0, 0))
        else:
            vb = bytearray()
            for v in m['vb']:
                c = _c8(v['col']); vb += struct.pack('<6fI2f', *v['pos'], *v['nrm'], 0xff000000 | c << 16 | c << 8 | c, *v['uv']) + bytes(4)
            ps = [v['pos'] for v in m['vb']]
            ce = [(min(p[a] for p in ps) + max(p[a] for p in ps)) / 2 for a in range(3)]
            r = max(math.dist(p, ce) for p in ps)
            bo = put(struct.pack('<4f', *ce, r)); ba = put(bytes(vb))
            recs.append(struct.pack('<10I', 0x10000, m['n'], node_at + m['node'] * 0x190, mat_at + k * 0x50, idx[k], ba, 0, 0, 0, bo))
    g0 = put(b''.join(recs[:len(n0)])); g1 = put(b''.join(recs[len(n0):])); g2 = len(w)
    struct.pack_into('<7I', w, skin_at, *counts, infl_at, stream_at, stream2_at)
    joint_new = put(b'', 0x20); w.extend(P[joint_at:joint_at + nj * 0x40])
    bank_x = put(b'', 0x20); w.extend(b'VXT.' + bytes(0x10) + bytes(0x24 * len(images)))
    stamp_new = put(P[stamp:stamp + 0x28]); names_new = len(w); w.extend(P[stamp + 0x28:])
    delta = names_new - (stamp + 0x28)
    for i in range(nj):
        a = joint_new + i * 0x40 + 0x2c; v = struct.unpack_from('<I', w, a)[0]
        if v: struct.pack_into('<I', w, a, v + delta)
    for k, m in enumerate(meshes):
        struct.pack_into('<I', w, mat_at + k * 0x50 + 8, bank_x + 0x14 + 0x24 * m['tex'])
        if m['env'] is not None: struct.pack_into('<I', w, mat_at + k * 0x50 + 12, bank_x + 0x14 + 0x24 * m['env'])
    w[:4] = b'VMX.'; w[4:8] = bytes((4, 0x10, 13, 0)); w[8:10] = b'\x00\x01'
    struct.pack_into('<7H', w, 0xa, len(nodes), len(n0), len(n1), 0, nj, len(meshes), 1 if order else 0)
    struct.pack_into('<13I', w, 0x18, bank_x, mat_at, stage_at, node_at, work, g0, g1, g2, skin_at, 0, joint_new, names_new, stamp_new)
    return bytes(w)


def convert(entry, budget, place=None):
    """Part 0 of a PS2 costume or weapon -> a GC VMG of at most `budget` bytes. The largest pictures lose their mip
    levels first. ValueError if it still does not fit."""
    geo = geometry(entry, place); vmx = synth_vmx(geo)
    mips = [1 if k in geo.single_level else min(MIP_LEVELS, 1 + int(math.log2(min(len(im), len(im[0])))))
            for k, im in enumerate(geo.images)]
    while True:
        model = vmx2vmg.convert(vmx, bank=vgt(geo.images, mips))
        if len(model) <= budget: return model
        left = [(len(im) * len(im[0]), k) for k, im in enumerate(geo.images) if mips[k] > 1]
        if not left:
            raise ValueError('model is %d bytes, over the %d budget, with no mip level left to drop' % (len(model), budget))
        mips[max(left)[1]] -= 1
