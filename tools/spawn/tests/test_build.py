"""build_spawn: Spawn alone through build_guests, root-guests.olk + guests.json.

The layout test needs the discs and RINGOUT_SPAWN_WORK (a directory with ~600 MB free; not tmpfs) and
takes ~4 minutes. The interruption test runs anywhere on a synthetic archive."""
import hashlib, json, os, sys, tempfile, unittest
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'guests'))
sys.path.insert(0, os.path.dirname(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'guests', 'tests'))
import olk, build_spawn, tables, ui
from test_olkw import make_olk

XBE, DOL = os.environ.get('RINGOUT_SPAWN_XBE', ''), os.environ.get('RINGOUT_SPAWN_DOL', '')
HAVE = (os.path.isfile(olk.X) and os.path.isfile(olk.G) and os.path.isfile(XBE) and os.path.isfile(DOL)
        and os.path.isdir(os.environ.get('RINGOUT_SPAWN_WORK', '')))


def sha1(path):
    h = hashlib.sha1()
    with open(path, 'rb') as f:
        for c in iter(lambda: f.read(1 << 24), b''): h.update(c)
    return h.hexdigest()


class NoNumpyTest(unittest.TestCase):
    def test_no_numpy(self):
        """Players have no numpy: nothing the converter runs may import it (only the test oracle does)."""
        import re
        here = os.path.join(os.path.dirname(__file__), '..')
        users = [n for n in sorted(os.listdir(here)) + ['tests/' + t for t in sorted(os.listdir(os.path.join(here, 'tests')))]
                 if n.endswith('.py') and n != 'tests/ref_dsp_numpy.py'
                 and re.search(r'^\s*(import numpy\b|from numpy\b|import .*\bnumpy\b)', open(os.path.join(here, n)).read(), re.M)]
        self.assertEqual(users, [])


