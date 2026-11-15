"""Ending illustrations (group 2, entry 57 + character ID). Disc-gated: needs RINGOUT_SPAWN_XBOX and
RINGOUT_SPAWN_GC. Locations and the GC palette layout are recorded in tools/spawn/MAP.md."""
import os, sys, unittest
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
import olk, ui

HAVE = os.path.isfile(olk.X) and os.path.isfile(olk.G)
TALIM, LINK, SPAWN = 22, 27, 29


def psnr(a, b):
    """Peak signal-to-noise ratio of two images given as rows of (r, g, b); 99 when identical."""
    import math
    d = [(p - q) ** 2 for ra, rb in zip(a, b) for pa, pb in zip(ra, rb) for p, q in zip(pa, pb)]
    mse = sum(d) / len(d)
    return 99.0 if mse == 0 else 10 * math.log10(255 ** 2 / mse)


@unittest.skipUnless(HAVE, 'needs RINGOUT_SPAWN_XBOX and RINGOUT_SPAWN_GC')
class EndingArtTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.x, cls.g = olk.Olk(olk.X), olk.Olk(olk.G)

    def test_locate(self):
        loc = ui.locate(self.g)
        self.assertEqual(loc['ending_art'](TALIM), (2, 79))
        self.assertEqual(loc['ending_art'](LINK), (2, 84))
        self.assertEqual(loc['ending_art'](SPAWN), (2, 86))
        self.assertEqual(loc['ending_text'], (2, 87))
        self.assertEqual(self.g.entry(2, 86), b'')            # the slot Spawn's art goes into is empty on GC

    def test_ending_text_already_has_spawn(self):
        self.assertIn(b'Malebolgia', self.g.entry(*ui.locate(self.g)['ending_text']))

    def test_convert_talim_matches_gc(self):
        """Talim's Xbox art converted must look like GC's own (different quantisation and resample,
        so compare decoded RGB: PSNR >= 25 dB on both pages)."""
        gc = self.g.entry(2, 79)
        out = ui.ending_art(self.x.entry(2, 79), gc)
        self.assertEqual(len(out), len(gc))
        for page in (0, 1):
            self.assertGreaterEqual(psnr(ui.decode_ending(out, page), ui.decode_ending(gc, page)), 25.0, page)

    def test_convert_spawn_keeps_his_text_part(self):
        out = ui.ending_art(self.x.entry(2, 86), self.g.entry(2, 79))
        self.assertEqual(olk.pack(out, True)[1], olk.pack(self.x.entry(2, 86), False)[1])


@unittest.skipUnless(HAVE, 'needs RINGOUT_SPAWN_XBOX and RINGOUT_SPAWN_GC')
class SilhouetteBankTest(unittest.TestCase):
    """Select-screen silhouettes: group 2 entry 10, part 0, 128x256 C4 figures indexed by cell +16. His cell
    was Link's, so he showed Link's figure while his model loaded (user report 2026-11-04)."""
    @classmethod
    def setUpClass(cls):
        cls.x, cls.g = olk.Olk(olk.X), olk.Olk(olk.G)
        cls.gc = cls.g.entry(*ui.SILHOUETTE_ENTRY)
        cls.out, cls.first = ui.silhouette_bank(cls.x.entry(*ui.SILHOUETTE_ENTRY), cls.gc)

    def test_shared_figures_match_across_discs(self):
        """The Xbox bank has 6 more early textures than the GC's: Mitsurugi's GC figures 50-52 are Xbox 56-58,
        and their indices match exactly."""
        xb = olk.pack(self.x.entry(*ui.SILHOUETTE_ENTRY), False)[ui.SILHOUETTE_PART]
        for k in (50, 51, 52):
            idx, _ = ui._xbox_p8(xb, k + 6, ui.SIL_W, ui.SIL_H)
            self.assertEqual(idx, ui.decode_silhouette(self.gc, k), k)

    def test_his_figures_are_appended_exactly(self):
        self.assertEqual(self.first, 150)
        xb = olk.pack(self.x.entry(*ui.SILHOUETTE_ENTRY), False)[ui.SILHOUETTE_PART]
        for j, xk in enumerate(ui.XB_SPAWN_SILHOUETTES):
            idx, _ = ui._xbox_p8(xb, xk, ui.SIL_W, ui.SIL_H)
            self.assertEqual(ui.decode_silhouette(self.out, self.first + j), idx, xk)

    def test_old_textures_unchanged(self):
        """Every old texture keeps its format, pixels and palette (offsets move, contents do not)."""
        import struct
        def tex(vgt, k):
            w = struct.unpack_from('>17I', vgt, 0x18 + 0x44 * k); d = struct.unpack_from('>6I', vgt, w[0])
            f, W, H = w[3] >> 20, (w[3] & 0x3ff) + 1, ((w[3] >> 10) & 0x3ff) + 1
            n = W * H // (2 if f == 8 else 1); c = 2 * (d[2] >> 16)
            return w[3], vgt[w[4]:w[4] + n], vgt[d[1]:d[1] + c], vgt[d[4]:d[4] + c]
        old = olk.pack(self.gc, True)[ui.SILHOUETTE_PART]; new = olk.pack(self.out, True)[ui.SILHOUETTE_PART]
        for k in range(150):
            self.assertEqual(tex(new, k), tex(old, k), k)


