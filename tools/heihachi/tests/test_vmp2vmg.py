"""Reflection pass and material values, on Talim: the PS2's chunks against the GC's own materials."""
import os, struct, sys, unittest
HERE = os.path.dirname(__file__)
for d in ('..', '../../guests', '../../spawn'): sys.path.insert(0, os.path.join(HERE, d))
import build_heihachi, gs, ps2iso, vmp, vmp2vmg
from olk import Olk, pack

IMAGE, GC = os.environ.get('RINGOUT_HEIHACHI_IMAGE', ''), os.environ.get('RINGOUT_SPAWN_GC', '')
HAVE = os.path.isfile(IMAGE) and os.path.isfile(GC)
TALIM, HEIHACHI = 46 + 39 * 14, 46 + 39 * 23


def materials(x, end='<'):
    """(word0 low bits, word1, amb, spec, power, has second texture) per material of a VMX (<) or VMG (>)."""
    n = struct.unpack_from(end + 'H', x, 0x14)[0]; at = struct.unpack_from(end + 'I', x, 0x1c)[0]; out = []
    for i in range(n):
        w = struct.unpack_from(end + '8I', x, at + i * 0x50); f = struct.unpack_from(end + '12f', x, at + i * 0x50 + 0x20)
        out.append((w[0] & 3, w[1], f[:3], f[8:11], f[11], bool(w[3]), w[6]))
    return out


def close(a, b):
    return abs(a - b) <= max(abs(b) * 0.05, 1e-3)


