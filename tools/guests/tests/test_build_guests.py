"""build_guests: one archive and manifest from a list of guests.

Needs the GC archive and RINGOUT_SPAWN_WORK (about 1.2 GB free for the stub builds)."""
import json, os, struct, sys, tempfile, unittest
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
import build_guests, guest, olk, tables, ui

DOL = os.environ.get('RINGOUT_SPAWN_DOL', '')
HAVE = os.path.isfile(olk.G) and os.path.isfile(olk.X) and os.path.isfile(DOL) and os.path.isdir(os.environ.get('RINGOUT_SPAWN_WORK', ''))


def stub(cid, cell, scenes, marker, pools=()):
    """A guest with marked entries and Xbox art as its faces (real items, so the bank's checks run)."""
    x = olk.Olk(olk.X); g = olk.Olk(olk.G)
    gv = ui.olk_pack(g.entry(*ui.FACE_ENTRY))[ui.FACE_PART]
    faces = ui.face_items(x.entry(*ui.FACE_ENTRY), g.entry(*ui.FACE_ENTRY), (51, 52))
    sils = ui.silhouette_items(x.entry(*ui.SILHOUETTE_ENTRY), g.entry(*ui.SILHOUETTE_ENTRY), (106, 107))
    template = g.entry(2, 79)
    t = {'descriptor': bytes(0xF8), 'strings': b'x\0', 'costume': bytes([1, 1, 0, 0, 0, 0]), 'roster': 1, 'voice': 1,
         'cell': struct.pack('>H', cid) + bytes(34), 'xcell': bytes(36), 'ending': bytes(8), 'ending_old': b'\xff' * 8,
         'preview': bytes(28)}
    if cid == 29: t['name'] = bytes(36)
    return guest.GuestData(id=cid, cell=cell, name='stub%d' % cid, block={0: marker + b'b0', 38: marker + b'b38'},
                           scenes={r: marker + b's%d' % r for r in scenes}, ending_art=template,
                           faces=faces, silhouettes=sils, camera=lambda e: e, tables=t, pool_lists=list(pools),
                           sources={'stub': cid})


@unittest.skipUnless(HAVE, 'needs RINGOUT_SPAWN_GC, _XBOX, _DOL and _WORK')
class TwoGuests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory(dir=os.environ['RINGOUT_SPAWN_WORK'])
        cls.both = os.path.join(cls.work.name, 'both'); os.mkdir(cls.both)
        cls.one = os.path.join(cls.work.name, 'one'); os.mkdir(cls.one)
        h, s = stub(28, 9, (17, 33), b'H'), stub(29, 4, (7, 23), b'S')
        cls.m = build_guests.build([h, s], olk.G, cls.both, DOL)     # given out of order on purpose
        cls.m1 = build_guests.build([stub(29, 4, (7, 23), b'S')], olk.G, cls.one, DOL)
        cls.a = olk.Olk(os.path.join(cls.both, 'root-guests.olk'))

    @classmethod
    def tearDownClass(cls):
        cls.a.f.close(); cls.work.cleanup()

    def test_two_stub_guests_blocks_in_order(self):
        self.assertEqual([g['id'] for g in self.m['guests']], [29, 28])
        self.assertEqual([g['block'] for g in self.m['guests']], [982, 1021])
        self.assertEqual(self.a.entry(4, 982), b'Sb0')
        self.assertEqual(self.a.entry(4, 982 + 38), b'Sb38')
        self.assertEqual(self.a.entry(4, 1021), b'Hb0')
        self.assertEqual(len(self.a.group(4)[1]), 1060)

    def test_scenes_and_art_per_guest(self):
        self.assertEqual(self.a.entry(4, 7), b'Ss7')
        self.assertEqual(self.a.entry(4, 33), b'Hs33')
        self.assertTrue(self.a.entry(2, 85) and self.a.entry(2, 86))

    def test_faces_and_silhouettes_follow_in_order(self):
        cells = [bytes.fromhex(g['cell']) for g in self.m['guests']]
        self.assertEqual((list(cells[0][8:10]), list(cells[1][8:10])), ([77, 78], [79, 80]))
        self.assertEqual((list(cells[0][16:18]), list(cells[1][16:18])), ([150, 151], [152, 153]))
        self.assertEqual(ui.decode_face(self.a.entry(*ui.FACE_ENTRY), 79), ui.decode_face(self.a.entry(*ui.FACE_ENTRY), 77))

    def test_hash_depends_on_the_guests(self):
        self.assertEqual(len(self.m['guests_sha1']), 40)
        self.assertNotEqual(self.m['guests_sha1'], self.m1['guests_sha1'])

    def test_manifest_on_disk_is_format_2(self):
        m = json.load(open(os.path.join(self.both, 'guests.json')))
        self.assertEqual(m, self.m)
        self.assertEqual(m['format'], 2)
        self.assertEqual(m['size'], os.path.getsize(os.path.join(self.both, 'root-guests.olk')))
        self.assertNotIn('name', m['guests'][1])

    def test_scene_record_taken(self):
        with tempfile.TemporaryDirectory(dir=os.environ['RINGOUT_SPAWN_WORK']) as d:
            with self.assertRaisesRegex(ValueError, 'not empty'):
                build_guests.build([stub(28, 9, (16,), b'H')], olk.G, d, DOL)
            self.assertEqual(os.listdir(d), [])

    def test_two_guests_may_not_share_a_scene_record(self):
        with tempfile.TemporaryDirectory(dir=os.environ['RINGOUT_SPAWN_WORK']) as d:
            with self.assertRaisesRegex(ValueError, 'two guests'):
                build_guests.build([stub(28, 9, (7,), b'H'), stub(29, 4, (7,), b'S')], olk.G, d, DOL)


class Pools(unittest.TestCase):
    BASE = 0x8028B50C

    def test_pools_both(self):
        block = bytes([1, 27, 3, 255, 2, 255]) + bytes(10)
        table = struct.pack('>I', self.BASE)
        b, t = tables.repack_pools(block, self.BASE, table, {29: [[1, 29, 3]], 28: [[1, 28, 3], [2]]})
        self.assertEqual(tables.pool_lists(b), [[1, 27, 28, 29, 3], [2]])
        self.assertEqual(len(b), len(block))


if __name__ == '__main__':
    unittest.main()
