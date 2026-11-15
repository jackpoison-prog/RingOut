"""setup_guests: every check before any work, and an install that is never half-written. Synthetic discs and a
fake build (a real one is in test_build); the release table is injected so a synthetic image is 'known'."""
import collections, dataclasses, hashlib, io, json, os, stat, struct, sys, tempfile, unittest
from unittest import mock
for d in ('..', '../../spawn', '../../heihachi', '../../spawn/tests', '.'):
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), d))
import build_guests, build_spawn, ps2iso, releases, setup_guests
from ximage import make_image

H = lambda b: hashlib.sha1(b).hexdigest()
XROOT, XBE = b'xbox-olnk' * 500, b'XBEH' * 300
GCROOT = b'gc-olnk' * 700
REL = dataclasses.replace(releases.US, root_sha1=H(XROOT), xbe_sha1=(H(XBE),))


class Fake:
    """Stands in for setup_guests.build: writes a small archive and a manifest, counts calls, can fail."""
    def __init__(self, fail=None):
        self.calls, self.fail = 0, fail

    def __call__(self, out_dir, game, spawn, heihachi):
        self.calls += 1
        a = os.path.join(out_dir, 'root-guests.olk')
        with open(a, 'wb') as f: f.write(b'NEW' * 1000)
        if self.fail: raise self.fail
        guests = []
        if spawn:
            rel, _, xbe = spawn
            guests.append({'id': 29, 'source': {'release': rel.name, 'root_sha1': rel.root_sha1, 'xbe_sha1': releases.sha1_view(xbe)}})
        if heihachi:
            rel, _, _ = heihachi
            guests.append({'id': 28, 'source': {'release': rel.name, 'root_sha1': rel.root_sha1, 'elf_sha1': rel.elf_sha1}})
        m = {'format': 2, 'disc': 'GRSEAF', 'converter': build_guests._converter_version(), 'gc_root_sha1': H(GCROOT),
             'sha1': H(b'NEW' * 1000), 'size': 3000, 'guests_sha1': '2' * 40, 'guests': guests}
        with open(os.path.join(out_dir, 'guests.json'), 'w') as f: json.dump(m, f)
        return m


def make_ps2_image(path, files):
    """A plain ISO 9660 image holding `files` in its root directory."""
    S = 2048; names = list(files); at = 20; recs = b''; body = {}
    for n in names:
        fid = (n + ';1').encode(); ln = 33 + len(fid); ln += ln % 2
        r = bytearray(ln); r[0] = ln
        struct.pack_into('<I', r, 2, at); struct.pack_into('<I', r, 10, len(files[n])); r[32] = len(fid); r[33:33 + len(fid)] = fid
        recs += bytes(r); body[at] = files[n]; at += (len(files[n]) + S - 1) // S
    pvd = bytearray(S); pvd[0] = 1; pvd[1:6] = b'CD001'; pvd[8:19] = b'PLAYSTATION'
    struct.pack_into('<I', pvd, 158, 18); struct.pack_into('<I', pvd, 166, len(recs))
    with open(path, 'wb') as f:
        f.write(bytes(16 * S)); f.write(pvd); f.write(bytes(S)); f.write(recs + bytes(2 * S - len(recs)))
        for a in sorted(body):
            f.seek(a * S); f.write(body[a])
        f.seek(at * S); f.write(bytes(S))


PROOT, PELF = b'ps2-olnk' * 500, b'\x7fELF' * 300
PS2REL = ps2iso.Release('US 1.00', H(PROOT), H(PELF), 'SLUS_206.43')


