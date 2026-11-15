"""Converter oracle on the cast both discs share: convert the Xbox entry and compare with the GC disc's
own bytes. Needs the player's discs, so it skips unless RINGOUT_SPAWN_XBOX and RINGOUT_SPAWN_GC name
the two root.olk files. Counts measured 2026-11-03 (the one costume-record miss is a GC revision)."""
import os, sys, unittest
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'guests'))
import struct
import olk, releases, sndconv, xb2gc, ui, xiso

HAVE = os.path.isfile(olk.X) and os.path.isfile(olk.G)
US_IMAGE = os.environ.get('RINGOUT_SPAWN_IMAGE_US', '')
SHARED_BLOCKS = 23          # character blocks 0..22 exist on both discs, at the same index
REVISED_SOUND_BLOCKS = {19, 20}  # Charade and Lizardman: the GC's sound banks are a later revision


@unittest.skipUnless(HAVE, 'needs RINGOUT_SPAWN_XBOX and RINGOUT_SPAWN_GC')
class OracleTest(unittest.TestCase):
    """The PAL Xbox disc (extracted root.olk)."""
    release = releases.PAL
    EXPECT = {'costume_records': (76, 77), 'dialogue': (68, 68), 'profile_camera': (53, 55, [53, 85]),
              'scene_records': (24, 24, 24), 'weapon_records': (222, 222), 'moveset_tables': (40, 40)}

    @classmethod
    def xbox(cls):
        return olk.X

    @classmethod
    def setUpClass(cls):
        cls.x, cls.g = olk.Olk(cls.xbox()), olk.Olk(olk.G)

    def pairs(self, slot, part):
        for blk in range(SHARED_BLOCKS):
            i = 46 + 39 * blk + slot
            a, b = self.x.entry(4, i), self.g.entry(4, i)
            pa = olk.pack(a, False) if a else None
            pb = olk.pack(b, True) if b else None
            if pa and pb and len(pa) > part and len(pb) > part and pa[part] and len(pa[part]) == len(pb[part]):
                yield pa[part], pb[part]

    def score(self, conv, slots, part):
        r = [conv(a) == b for s in slots for a, b in self.pairs(s, part)]
        return sum(r), len(r)

    def test_costume_records(self):
        self.assertEqual(self.score(xb2gc.records, (0, 1, 8, 9), 1), self.EXPECT['costume_records'])

    def test_dialogue(self):
        """Costume part 6 holds the fighter's pre-fight lines. The Xbox keeps six strings per line (column 0
        and 1 English, then German, French, Spanish, Italian) under the header word 0x10; the US GC keeps
        column 0 alone under 1. Unconverted, his stage-7 scene showed the German copy of another line
        (user report 2026-11-04)."""
        ok = n = 0
        for blk in range(SHARED_BLOCKS):
            for slot in (0, 1, 8, 9):
                a, b = self.x.entry(4, 46 + 39 * blk + slot), self.g.entry(4, 46 + 39 * blk + slot)
                if not a or not b:
                    continue
                xa, gb = olk.pack(a, False)[6], olk.pack(b, True)[6]
                if not xa or not gb:
                    continue
                c = xb2gc.dialogue(xa, self.release.dialogue_columns)
                n += 1
                ok += gb[:len(c)] == c and not any(gb[len(c):])
        self.assertEqual((ok, n), self.EXPECT['dialogue'])

    def test_profile_camera(self):
        """Group 2 entry 503 (Xbox 516) is the Character Profile camera, three 32-slot sections by character
        ID. Section 0 converts by u32 swap, sections 1-2 by a u32 count then u16s. Every shared fighter
        matches except two script parts (53, 85) whose content differs between the discs. Without his
        slots the camera aimed at his feet (user report 2026-11-04)."""
        g = olk.pack(self.g.entry(*ui.PROFILE_CAMERA), True); x = olk.pack(self.x.entry(2, self.release.profile_camera), False)
        ok = n = 0; bad = []
        for k in range(1, 96):
            if g[k] and x[k]:
                n += 1
                if ui._profile_part(x[k], k // ui.PROFILE_SLOTS) == g[k]: ok += 1
                else: bad.append(k)
        self.assertEqual((ok, n, bad), self.EXPECT['profile_camera'])

    def test_scene_records(self):
        """Group-4 records 8-45 are each fighter's home-stage scene: part 0 a two-motion bank, part 1 the
        motion names and a string table, part 2 a u32 table with one u16 run at 0x48, parts 7/8 sound.
        Parts 1 and 2 convert byte-exact on all 24 records both discs share; part 0 matches everywhere
        but the filler between motion blocks (and the last 0xFD of one header run), where the GC has
        stale bytes and the Xbox zeros."""
        exact = blocks = 0
        n = 0
        for e in range(8, 46):
            a, b = self.x.entry(4, e), self.g.entry(4, e)
            if e in (16, 17, 32, 33) or not a or not b:
                continue
            xa, gb = olk.pack(a, False), olk.pack(b, True)
            n += 1
            exact += xb2gc.scene_names(xa[1]) == gb[1] and xb2gc.scene_table(xa[2]) == gb[2]
            c, g = xb2gc.bank(xa[0]), gb[0]
            cnt = struct.unpack_from('<I', xa[0], 0)[0]
            offs = list(struct.unpack_from('<%dI' % cnt, xa[0], 8)) + [len(xa[0])]
            ok = len(c) == len(g)
            for k in range(cnt):
                m = xa[0][offs[k]:offs[k + 1]]
                frames, cb, flags = struct.unpack_from('<HHI', m, 0)
                nb = xb2gc.blocks_of(frames, flags)
                at = offs[k]
                ok &= c[at:at + 0x1b] == g[at:at + 0x1b] and c[at + 0x1c:at + 0x1c + 2 * nb] == g[at + 0x1c:at + 0x1c + 2 * nb]
                p = 0x1c
                for step in struct.unpack_from('<%dh' % nb, m, 0x1c):
                    p += step
                    ok &= c[at + p:at + p + cb] == g[at + p:at + p + cb]
            blocks += ok
        self.assertEqual((exact, blocks, n), self.EXPECT['scene_records'])

    def test_weapon_records(self):
        self.assertEqual(self.score(xb2gc.records, range(16, 28), 2), self.EXPECT['weapon_records'])

    def test_sound_record_settings(self):
        """Every playback setting (record bytes 4..0x4F) of the converted sound records matches the GC's,
        in the costume banks (parts 2 and 12) and the voice slots 30-36. Byte 0x3c set where the GC has 0
        played his VS-screen name call at the DSP's maximum rate (user report 2026-11-04)."""
        def recs(c):
            ho, hs = struct.unpack_from('>II', c, 0x18)
            h = c[ho:ho + hs]
            return [h[0x10 + 0xb0 * j:0x10 + 0xb0 * j + 0x50] for j in range(struct.unpack_from('>H', h, 8)[0])]
        def xrecs(c):  # the converter's record for bank 0 of an mmx, without encoding any audio
            ho, hs = struct.unpack_from('<II', c, 0x18)
            xh = c[ho:ho + hs]
            _, _, _, cnt, _, _, oa, _, ob, _ = struct.unpack_from('<BBHHHIIIII', xh, 4)
            return [sndconv.gc_record(xh[oa + 0x58 * i:oa + 0x58 * (i + 1)], xh[ob + 0x114 * i:ob + 0x114 * (i + 1)],
                                      0, 0, 0, 1, 0, [(0, 0)] * 8, 0)[:0x50] for i in range(cnt)]
        bad = set()
        for blk in range(SHARED_BLOCKS):
            e = 46 + 39 * blk
            cx, cg = self.x.entry(4, e), self.g.entry(4, e)
            pairs = [(olk.pack(cx, False)[p], olk.pack(cg, True)[p]) for p in (2, 12)] if cx and cg else []
            pairs += [(self.x.entry(4, e + s), self.g.entry(4, e + s)) for s in range(30, 37)]
            for a, b in pairs:
                if a[:4] != b'mmx\0' or b[:4] != b'mmg\0':
                    continue
                for ra, rb in zip(xrecs(a), recs(b)):
                    bad |= {(blk, k) for k in range(4, 0x50) if ra[k] != rb[k] and blk not in REVISED_SOUND_BLOCKS}
        self.assertEqual(sorted(bad), [])

    def test_moveset_tables(self):
        self.assertEqual(self.score(xb2gc.moveset_tables, (28, 29), 2), self.EXPECT['moveset_tables'])


@unittest.skipUnless(os.path.isfile(olk.G) and os.path.isfile(US_IMAGE), 'needs RINGOUT_SPAWN_GC and RINGOUT_SPAWN_IMAGE_US')
class USOracleTest(OracleTest):
    """The US Xbox disc, read straight from its image."""
    release = releases.US
    EXPECT = dict(OracleTest.EXPECT)

    @classmethod
    def xbox(cls):
        return xiso.find(xiso.open_image(US_IMAGE), 'root.olk')


if __name__ == '__main__':
    unittest.main()