@unittest.skipUnless(HAVE, 'needs RINGOUT_SPAWN_XBOX and RINGOUT_SPAWN_GC')
class FaceBankTest(unittest.TestCase):
    """Select-screen face portraits: group 2 entry 5, part 2, one 64x64 C8 per index (MAP.md)."""
    @classmethod
    def setUpClass(cls):
        cls.x, cls.g = olk.Olk(olk.X), olk.Olk(olk.G)
        cls.gc = cls.g.entry(*ui.FACE_ENTRY)
        cls.out, cls.first = ui.face_bank(cls.x.entry(*ui.FACE_ENTRY), cls.gc)

    def psnr(self, a, b):
        return psnr(a, b)

    def test_talims_xbox_face_converts_to_gcs(self):
        """The channel layout is right: Talim's Xbox face (39) converted equals GC's own face 39."""
        img = ui.xbox_face(olk.pack(self.x.entry(*ui.FACE_ENTRY), False)[ui.FACE_PART], 39)
        self.assertGreaterEqual(self.psnr(img, ui.decode_face(self.gc, 39)), 30.0)

    def test_his_faces_are_appended(self):
        self.assertEqual(self.first, 77)                     # GC has 77 faces, so his go at 77 and 78
        xb = olk.pack(self.x.entry(*ui.FACE_ENTRY), False)[ui.FACE_PART]
        for k, xk in enumerate(ui.XB_SPAWN_FACES):
            self.assertEqual(self.psnr(ui.decode_face(self.out, self.first + k), ui.xbox_face(xb, xk)), 99.0)

    def test_record_table_stays_contiguous_at_0x18(self):
        """The game reads face records from 0x18 whatever the header says (a relocated table left his
        faces, and the whole top row with them, blank), so his records follow the old ones there."""
        import struct
        vgt = olk.pack(self.out, True)[ui.FACE_PART]
        self.assertEqual(struct.unpack_from('>I', vgt, 0x10)[0], 0x18)
        self.assertEqual(struct.unpack_from('>I', vgt, 8)[0], 79)
        self.assertLessEqual(0x18 + 0x44 * 79, min(struct.unpack_from('>I', vgt, 0x18 + 0x44 * k)[0] for k in range(79)))

    def test_descriptors_stay_one_contiguous_array(self):
        """The loader walks TLUT descriptors as an array right after the table, one per record, and
        ends it at header +0x14; his left at the end were never converted (in RAM, 2026-11-03)."""
        import struct
        vgt = olk.pack(self.out, True)[ui.FACE_PART]
        end = 0x18 + 0x44 * 79
        for k in range(79):
            self.assertEqual(struct.unpack_from('>I', vgt, 0x18 + 0x44 * k)[0], end + 0x18 * k, k)
        self.assertEqual(struct.unpack_from('>I', vgt, 0x14)[0], end + 0x18 * 79)

    def test_palettes_stay_one_block_before_the_pixels(self):
        """The loader converts only palettes in the block before the first pixel data; his at the end of
        the file were never converted (in RAM, 2026-11-03)."""
        import struct
        vgt = olk.pack(self.out, True)[ui.FACE_PART]
        end = 0x18 + 0x44 * 79
        pix = min(struct.unpack_from('>I', vgt, 0x18 + 0x44 * k + 16)[0] for k in range(79))
        for k in range(79):
            d = struct.unpack_from('>6I', vgt, end + 0x18 * k)
            self.assertLess(d[1], pix, k); self.assertLess(d[4], pix, k)

    def test_existing_faces_and_other_parts_unchanged(self):
        for k in (0, 39, 51, 76):
            self.assertEqual(ui.decode_face(self.out, k), ui.decode_face(self.gc, k), k)
        a, b = olk.pack(self.out, True), olk.pack(self.gc, True)
        self.assertEqual(len(a), len(b))
        for i in range(len(a)):
            if i != ui.FACE_PART: self.assertEqual(a[i], b[i], i)


if __name__ == '__main__':
    unittest.main()