class DriverTest(unittest.TestCase):
    def setUp(self):
        d = tempfile.TemporaryDirectory(); self.addCleanup(d.cleanup); self.d = d.name
        self.game = os.path.join(self.d, 'install', 'game')
        for sub in ('sys', 'files'): os.makedirs(os.path.join(self.game, sub))
        self.put('sys/boot.bin', b'GRSEAF' + bytes(26)); self.put('sys/main.dol', b'dol')
        self.put('files/root.olk', GCROOT)
        self.image = os.path.join(self.d, 'My Xbox Disc.iso')
        make_image(self.image, {'root.olk': XROOT, 'Default.xbe': XBE})
        self.ps2 = os.path.join(self.d, 'My PS2 Disc.iso')
        make_ps2_image(self.ps2, {'ROOT.OLK': PROOT, 'SLUS_206.43': PELF})
        self.log = os.path.join(self.d, 'install', 'work', 'guests.log')

    def put(self, rel, data):
        with open(os.path.join(self.game, rel), 'wb') as f: f.write(data)

    def run_(self, build=None, spawn=True, heihachi=False, **kw):
        out = io.StringIO()
        with mock.patch('sys.stdout', out):
            code = setup_guests.run(self.image if spawn is True else spawn or None,
                                    self.ps2 if heihachi is True else heihachi or None, self.game, self.log,
                                    build=build or Fake(), spawn_table=(REL,), heihachi_table=(PS2REL,),
                                    gc_sha1=H(GCROOT), **kw)
        return code, out.getvalue().strip()

    def spawn(self, name='root-guests.olk'):
        with open(os.path.join(self.game, 'guests', name), 'rb') as f: return f.read()

    # ---- refusals: exit 3, nothing written
    def assertRefused(self, code_msg, msg):
        self.assertEqual(code_msg, (3, msg)); self.assertFalse(os.path.exists(os.path.join(self.game, 'guests')))

    def test_not_an_image(self):
        with open(self.image, 'wb') as f: f.write(bytes(70000))
        self.assertRefused(self.run_(), 'not an Xbox disc image')

    def test_unknown_release(self):
        make_image(self.image, {'root.olk': b'other', 'Default.xbe': XBE})
        self.assertRefused(self.run_(), 'this Xbox disc is not a supported release of Soul Calibur II (supported: US)')

    def test_other_gamecube_region(self):
        self.put('sys/boot.bin', b'GRSPAF' + bytes(26))
        self.assertRefused(self.run_(), 'the guests need the US GameCube disc (this install is GRSPAF)')

    def test_modded_game_data(self):
        self.put('files/root.olk', GCROOT + b'mod')
        self.assertRefused(self.run_(), 'the game data is modified (mods applied): remove the mods or rerun setup first')

    def test_no_install(self):
        os.remove(os.path.join(self.game, 'files', 'root.olk'))
        self.assertRefused(self.run_(), 'no GameCube install found in %s' % self.game)

    def test_not_enough_space(self):
        du = collections.namedtuple('du', 'total used free')(10 ** 9, 0, 1000)
        with mock.patch('shutil.disk_usage', return_value=du):
            code, msg = self.run_()
        self.assertEqual(code, 3); self.assertRegex(msg, r'^not enough free space: the guests need [0-9.]+ GB$')

    def test_space_for_the_builds_peak(self):
        """The build holds a staging archive and the final one at once: twice the GameCube archive."""
        du = collections.namedtuple('du', 'total used free')(10 ** 9, 0, int(1.5 * len(GCROOT)) + setup_guests.HEADROOM)
        with mock.patch('shutil.disk_usage', return_value=du):
            code, msg = self.run_()
        self.assertEqual(code, 3); self.assertRegex(msg, r'^not enough free space')

    def test_check_image_only_skips_the_gamecube_checks(self):
        self.put('sys/boot.bin', b'GRSPAF' + bytes(26))
        b = Fake()
        self.assertEqual(self.run_(build=b, check_only=True), (0, 'Spawn: US Xbox disc recognised'))
        self.assertEqual(b.calls, 0)

    # ---- building
    def test_builds(self):
        self.assertEqual(self.run_(), (0, 'Spawn built'))
        self.assertEqual(self.spawn(), b'NEW' * 1000)
        self.assertEqual(sorted(os.listdir(self.game)), ['files', 'guests', 'sys'])
        self.assertTrue(os.path.isfile(self.log))

    def test_already_built_does_no_work(self):
        self.run_(); b = Fake()
        self.assertEqual(self.run_(build=b), (0, 'Spawn already built'))
        self.assertEqual(b.calls, 0)

    def test_rebuilds_when_the_archive_is_damaged(self):
        self.run_()
        with open(os.path.join(self.game, 'guests', 'root-guests.olk'), 'ab') as f: f.write(b'x')
        b = Fake()
        self.assertEqual(self.run_(build=b)[0], 0); self.assertEqual(b.calls, 1)

    def test_failure_keeps_the_previous_spawn(self):
        self.run_()
        with open(os.path.join(self.game, 'guests', 'root-guests.olk'), 'ab') as f: f.write(b'old')  # force a rebuild
        before = (self.spawn(), self.spawn('guests.json'))
        code, msg = self.run_(build=Fake(fail=ValueError('costume is too big')))
        self.assertEqual((code, msg), (1, 'Spawn could not be built (details: %s)' % self.log))
        self.assertEqual((self.spawn(), self.spawn('guests.json')), before)
        self.assertEqual(sorted(os.listdir(self.game)), ['files', 'guests', 'sys'])
        self.assertIn('costume is too big', open(self.log).read())

    def test_interrupt_is_a_failure(self):
        code, _ = self.run_(build=Fake(fail=KeyboardInterrupt()))
        self.assertEqual(code, 1)
        self.assertEqual(sorted(os.listdir(self.game)), ['files', 'sys'])

    def test_a_swap_cut_short_is_repaired(self):
        """Killed between moving the old Spawn aside and moving the new one in: the next run restores it."""
        self.run_()
        os.rename(os.path.join(self.game, 'guests'), os.path.join(self.game, 'guests.old'))
        b = Fake()
        self.assertEqual(self.run_(build=b), (0, 'Spawn already built'))
        self.assertEqual(sorted(os.listdir(self.game)), ['files', 'guests', 'sys'])

    # ---- review focus
    def test_install_with_only_a_game_folder(self):
        self.assertEqual(os.listdir(os.path.dirname(self.game)), ['game'])
        self.assertEqual(self.run_()[0], 0)

    def test_old_archive_open_during_the_swap(self):
        self.run_(build=Fake())
        with open(os.path.join(self.game, 'guests', 'root-guests.olk'), 'ab') as f: f.write(b'old')  # force a rebuild
        with open(os.path.join(self.game, 'guests', 'root-guests.olk'), 'rb') as held:
            self.assertEqual(self.run_()[0], 0)
            self.assertEqual(self.spawn(), b'NEW' * 1000)
            self.assertTrue(held.read().endswith(b'old'))

    def test_read_only_image(self):
        os.chmod(self.image, stat.S_IRUSR | stat.S_IRGRP | stat.S_IROTH)
        self.assertEqual(self.run_()[0], 0)


