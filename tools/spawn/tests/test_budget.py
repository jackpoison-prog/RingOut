"""Costume size budget. The game loads each player's costume into a fixed buffer: a converted costume
bigger than GC's own largest crashes the fight load (a null object, Invalid read at 0xc). Measured
2026-11-03: his costume 1 padded to costume 2's size crashes exactly like costume 2 does.

The synthetic test runs anywhere; the costume tests need the discs (RINGOUT_SPAWN_XBOX, _GC)."""
import os, struct, sys, unittest
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'guests'))
import olk, releases, texconv

HAVE = os.path.isfile(olk.X) and os.path.isfile(olk.G)


def vxt_dxt1(w, h, mips):
    """A VXT holding one DXT1 texture of `mips` levels; each level's blocks carry its level number."""
    data = bytearray(); lw, lh = w, h
    for l in range(mips):
        nb = max(1, lw // 4) * max(1, lh // 4)
        data += struct.pack('<HHI', 0xf800 >> l, 0x001f, 0x55555555) * nb
        lw, lh = max(1, lw // 2), max(1, lh // 2)
    hdr = bytearray(0x14 + 0x24)
    hdr[:4] = b'VXT.'; struct.pack_into('<I', hdr, 8, 1)
    struct.pack_into('<IIII', hdr, 0x14 + 0x0c, (h << 16) | w, len(hdr), texconv.D3D_DXT1, 0)
    struct.pack_into('<I', hdr, 0x14 + 0x1c, mips)
    return bytes(hdr + data)


def vgt_tex(vgt, k):
    w = struct.unpack_from('>17I', vgt, 0x18 + 0x44 * k)
    return (w[3] & 0x3ff) + 1, ((w[3] >> 10) & 0x3ff) + 1, (w[8] & 0xff) - 1, w[4]


class DropTopMip(unittest.TestCase):
    def test_dropping_the_top_level_promotes_the_next(self):
        v = vxt_dxt1(64, 32, 3)
        full, _ = texconv.vxt_to_vgt(v)
        cut, _ = texconv.vxt_to_vgt(v, drop_top={0})
        self.assertEqual(vgt_tex(full, 0)[:3], (64, 32, 3))
        self.assertEqual(vgt_tex(cut, 0)[:3], (32, 16, 2))
        # the promoted base is exactly the old second level: same CMPR bytes
        fo, co = vgt_tex(full, 0)[3], vgt_tex(cut, 0)[3]
        l0, l1 = 64 * 32 // 2, 32 * 16 // 2
        self.assertEqual(cut[co:co + l1], full[fo + l0:fo + l0 + l1])
        self.assertLess(len(cut), len(full))

    def test_single_level_texture_cannot_drop(self):
        with self.assertRaises(ValueError):
            texconv.vxt_to_vgt(vxt_dxt1(64, 32, 1), drop_top={0})


@unittest.skipUnless(HAVE, 'needs RINGOUT_SPAWN_XBOX and RINGOUT_SPAWN_GC')
class CostumeBudget(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        import build_spawn
        cls.b = build_spawn
        cls.x = olk.Olk(olk.X)

    def test_both_costumes_fit_gcs_largest(self):
        for slot in (0, 1):
            e = self.b.costume(self.x.entry(4, releases.PAL.spawn_block + slot))
            model = olk.pack(e, True)[0]
            self.assertLessEqual(len(model), self.b.GC_MODEL_MAX, slot)
            self.assertLessEqual(len(e), self.b.GC_COSTUME_MAX, slot)

    def test_costume_already_in_budget_is_untouched(self):
        import vmx2vmg
        xp = self.x.entry(4, releases.PAL.spawn_block)
        self.assertEqual(olk.pack(self.b.costume(xp), True)[0], vmx2vmg.convert(olk.pack(xp, False)[0]))


if __name__ == '__main__':
    unittest.main()
