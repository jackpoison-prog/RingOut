"""vmp, gs, vmp2vmg, ps2snd. Talim is on all three discs, so the PS2 conversion of him is checked against the
GC's and the Xbox's own data. Disc-gated tests need RINGOUT_HEIHACHI_IMAGE, RINGOUT_SPAWN_GC, RINGOUT_SPAWN_XBOX."""
import os, struct, sys, unittest
HERE = os.path.dirname(__file__)
for d in ('..', '../../guests', '../../spawn'): sys.path.insert(0, os.path.join(HERE, d))
import gs, ps2iso, ps2snd, vmp2vmg, vmx2vmg, build_heihachi, build_spawn, xb2gc
from olk import Olk, pack

IMAGE, GC, XB = (os.environ.get(k, '') for k in ('RINGOUT_HEIHACHI_IMAGE', 'RINGOUT_SPAWN_GC', 'RINGOUT_SPAWN_XBOX'))
HAVE = all(os.path.isfile(p) for p in (IMAGE, GC))
TALIM = 46 + 39 * 14
HEIHACHI = 46 + 39 * 23


class Synthetic(unittest.TestCase):
    def test_spu_block_decodes(self):
        # filter 0, shift 12: each nibble is the sample's top bits, shifted down by 12
        block = bytes([12, 0]) + bytes([0x21] + [0] * 13)
        self.assertEqual(ps2snd.spu_decode(block)[:3], [1, 2, 0])

    def test_leading_silent_block_is_skipped(self):
        self.assertEqual(len(ps2snd.spu_decode(bytes(16) + bytes([12, 0]) + bytes(14))), 28)

    def test_box_mip_halves(self):
        img = [[(0, 0, 0, 255), (4, 8, 12, 255)], [(4, 8, 12, 255), (0, 0, 0, 255)]]
        self.assertEqual(vmp2vmg.half(img), [[(2, 4, 6, 255)]])

    def test_triangle_winding_follows_the_normal(self):
        up = dict(skin=False, node=0, tris=[(0, 1, 2)], vb=[dict(pos=p, nrm=(0, 0, 1)) for p in ((0, 0, 0), (0, 1, 0), (1, 0, 0))])
        vmp2vmg.fix_winding([up], [])
        self.assertEqual(up['tris'], [(1, 0, 2)])

    def test_winding_crosses_a_shared_edge_when_the_normal_says_nothing(self):
        vb = [dict(pos=p, nrm=(0, 0, 1)) for p in ((0, 0, 0), (1, 0, 0), (0, 1, 0))] + [dict(pos=(1, 1, 0), nrm=(0, 0, 0))]
        m = dict(skin=False, node=0, tris=[(0, 1, 2), (3, 2, 1), (1, 2, 3)][:2], vb=vb)
        m['vb'][1]['nrm'] = m['vb'][2]['nrm'] = (0, 0, 0)  # only triangle 0's own corner says anything
        m['vb'][0]['nrm'] = (0, 0, 1)
        vmp2vmg.fix_winding([m], [])
        self.assertEqual(m['tris'], [(0, 1, 2), (3, 2, 1)])