class TwoDiscsTest(DriverTest):
    # ---- Heihachi and both
    def test_heihachi_alone(self):
        self.assertEqual(self.run_(spawn=False, heihachi=True), (0, 'Heihachi built'))
        with open(os.path.join(self.game, 'guests', 'guests.json')) as f:
            self.assertEqual([g['id'] for g in json.load(f)['guests']], [28])

    def test_both(self):
        self.assertEqual(self.run_(heihachi=True), (0, 'Spawn and Heihachi built'))
        b = Fake()
        self.assertEqual(self.run_(build=b, heihachi=True), (0, 'Spawn and Heihachi already built'))
        self.assertEqual(b.calls, 0)

    def test_check_image_names_both(self):
        self.assertEqual(self.run_(heihachi=True, check_only=True),
                         (0, 'Spawn: US Xbox disc recognised\nHeihachi: US 1.00 PS2 disc recognised'))

    def test_ps2_files_hashed_once(self):
        """Hashing the PS2 files is the slow part of the check: once each per run, not twice."""
        real = ps2iso.sha1_view
        with mock.patch.object(ps2iso, 'sha1_view', side_effect=real) as h:
            self.assertEqual(self.run_(spawn=False, heihachi=True, check_only=True)[0], 0)
        self.assertEqual(h.call_count, 2)

    def test_nothing_to_build(self):
        self.assertEqual(self.run_(spawn=False)[0], 3)

    def test_missing_flag(self):
        self.run_()
        self.assertEqual(self.run_(spawn=False, heihachi=True),
                         (3, 'Spawn is installed and would be lost: give his image too with --spawn'))
        self.assertEqual(self.spawn(), b'NEW' * 1000)

    def test_missing_flag_for_heihachi(self):
        self.run_(spawn=False, heihachi=True)
        self.assertEqual(self.run_(), (3, 'Heihachi is installed and would be lost: give his image too with --heihachi'))

    def test_old_format_spawn_counts_as_installed(self):
        os.makedirs(os.path.join(self.game, 'spawn'))
        code, msg = self.run_(spawn=False, heihachi=True)
        self.assertEqual(code, 3); self.assertIn('--spawn', msg)

    # ---- Review Focus 4
    def test_wrong_disc_each_way(self):
        code, msg = self.run_(spawn=False, heihachi=self.image)
        self.assertEqual((code, msg), (3, 'My Xbox Disc.iso is not a PS2 disc image'))
        self.assertEqual(self.run_(spawn=self.ps2), (3, 'not an Xbox disc image'))
        other = os.path.join(self.d, 'other.iso')
        make_ps2_image(other, {'ROOT.OLK': b'another', 'SLUS_206.43': PELF})
        code, msg = self.run_(spawn=False, heihachi=other)
        self.assertEqual(code, 3); self.assertIn('not a supported release', msg)
        cut = os.path.join(self.d, 'cut.iso')
        with open(self.ps2, 'rb') as f: data = f.read()
        with open(cut, 'wb') as f: f.write(data[:21 * 2048])
        code, msg = self.run_(spawn=False, heihachi=cut)
        self.assertEqual((code, msg), (3, 'cut.iso is not a PS2 disc image: it is cut short'))
        self.assertEqual(sorted(os.listdir(self.game)), ['files', 'sys'])

    def test_a_game_that_is_not_soul_calibur(self):
        other = os.path.join(self.d, 'other.iso')
        make_ps2_image(other, {'SOMETHING.BIN': b'x'})
        self.assertEqual(self.run_(spawn=False, heihachi=other), (3, 'this PS2 disc image is not Soul Calibur II'))

    # ---- Review Focus 3
    def test_interrupted_keeps_old(self):
        old = os.path.join(self.game, 'spawn'); os.makedirs(old)
        with open(os.path.join(old, 'root-spawn.olk'), 'wb') as f: f.write(b'OLD')
        for fail in (ValueError('bad'), KeyboardInterrupt()):
            self.assertEqual(self.run_(build=Fake(fail=fail), heihachi=True)[0], 1)
            with open(os.path.join(old, 'root-spawn.olk'), 'rb') as f: self.assertEqual(f.read(), b'OLD')
            self.assertEqual(sorted(os.listdir(self.game)), ['files', 'spawn', 'sys'])
        self.assertEqual(self.run_(heihachi=True), (0, 'Spawn and Heihachi built'))

    def test_swap_removes_old(self):
        old = os.path.join(self.game, 'spawn'); os.makedirs(old)
        seen = {}
        def watch(out_dir, game, spawn, heihachi):
            seen['spawn during build'] = os.path.isdir(old)
            return Fake()(out_dir, game, spawn, heihachi)
        self.assertEqual(self.run_(build=watch)[0], 0)
        self.assertTrue(seen['spawn during build'])
        self.assertEqual(sorted(os.listdir(self.game)), ['files', 'guests', 'sys'])

    def test_already_built_needs_the_same_guests(self):
        self.run_(heihachi=True); b = Fake()
        with open(os.path.join(self.game, 'guests', 'guests.json')) as f: m = json.load(f)
        m['guests'].pop()
        with open(os.path.join(self.game, 'guests', 'guests.json'), 'w') as f: json.dump(m, f)
        self.assertEqual(self.run_(build=b, heihachi=True)[0], 0); self.assertEqual(b.calls, 1)


if __name__ == '__main__':
    unittest.main()