@unittest.skipUnless(HAVE, 'needs RINGOUT_SPAWN_XBOX, _GC, _XBE, _DOL and _WORK')
class BuildLayoutTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.out = tempfile.mkdtemp(dir=os.environ['RINGOUT_SPAWN_WORK'])
        cls.man = build_spawn.build(olk.X, olk.G, cls.out, XBE, DOL)
        cls.g, cls.s = olk.Olk(olk.G), olk.Olk(os.path.join(cls.out, 'root-guests.olk'))

    @classmethod
    def tearDownClass(cls):
        for f in os.listdir(cls.out): os.remove(os.path.join(cls.out, f))
        os.rmdir(cls.out)

    def test_pal_build_identical_to_build15(self):
        """Removing numpy changed no archive byte: build 15 (2026-11-04, numpy converters) is the reference.
        guests_sha1 also covers his table rows, which have changed since."""
        self.assertEqual(len(self.man['guests_sha1']), 40)
        self.assertEqual(self.man['sha1'], '93a6306659c44f51b9d2b462436108b0a5e63190')

    def test_original_entries_unchanged(self):
        for gi in range(len(self.g.top[1])):
            if not self.g.top[1][gi][1]: continue
            ents = self.g.group(gi)[1]
            for i in range(len(ents)):
                if (gi, i) in ((2, 86), (2, 5), (2, 10), (2, 503), (4, 7), (4, 23)): continue
                self.assertEqual(self.s.entry(gi, i), self.g.entry(gi, i), (gi, i))

    def test_spawn_block_appended(self):
        self.assertEqual(len(self.s.group(4)[1]), 1021)
        blk = build_spawn.spawn_block()
        for slot in range(39):
            self.assertEqual(self.s.entry(4, 982 + slot), blk.get(slot, b''), slot)

    def test_profile_camera_slots(self):
        """His three profile-camera slots are filled; every other part of entry 503 is the GC's own."""
        g = olk.pack(olk.Olk(olk.G).entry(*ui.PROFILE_CAMERA), True); s = olk.pack(self.s.entry(*ui.PROFILE_CAMERA), True)
        for k in range(96):
            if k in (29, 61, 93): self.assertTrue(s[k], k)
            else: self.assertEqual(s[k], g[k], k)

    def test_scene_records_7_and_23(self):
        """His Xbox home-stage scene (17 Japanese, 33 English) in the records ID 29 reaches on GC."""
        self.assertEqual(self.s.entry(4, 7), build_spawn.spawn_scenes()[7])
        self.assertEqual(self.s.entry(4, 23), build_spawn.spawn_scenes()[23])
        for r in (7, 23):
            p = olk.pack(self.s.entry(4, r), True)
            self.assertEqual(p[1][0x10:0x12], bytes([0, 29]), r)  # his ID in the first motion record

    def test_ending_art_in_record_86(self):
        self.assertEqual(len(self.s.entry(2, 86)), len(self.g.entry(2, 79)))

    def test_his_faces_in_entry_5(self):
        self.assertEqual(self.s.entry(2, 5), ui.face_bank(olk.Olk(olk.X).entry(2, 5), self.g.entry(2, 5))[0])

    def test_his_silhouettes_in_entry_10(self):
        self.assertEqual(self.s.entry(2, 10), ui.silhouette_bank(olk.Olk(olk.X).entry(2, 10), self.g.entry(2, 10))[0])

    def test_manifest(self):
        m = json.load(open(os.path.join(self.out, 'guests.json')))
        self.assertEqual(m, self.man)
        self.assertEqual(m['format'], 2)
        self.assertEqual(m['disc'], 'GRSEAF')
        self.assertEqual(m['gc_root_sha1'], build_spawn.GRSEAF_ROOT_SHA1)
        self.assertEqual(m['sha1'], sha1(os.path.join(self.out, 'root-guests.olk')))
        self.assertEqual(m['size'], os.path.getsize(os.path.join(self.out, 'root-guests.olk')))
        self.assertEqual(m['edits'], [{'group': 4, 'append_first': 982, 'count': 39},
                                      {'group': 4, 'replace': 7}, {'group': 4, 'replace': 23},
                                      {'group': 2, 'replace': 5}, {'group': 2, 'replace': 10},
                                      {'group': 2, 'replace': 86}, {'group': 2, 'replace': 503}])
        self.assertEqual(len(m['guests_sha1']), 40)
        t = tables.spawn_tables(XBE, DOL)
        pool = tables.pools(tables.Dol(DOL), {29: t['pool_lists']})
        self.assertEqual({k: m[k] for k in pool}, {k: v.hex() for k, v in pool.items()})
        g = dict(m['guests'][0]); g.pop('source')
        self.assertEqual(g, {'id': 29, 'block': 982, 'roster': t['roster'], 'costume': t['costume'].hex(),
                             'descriptor': t['descriptor'].hex(), 'strings': t['strings'].hex(),
                             'cell': tables.cell_with_silhouettes(tables.cell_with_faces(t['cell'], (77, 78)),
                                                                 (150, 151), t['xcell']).hex(),
                             'ending': t['ending'].hex(), 'ending_old': t['ending_old'].hex(),
                             'voice': t['voice'], 'name': t['name'].hex(), 'preview': t['preview'].hex()})


class InterruptedBuildTest(unittest.TestCase):
    def test_interrupted_build_leaves_no_manifest(self):
        d = tempfile.TemporaryDirectory(); self.addCleanup(d.cleanup)
        src = os.path.join(d.name, 'root.olk')
        with open(src, 'wb') as f: f.write(make_olk([[b'x'], [b'y'], [b'z'], [b'w'], [b'v']]))
        out = os.path.join(d.name, 'out'); os.mkdir(out)

        def boom():
            raise RuntimeError('conversion failed')
        with self.assertRaises(RuntimeError):
            build_spawn.build('unused', src, out, 'unused', 'unused', check_gc=False, block_fn=boom,
                              tables_fn=lambda: {}, scenes_fn=lambda: {})
        self.assertEqual(os.listdir(out), [])

    def test_wrong_gc_disc_refused(self):
        d = tempfile.TemporaryDirectory(); self.addCleanup(d.cleanup)
        src = os.path.join(d.name, 'root.olk')
        with open(src, 'wb') as f: f.write(make_olk([[b'x']]))
        with self.assertRaisesRegex(ValueError, 'GRSEAF'):
            build_spawn.build('unused', src, d.name, 'unused', 'unused')


if __name__ == '__main__':
    unittest.main()


US_IMAGE = os.environ.get('RINGOUT_SPAWN_IMAGE_US', '')
US_FILES = os.environ.get('RINGOUT_SPAWN_US_FILES', '')   # a folder with the same disc's root.olk and its original
                                                          # executable (orig_Default.xbe or default.xbe)