@unittest.skipUnless(HAVE, 'needs RINGOUT_HEIHACHI_IMAGE and RINGOUT_SPAWN_GC')
class Discs(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        root, _ = ps2iso.identify(ps2iso.open_image(IMAGE))
        cls.p, cls.g = Olk(root), Olk(GC)

    def test_motion_matches_gc(self):
        for s in (28, 29):
            ours, gc = pack(build_spawn.moveset(self.p.entry(4, TALIM + s)), True), pack(self.g.entry(4, TALIM + s), True)
            same = sum(sum(a == b for a, b in zip(x, y)) for x, y in zip(ours, gc))
            self.assertGreaterEqual(same / sum(len(y) for y in gc), 0.999, s)

    def test_sound_records_match_gc(self):
        c, g = ps2snd.convert_container(self.p.entry(4, TALIM + 30)), self.g.entry(4, TALIM + 30)
        ca, ga = struct.unpack_from('>4I', c, 0x18), struct.unpack_from('>4I', g, 0x18)
        ch, gh = c[ca[0]:ca[0] + ca[1]], g[ga[0]:ga[0] + ga[1]]
        self.assertEqual(ch[:12], gh[:12])
        n = struct.unpack_from('>H', gh, 8)[0]; bad = 0
        self.assertGreater(n, 0)
        for i in range(n):
            a, b = gh[0x10 + i * 0xb0:0x10 + (i + 1) * 0xb0], ch[0x10 + i * 0xb0:0x10 + (i + 1) * 0xb0]
            bad += a[4:0x50] != b[4:0x50] or any(a[k:k + 4] != b[k:k + 4] for k in (0x58, 0x5c, 0x60, 0x68))
        self.assertEqual(bad, 0)

    def test_scene_matches_gc(self):
        ours = xb2gc.scene(pack(self.p.entry(4, 8), False)[:3] + [b''] * 6, ps2snd.convert_container, b'mmp\0')
        gc = pack(self.g.entry(4, 8), True)
        for k in (0, 1, 2):
            same = sum(a == b for a, b in zip(ours[k], gc[k]))
            self.assertGreaterEqual(same / len(gc[k]), 0.999, k)

    @unittest.skipUnless(os.path.isfile(XB), 'needs RINGOUT_SPAWN_XBOX')
    def test_geometry_matches_xbox(self):
        vmx = pack(Olk(XB).entry(4, TALIM), False)[0]
        geo = vmp2vmg.geometry(pack(self.p.entry(4, TALIM), False)[0])
        n_skin = sum(struct.unpack_from('<4I', vmx, struct.unpack_from('<I', vmx, 0x38)[0]))
        self.assertLessEqual(abs(len(geo.verts) - n_skin), n_skin // 100)
        # A skin vertex is known by its influences, a rigid one by its position: PS2 and Xbox indices differ.
        key = lambda p: tuple(round(c, 3) for c in p)
        counts = struct.unpack_from('<4I', vmx, struct.unpack_from('<I', vmx, 0x38)[0])
        at = struct.unpack_from('<I', vmx, struct.unpack_from('<I', vmx, 0x38)[0] + 16)[0]; skin = []; more = 0
        for n, cnt in enumerate(counts, start=1):
            for _ in range(cnt):
                k = n + more if n == 4 else n; infl = []
                for _ in range(k):
                    r = struct.unpack_from('<7f', vmx, at); bone = struct.unpack_from('<I', vmx, at + 28)[0]; at += 32
                    infl.append(r + (bone & 0xff,))
                if n == 4: more += bone >> 8
                skin.append(infl)
        known = lambda v: tuple(sorted(key(r[:3]) + (r[7],) for r in v))
        xbox = set(); groups = struct.unpack_from('<3H', vmx, 0x0c)
        for gi, f in enumerate((0x2c, 0x30, 0x34)):
            rec = struct.unpack_from('<I', vmx, f)[0]
            for r in range(groups[gi]):
                kind, count, _, _, idx, buf = struct.unpack_from('<6I', vmx, rec + r * 0x28)
                seq = struct.unpack_from('<%dH' % count, vmx, idx)
                tris = [seq[i:i + 3] for i in range(0, len(seq) - 2, 3)] if kind & 0x10000 else vmx2vmg.strip_tris(seq)
                for t in tris:
                    if kind & 4:
                        # A quarter of the Xbox's skinned triangles run against their own normals, on every joint, so
                        # the Xbox says nothing about those: only triangles it draws along their normals are testable.
                        if vmp2vmg._tri_sign(dict(skin=True), (0, 1, 2), [(skin[v],) for v in t]) > 0:
                            xbox.add(tuple(known(skin[v]) for v in t))
                    else:
                        xbox.add(tuple(key(struct.unpack_from('<3f', vmx, buf + 40 * v)) for v in t))
        testable = matched = 0
        for m in geo.meshes:
            for t in m['tris']:
                if m['skin']:
                    a, b, c = (known(geo.verts[v][0]) for v in t)
                else:
                    a, b, c = (key(m['vb'][v]['pos']) for v in t)
                same = {(a, b, c), (b, c, a), (c, a, b)} & xbox
                other = {(b, a, c), (a, c, b), (c, b, a)} & xbox
                testable += bool(same or other); matched += bool(same)
        self.assertGreaterEqual(testable, 1000)
        self.assertGreaterEqual(matched / testable, 1421 / 1422, (matched, testable))

    def test_heihachi_fits_and_loads(self):
        for s in (0, 1, 8, 9):
            e = build_heihachi.costume(self.p.entry(4, HEIHACHI + s))
            self.assertLessEqual(len(e), build_spawn.GC_COSTUME_MAX)
            m = pack(e, True)[0]
            self.assertEqual(m[:4], b'VMG.')
            self.assertLessEqual(len(m), build_spawn.GC_MODEL_MAX)

    def test_rigid_only_weapon_has_no_skin_block(self):
        vmx = vmp2vmg.synth_vmx(vmp2vmg.geometry(pack(self.p.entry(4, HEIHACHI + 16), False)[0]))
        self.assertEqual(struct.unpack_from('<H', vmx, 0x16)[0], 0)

    def test_stage_list_is_terminated(self):
        geo = vmp2vmg.geometry(pack(self.p.entry(4, HEIHACHI + 16), False)[0])
        vmx = vmp2vmg.synth_vmx(geo)
        stage, skin = struct.unpack_from('<I', vmx, 0x20)[0], struct.unpack_from('<I', vmx, 0x28)[0]
        self.assertIn(b'\xff\xff\xff\xff', vmx[stage:skin])

    def test_opaque_textures_have_full_alpha(self):
        geo = vmp2vmg.geometry(pack(self.p.entry(4, HEIHACHI), False)[0])
        self.assertGreater(max(len(im) * len(im[0]) for im in geo.images), 64 * 64)
        self.assertGreater(len({px for row in geo.images[0] for px in row}), 16)
        for m in geo.meshes:
            if m['list'] == 0 and m['env'] is None:
                self.assertTrue(all(px[3] == 255 for row in geo.images[m['tex']] for px in row))


if __name__ == '__main__':
    unittest.main()
