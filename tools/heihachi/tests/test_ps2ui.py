"""ps2ui: the cast both discs share is the oracle for the PS2's select-screen art, endings and profile camera."""
import math, os, struct, sys, tempfile, unittest
HERE = os.path.dirname(__file__)
for d in ('..', '../../guests', '../../spawn'): sys.path.insert(0, os.path.join(HERE, d))
import ps2iso, ps2tables, ps2ui, tables, ui
from olk import Olk, pack

IMAGE, GC, DOL = (os.environ.get(k, '') for k in ('RINGOUT_HEIHACHI_IMAGE', 'RINGOUT_SPAWN_GC', 'RINGOUT_SPAWN_DOL'))
HAVE = all(os.path.isfile(p) for p in (IMAGE, GC, DOL))


def psnr(a, b):
    se = sum((x - y) ** 2 for r, s in zip(a, b) for p, q in zip(r, s) for x, y in zip(p[:3], q[:3]))
    return 99 if not se else 10 * math.log10(255 ** 2 * 3 * len(a) * len(a[0]) / se)


@unittest.skipUnless(HAVE, 'needs RINGOUT_HEIHACHI_IMAGE, RINGOUT_SPAWN_GC and RINGOUT_SPAWN_DOL')
class Oracles(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root, elf = ps2iso.identify(ps2iso.open_image(IMAGE))
        cls.p, cls.g, cls.e, cls.d = Olk(cls.root), Olk(GC), ps2tables.Elf(elf), tables.Dol(DOL)
        cls.gcell = {}
        for i in range(tables.CELL_COUNT if hasattr(tables, 'CELL_COUNT') else 32):
            r = cls.d.read(tables.CELL_ADDR + i * tables.CELL_STRIDE, tables.CELL_STRIDE)
            cls.gcell.setdefault(struct.unpack_from('>H', r, 0)[0], r)

    def shared(self):
        for cid in tables.SHARED_IDS:
            if cid in self.gcell: yield cid, self.gcell[cid], ps2tables.ps2_cell(self.e, cid)

    def test_faces_match(self):
        pe, ge = self.p.entry(*ui.FACE_ENTRY), self.g.entry(*ui.FACE_ENTRY); n = 0
        for cid, g, x in self.shared():
            self.assertGreaterEqual(psnr(ps2ui.face_rows(pe, x[8]), ui.decode_face(ge, g[8])), 26, cid); n += 1
        self.assertGreater(n, 15)

    def test_silhouettes_exact(self):
        pe, ge = self.p.entry(*ui.SILHOUETTE_ENTRY), self.g.entry(*ui.SILHOUETTE_ENTRY); n = 0
        for cid, g, x in self.shared():
            self.assertEqual(ps2ui.silhouette_indices(pe, x[16]), ui.decode_silhouette(ge, g[16]), cid); n += 1
        self.assertGreater(n, 15)

    def test_his_art_builds_in_the_gc_format(self):
        ge = self.g.entry(*ui.FACE_ENTRY)
        new, first = ui.append_textures(ge, ui.FACE_PART, ps2ui.faces(self.p.entry(*ui.FACE_ENTRY), ge))
        self.assertEqual(first, 77)
        self.assertGreaterEqual(psnr(ui.decode_face(new, 77), ps2ui.face_rows(self.p.entry(*ui.FACE_ENTRY), ps2ui.FACES[0])), 40)
        gs_ = self.g.entry(*ui.SILHOUETTE_ENTRY)
        new, first = ui.append_textures(gs_, ui.SILHOUETTE_PART, ps2ui.silhouettes(self.p.entry(*ui.SILHOUETTE_ENTRY), gs_))
        self.assertEqual(ui.decode_silhouette(new, first), ps2ui.silhouette_indices(self.p.entry(*ui.SILHOUETTE_ENTRY), ps2ui.SILHOUETTES[0]))

    def test_ending_art_matches(self):
        for art in (79, 65):
            ours = ps2ui.ending_art(self.p.entry(2, art), self.g.entry(2, art))
            for page in (0, 1):
                self.assertGreaterEqual(psnr(ui.decode_ending(ours, page), ui.decode_ending(self.g.entry(2, art), page)), 26, (art, page))

    def test_his_ending_is_the_template_size(self):
        t = self.g.entry(2, 79)
        self.assertEqual(len(ps2ui.ending_art(self.p.entry(2, 85), t)), len(t))

    def test_camera_slots(self):
        x, g = pack(self.p.entry(*ui.PROFILE_CAMERA), False), pack(self.g.entry(*ui.PROFILE_CAMERA), True)
        misses = {(21, 1): 2, (21, 2): 2}   # measured: the two discs disagree in two bytes of these
        for cid in tables.SHARED_IDS:
            for s in range(3):
                k = cid + ui.PROFILE_SLOTS * s
                if k < len(x) and (x[k] or g[k]):
                    ours = ui._profile_part(x[k], s)
                    self.assertEqual(len(ours), len(g[k]), (cid, s))
                    self.assertEqual(sum(a != b for a, b in zip(ours, g[k])), misses.get((cid, s), 0), (cid, s))

    def test_camera_fills_his_three_slots(self):
        out = pack(ps2ui.camera(self.p.entry(*ui.PROFILE_CAMERA))(self.g.entry(*ui.PROFILE_CAMERA)), True)
        for k in (28, 60, 92): self.assertTrue(out[k], k)


if __name__ == '__main__':
    unittest.main()