@unittest.skipUnless(os.path.isfile(US_IMAGE) and os.path.isfile(olk.G) and os.path.isfile(DOL)
                     and os.path.isdir(os.environ.get('RINGOUT_SPAWN_WORK', '')),
                     'needs RINGOUT_SPAWN_IMAGE_US, _GC, _DOL and _WORK')
class USBuildTest(unittest.TestCase):
    """The US Xbox release, built straight from its image."""
    @classmethod
    def setUpClass(cls):
        import releases, xiso
        cls.files = xiso.open_image(US_IMAGE)
        cls.rel, root, cls.xbe = releases.identify(cls.files)
        cls.out = tempfile.mkdtemp(dir=os.environ['RINGOUT_SPAWN_WORK'])
        cls.man = build_spawn.build(root, olk.G, cls.out, cls.xbe, DOL, release=cls.rel)

    @classmethod
    def tearDownClass(cls):
        for f in os.listdir(cls.out): os.remove(os.path.join(cls.out, f))
        os.rmdir(cls.out)

    def test_identified_as_us_by_its_original_executable(self):
        self.assertEqual(self.rel.name, 'US')
        import xiso
        self.assertEqual(self.xbe.offset, xiso.find(self.files, 'orig_Default.xbe').offset)

    def test_manifest_names_the_xbox_disc(self):
        import releases
        self.assertEqual(self.man['guests'][0]['source'], {'release': 'US', 'root_sha1': releases.US.root_sha1,
                                            'xbe_sha1': 'e5d4fb1f709154436fe25cf5c3d7ba5b7f42da84'})
        with open(os.path.join(self.out, 'guests.json')) as f: self.assertEqual(json.load(f)['guests'][0]['source'], self.man['guests'][0]['source'])

    def test_setup_driver_end_to_end(self):
        """setup_spawn on an install made of links to the real GameCube files: builds, then 'already built'."""
        import io, setup_guests
        from unittest import mock
        top = tempfile.mkdtemp(dir=os.environ['RINGOUT_SPAWN_WORK']); game = os.path.join(top, 'game')
        try:
            os.makedirs(os.path.join(game, 'files')); os.makedirs(os.path.join(game, 'sys'))
            os.symlink(olk.G, os.path.join(game, 'files', 'root.olk'))
            os.symlink(DOL, os.path.join(game, 'sys', 'main.dol'))
            os.symlink(os.path.join(os.path.dirname(DOL), 'boot.bin'), os.path.join(game, 'sys', 'boot.bin'))
            log = os.path.join(top, 'work', 'spawn.log')
            for want in ('Spawn built', 'Spawn already built'):
                out = io.StringIO()
                with mock.patch('sys.stdout', out): code = setup_guests.run(US_IMAGE, None, game, log)
                self.assertEqual((code, out.getvalue().strip()), (0, want))
            with open(os.path.join(game, 'guests', 'guests.json')) as f:
                self.assertEqual(json.load(f)['guests_sha1'], self.man['guests_sha1'])
        finally:
            import shutil; shutil.rmtree(top)

    def test_archive_valid(self):
        s = olk.Olk(os.path.join(self.out, 'root-guests.olk'))
        self.assertEqual(len(s.group(4)[1]), 1021)
        self.assertEqual(sha1(os.path.join(self.out, 'root-guests.olk')), self.man['sha1'])

    def test_both_executables_give_the_same_tables(self):
        import xiso
        a = tables.spawn_tables(xiso.find(self.files, 'orig_Default.xbe'), DOL, self.rel.faces)
        b = tables.spawn_tables(xiso.find(self.files, 'Default.xbe'), DOL, self.rel.faces)
        self.assertEqual(a, b)

    @unittest.skipUnless(os.path.isdir(US_FILES), 'needs RINGOUT_SPAWN_US_FILES')
    def test_image_and_extracted_files_build_the_same(self):
        out = tempfile.mkdtemp(dir=os.environ['RINGOUT_SPAWN_WORK'])
        try:
            m = build_spawn.build(os.path.join(US_FILES, 'root.olk'), olk.G, out,
                                  next(os.path.join(US_FILES, n) for n in ('orig_Default.xbe', 'default.xbe')
                                       if os.path.isfile(os.path.join(US_FILES, n))), DOL, release=self.rel)
            self.assertEqual((m['sha1'], m['guests_sha1']), (self.man['sha1'], self.man['guests_sha1']))
        finally:
            for f in os.listdir(out): os.remove(os.path.join(out, f))
            os.rmdir(out)