@unittest.skipUnless(HAVE, 'needs RINGOUT_HEIHACHI_IMAGE and RINGOUT_SPAWN_GC')
class Talim(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        root, _ = ps2iso.identify(ps2iso.open_image(IMAGE))
        cls.P = pack(Olk(root).entry(4, TALIM), False)[0]
        cls.G = pack(Olk(GC).entry(4, TALIM), True)[0]
        cls.geo = vmp2vmg.geometry(cls.P)
        cls.vmx = vmp2vmg.synth_vmx(cls.geo)
        cls.mine, cls.gc = materials(cls.vmx), materials(cls.G, '>')

    def test_every_second_pass_chunk_gives_a_two_texture_material(self):
        m = vmp.Model(self.P); limit = struct.unpack_from('<I', self.P, 0x10)[0]
        n = sum(c['cnt'] == 9 for b in m.batches for _, a in b['lists'] for c in m.chunks(a, limit))
        self.assertEqual(n, 10)
        two = [k for k in self.mine if k[5]]
        self.assertEqual(len(two), n)
        for k in two:
            self.assertEqual(k[1] & 0x20, 0x20)
            self.assertEqual(struct.unpack_from('<H', self.vmx, k[6])[0], 5)   # the stage GC's reflection materials use

    def test_second_pass_shares_its_packets(self):
        m = vmp.Model(self.P); limit = struct.unpack_from('<I', self.P, 0x10)[0]
        for b in m.batches:
            for _, a in b['lists']:
                cs = m.chunks(a, limit)
                for i, c in enumerate(cs):
                    if c['cnt'] == 9: self.assertEqual(m.packets_of(c), m.packets_of(cs[i - 1]))

    def test_values_match_gc(self):
        misses = []
        for k in self.mine:
            hit = [g for g in self.gc if g[:2] == k[:2] and g[5] == k[5] and close(k[4], g[4]) and
                   all(close(a, b) for a, b in zip(k[2] + k[3], g[2] + g[3]))]
            if not hit: misses.append(k[:6])
        # One measured difference: an unlit chunk the GC draws with its reflection material; the PS2 has no second pass for it.
        self.assertEqual({round(k[2][0], 3) for k in misses}, {0.064})

    def test_no_placeholder_left(self):
        self.assertFalse([k for k in self.mine if k[0] == 2 and k[4] == 10.0 and k[3] == (0.2, 0.2, 0.2)])

    def test_power_mapping(self):
        self.assertEqual([vmp2vmg.power(v) for v in (5.087, 7.279, 9.469)], [10.0, 15.0, 20.0])

    def test_node_matrices_stay_identity(self):
        # Not derived: the PS2 keeps no bind pose in this model, and rigid parts sit right in the driven runs (T9).
        at = struct.unpack_from('<I', self.vmx, 0x24)[0]
        self.assertEqual(struct.unpack_from('<16f', self.vmx, at + 0x10)[0], 1.0)


@unittest.skipUnless(HAVE, 'needs RINGOUT_HEIHACHI_IMAGE and RINGOUT_SPAWN_GC')
class TextureGroups(unittest.TestCase):
    """Each draw list has its own upload group, all loaded at the same blocks: a list reads only its own group."""
    @classmethod
    def setUpClass(cls):
        root, _ = ps2iso.identify(ps2iso.open_image(IMAGE))
        o = Olk(root); cls.models = [pack(o.entry(4, e), False)[0] for e in (TALIM, HEIHACHI, HEIHACHI + 1)]

    def test_every_chunk_reads_an_upload_of_its_own_list(self):
        for P in self.models:
            m = vmp.Model(P); limit, bank = struct.unpack_from('<II', P, 0x10)
            groups = gs.upload_groups(P, bank)
            for b in m.batches:
                for kind, a in b['lists']:
                    blocks = {u[0] for u in groups[kind]}
                    for c in m.chunks(a, limit):
                        t = gs.tex0(*struct.unpack_from('<II', P, c['tex'] + 0x20))
                        self.assertIn(t['tbp'], blocks)
                        if t['psm'] in (gs.PSMT8, gs.PSMT4): self.assertIn(t['cbp'], blocks)

    def test_no_empty_picture(self):
        for P in self.models:
            for k, im in enumerate(vmp2vmg.geometry(P).images):
                self.assertTrue(any(p[3] for r in im for p in r), 'picture %d is fully transparent' % k)

    def test_blended_layer_of_his_gi_is_clean(self):
        # The garbled patch on his back: 128x128 in list 1, read from list 0's memory. The real one is the tiger of his
        # gi, on a transparent ground.
        g = vmp2vmg.geometry(self.models[2])
        im = next(g.images[m['tex']] for m in g.meshes if m['list'] and len(g.images[m['tex']]) == 128)
        self.assertEqual([im[y][x][3] for y, x in ((0, 0), (0, 127), (127, 0), (127, 127))], [0, 0, 0, 0])
        self.assertGreater(im[64][64][3], 200)


@unittest.skipUnless(HAVE, 'needs RINGOUT_HEIHACHI_IMAGE and RINGOUT_SPAWN_GC')
class RegionClamp(unittest.TestCase):
    """A 128x256 texture with 192 rows uploaded: its chunk's CLAMP_1 is REGION_CLAMP with MAXV 191, so the GS repeats
    row 191 below it. The rows past it are other textures' memory (the garbled lower back of his costume B)."""
    def test_clamp_register(self):
        self.assertEqual(gs.clamp(0x001fc009, 0x000002fc), dict(wms=1, wmt=2, minu=0, maxu=127, minv=0, maxv=191))

    def test_rows_past_maxv_repeat_the_edge(self):
        root, _ = ps2iso.identify(ps2iso.open_image(IMAGE))
        for entry in (HEIHACHI + 1, TALIM):
            g = vmp2vmg.geometry(pack(Olk(root).entry(4, entry), False)[0])
            tall = [im for im in g.images if len(im) == 256 and len(im[0]) == 128]
            self.assertTrue(tall)
            for im in tall:
                self.assertTrue(all(im[y] == im[191] for y in range(192, 256)))
                self.assertNotEqual(im[190], im[191])

    def test_uvs_span_the_region_not_the_texture(self):
        """Under REGION_CLAMP his UVs run 0..1 over the region, not the whole texture: the 128x128 picture with a
        96x96 region is black past it, and the 512x256 torso (MAXU 383) has his ear where u 0.70..0.79 of the region
        lands. Read as the whole texture, the UVs past the region hit the clamped edge, smeared over his back."""
        root, _ = ps2iso.identify(ps2iso.open_image(IMAGE))
        for entry in (HEIHACHI, HEIHACHI + 1):
            P = pack(Olk(root).entry(4, entry), False)[0]
            g, seen = vmp2vmg.geometry(P), 0
            for m in g.meshes:
                c = m['chunk']
                if P[c['tex'] + 0x38] != 8:
                    continue
                cl = gs.clamp(*struct.unpack_from('<II', P, c['tex'] + 0x30))
                t = gs.tex0(*struct.unpack_from('<II', P, c['tex'] + 0x20))
                uvs = [g.verts[i][1] for tri in m['tris'] for i in tri] if m['skin'] else [v['uv'] for v in m['vb']]
                if cl['wms'] == 2:
                    seen += 1
                    self.assertLessEqual(max(u for u, _ in uvs), (cl['maxu'] + 1) / t['tw'] + 1e-6)
                if cl['wmt'] == 2:
                    seen += 1
                    self.assertLessEqual(max(v for _, v in uvs), (cl['maxv'] + 1) / t['th'] + 1e-6)
            self.assertGreater(seen, 0)


@unittest.skipUnless(HAVE, 'needs RINGOUT_HEIHACHI_IMAGE and RINGOUT_SPAWN_GC')
class Grip(unittest.TestCase):
    """The GC runtime hangs a weapon half from its hand at a fixed grip G (rotation R, offset t), read from guest RAM in
    the select pose and two fight frames. His gauntlet halves are built in the bare hand's frame, so each is placed by
    G's inverse: hand-frame v becomes R^T (v - t), and the runtime's R v' + t puts it back on the forearm."""
    @classmethod
    def setUpClass(cls):
        root, _ = ps2iso.identify(ps2iso.open_image(IMAGE)); cls.P = pack(Olk(root).entry(4, HEIHACHI + 16), False)[0]
        cls.plain = vmp2vmg.geometry(cls.P); cls.placed = vmp2vmg.geometry(cls.P, place=build_heihachi.GRIP)

    def test_grip_matrices_are_rotations(self):
        for R, _ in build_heihachi.GRIP.values():
            for i in range(3):
                for j in range(3):
                    self.assertAlmostEqual(sum(R[k][i] * R[k][j] for k in range(3)), float(i == j), places=6)

    def test_the_grip_puts_every_vertex_back(self):
        for a, b in zip(self.plain.meshes, self.placed.meshes):
            if a['skin']: continue
            R, t = build_heihachi.GRIP[self.plain.nodes[a['node']]]
            for u, v in zip(a['vb'], b['vb']):
                back = [sum(R[i][k] * v['pos'][k] for k in range(3)) + t[i] for i in range(3)]
                for i in range(3): self.assertAlmostEqual(back[i], u['pos'][i], places=5)
                nb = [sum(R[i][k] * v['nrm'][k] for k in range(3)) for i in range(3)]
                for i in range(3): self.assertAlmostEqual(nb[i], u['nrm'][i], places=5)

    def test_triangles_and_joints_unchanged(self):
        self.assertEqual([m['tris'] for m in self.plain.meshes], [m['tris'] for m in self.placed.meshes])
        self.assertEqual(sorted({m['node'] for m in self.placed.meshes}), [1, 2]); self.assertEqual(self.placed.nodes, [0, 0, 1])

    def test_every_weapon_half_takes_its_own_hands_grip(self):
        # The grip follows the joint a half hangs from (0 right, 1 left), not the order the halves come in: weapon 25
        # stores the left half first.
        root, _ = ps2iso.identify(ps2iso.open_image(IMAGE)); o = Olk(root)
        for slot in range(16, 28):
            P = pack(o.entry(4, HEIHACHI + slot), False)[0]
            plain, placed = vmp2vmg.geometry(P), vmp2vmg.geometry(P, place=build_heihachi.GRIP)
            for a, b in zip(plain.meshes, placed.meshes):
                if a['skin']: continue
                R, t = build_heihachi.GRIP[plain.nodes[a['node']]]
                for u, v in zip(a['vb'], b['vb']):
                    back = [sum(R[i][k] * v['pos'][k] for k in range(3)) + t[i] for i in range(3)]
                    for i in range(3): self.assertAlmostEqual(back[i], u['pos'][i], places=5, msg='weapon %d' % slot)

    def test_placed_halves_lie_along_the_forearm(self):
        # Under the runtime's grip the x extent (the forearm direction in the hand frame) is restored, not left as the
        # grip's x, which points out of the fist like a blade.
        for m in self.placed.meshes:
            if m['skin']: continue
            R, t = build_heihachi.GRIP[self.placed.nodes[m['node']]]
            xs = [sum(R[0][k] * v['pos'][k] for k in range(3)) + t[0] for v in m['vb']]
            self.assertAlmostEqual(min(xs), -0.171, places=3); self.assertAlmostEqual(max(xs), 0.101, places=3)


if __name__ == '__main__':
    unittest.main()


@unittest.skipUnless(os.path.isfile(IMAGE), 'needs RINGOUT_HEIHACHI_IMAGE')
class Unarmed(unittest.TestCase):
    """His Tekken weapon (slot 26) is a model whose only picture has alpha 0: the PS2's alpha test draws nothing, so he
    fights bare-handed. Forced opaque it was a black gauntlet."""
    @classmethod
    def setUpClass(cls):
        root, _ = ps2iso.identify(ps2iso.open_image(IMAGE))
        cls.root = Olk(root)

    def geo(self, slot):
        return vmp2vmg.geometry(pack(self.root.entry(4, HEIHACHI + slot), False)[0])

    def test_a_fully_transparent_picture_stays_transparent_and_blended(self):
        g = self.geo(26)
        self.assertTrue(g.meshes)
        for m in g.meshes:
            self.assertEqual(m['list'], 1)
            self.assertEqual(max(p[3] for row in g.images[m['tex']] for p in row), 0)

    def test_a_visible_weapon_is_untouched(self):
        g = self.geo(25)
        self.assertEqual({m['list'] for m in g.meshes}, {0})
