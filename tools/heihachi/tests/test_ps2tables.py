"""ps2iso and ps2tables on the player's PS2 disc (RINGOUT_HEIHACHI_IMAGE, RINGOUT_SPAWN_DOL). The refusals run anywhere."""
import os, struct, sys, tempfile, unittest
HERE = os.path.dirname(__file__)
for d in ('..', '../../guests', '../../spawn'): sys.path.insert(0, os.path.join(HERE, d))
import ps2iso, ps2tables, tables

IMAGE, DOL = os.environ.get('RINGOUT_HEIHACHI_IMAGE', ''), os.environ.get('RINGOUT_SPAWN_DOL', '')
HAVE = os.path.isfile(IMAGE) and os.path.isfile(DOL)


class Refusals(unittest.TestCase):
    def test_truncated_file_refused(self):
        with tempfile.NamedTemporaryFile() as f:
            f.write(b'x' * 4096); f.flush()
            with self.assertRaisesRegex(ps2iso.Refusal, 'not a PS2 disc image'):
                ps2iso.identify(ps2iso.open_image(f.name))

    def test_missing_file_refused(self):
        with self.assertRaisesRegex(ps2iso.Refusal, 'cannot read'):
            ps2iso.open_image('/nonexistent/x.iso')

    def test_unknown_release_refused(self):
        with self.assertRaisesRegex(ps2iso.Refusal, 'not a supported'):
            ps2iso.check_release('0' * 40, '1' * 40, table=())


@unittest.skipUnless(HAVE, 'needs RINGOUT_HEIHACHI_IMAGE and RINGOUT_SPAWN_DOL')
class Rows(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root, cls.elf = ps2iso.identify(ps2iso.open_image(IMAGE))
        cls.t = ps2tables.heihachi_tables(cls.elf, DOL)
        cls.e = ps2tables.Elf(cls.elf); cls.g = tables.Dol(DOL)

    def test_rows_measured(self):
        self.assertEqual(self.t['costume'], bytes([1, 1, 1, 1, 0, 0]))
        self.assertEqual(self.t['roster'], 1)
        self.assertEqual(struct.unpack('>hhI', self.t['ending']), (85, 12, 81))
        self.assertEqual(self.t['ending_old'], b'\xff' * 8)
        self.assertEqual(self.t['voice'], 1)
        self.assertEqual(len(self.t['preview']), 56)
        self.assertNotIn('name', self.t)

    def test_descriptor_strings_resolve(self):
        d, blob = self.t['descriptor'], self.t['strings']
        def s(slot):
            p = struct.unpack_from('>I', d, slot)[0] - ps2tables.STRINGS_ADDR
            return blob[p:blob.index(b'\0', p)]
        self.assertEqual((s(0x00), s(0x18), s(0x30), s(0x48), s(0x60)),
                         (b'Heihachi Mishima', b'HEIHACHI', b'Iron Fist', b'Japan?', b'75'))
        for o in (0x82, 0x86, 0x88):
            self.assertEqual(struct.unpack_from('>H', d, o)[0], 28, hex(o))
        self.assertLessEqual(len(blob), ps2tables.STRINGS_SIZE)

    def test_cell(self):
        self.assertEqual(self.t['cell'][:2], b'\x00\x1c')
        self.assertEqual(self.t['xcell'][:4], struct.pack('<HH', 28, 2))
        self.assertEqual(self.t['xcell'][8:10], bytes(ps2tables.PS2_FACES))
        self.assertEqual(self.t['xcell'][16:18], bytes(ps2tables.PS2_SILHOUETTES))

    def test_pool_lists_place_him_where_gc_has_link(self):
        have = {tuple(l) for l in self.t['pool_lists']}
        gc = [l for l in tables.pool_lists(self.g.read(tables.POOL_ADDR, tables.POOL_SIZE)) if tables.LINK_ID in l]
        self.assertEqual(len(gc), 7)
        for l in gc:
            self.assertIn(tuple(28 if v == 27 else v for v in l), have)

    def test_gc_slots_are_free(self):
        for a, n in ((ps2tables.STRINGS_ADDR, ps2tables.STRINGS_SIZE), (ps2tables.PREVIEW_HOME, 56),
                     (tables.DESC_ADDR + 28 * tables.DESC_STRIDE, tables.DESC_STRIDE)):
            self.assertEqual(self.g.read(a, n), bytes(n), hex(a))
        for a, n in ((tables.STRINGS_ADDR, tables.STRINGS_SIZE), (tables.PREVIEW_HOME, 28), (tables.NAME_AT, 36)):
            self.assertFalse(a < ps2tables.STRINGS_ADDR + ps2tables.STRINGS_SIZE and ps2tables.STRINGS_ADDR < a + n)
            self.assertFalse(a < ps2tables.PREVIEW_HOME + 56 and ps2tables.PREVIEW_HOME < a + n)

    def test_weapon_rows_equal_gc(self):
        # He keeps the GC's weapon rows and save slots; nothing is converted. A PS2 row differing would be news.
        self.assertEqual(ps2tables.weapon_row_differences(self.e, self.g), [])

    def test_home_scene_is_his(self):
        self.assertEqual(ps2tables.home_scene(self.root), {18: 18, 34: 34})

    def test_switch_case_is_as_id_24(self):
        self.assertEqual(ps2tables.switch_as_id(self.e), 24)


if __name__ == '__main__':
    unittest.main()
