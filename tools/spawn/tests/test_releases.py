"""Release table and identify(), on synthetic images with an injected table."""
import dataclasses, hashlib, os, sys, tempfile, unittest
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'guests'))
sys.path.insert(0, os.path.dirname(__file__))
import releases, xiso
from ximage import make_image

ROOT, XBE_A, XBE_B, XBE_C = b'olnk' * 300, b'XBEH-orig' * 50, b'XBEH-patched' * 50, b'XBEH-unknown' * 50
h = lambda b: hashlib.sha1(b).hexdigest()
FAKE = dataclasses.replace(releases.PAL, name='US', root_sha1=h(ROOT), xbe_sha1=(h(XBE_A), h(XBE_B)))
TABLE = (dataclasses.replace(releases.PAL, name='PAL', root_sha1='0' * 40), FAKE)


class IdentifyTest(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.TemporaryDirectory(); self.addCleanup(self.d.cleanup)

    def image(self, files):
        p = os.path.join(self.d.name, 'x.iso'); make_image(p, files)
        return xiso.open_image(p)

    def refusal(self, files):
        with self.assertRaises(releases.Refusal) as c: releases.identify(self.image(files), table=TABLE)
        return str(c.exception)

    def test_known_release(self):
        rel, root, xbe = releases.identify(self.image({'root.olk': ROOT, 'Default.xbe': XBE_B}), table=TABLE)
        self.assertIs(rel, FAKE)
        self.assertEqual(root.read_all(), ROOT); self.assertEqual(xbe.read_all(), XBE_B)

    def test_orig_default_preferred(self):
        _, _, xbe = releases.identify(self.image({'root.olk': ROOT, 'Default.xbe': XBE_B, 'orig_Default.xbe': XBE_A}),
                                      table=TABLE)
        self.assertEqual(xbe.read_all(), XBE_A)

    def test_unknown_orig_falls_back_to_default(self):
        _, _, xbe = releases.identify(self.image({'root.olk': ROOT, 'Default.xbe': XBE_B, 'orig_Default.xbe': XBE_C}),
                                      table=TABLE)
        self.assertEqual(xbe.read_all(), XBE_B)

    def test_unknown_root(self):
        self.assertEqual(self.refusal({'root.olk': b'other' * 9, 'Default.xbe': XBE_A}),
                         'this Xbox disc is not a supported release of Soul Calibur II (supported: PAL, US)')

    def test_unknown_xbe(self):
        self.assertEqual(self.refusal({'root.olk': ROOT, 'Default.xbe': XBE_C}),
                         'unrecognised Default.xbe (patched?) for the US release')

    def test_no_root(self):
        self.assertEqual(self.refusal({'Default.xbe': XBE_A}), 'not Soul Calibur II (no root.olk on the disc)')

    def test_no_xbe(self):
        self.assertEqual(self.refusal({'root.olk': ROOT}), 'not Soul Calibur II (no Default.xbe on the disc)')

    def test_real_table_names(self):
        self.assertEqual([r.name for r in releases.RELEASES], ['PAL', 'US'])


if __name__ == '__main__':
    unittest.main()


class DialogueLayoutTest(unittest.TestCase):
    """Pre-fight lines: the PAL disc keeps six columns (header 0x10); the US disc one (header 1), already in
    the GameCube's layout, padded with zeros."""
    @staticmethod
    def one_column(lines, pad=22):
        import struct
        strs = [l + b'\0' for l in lines]
        at = [sum(len(s) for s in strs[:i]) for i in range(len(strs))]
        return struct.pack('<II', 1, len(strs)) + struct.pack('<%dI' % len(strs), *at) + b''.join(strs) + bytes(pad)

    def test_one_column_passes_through_without_padding(self):
        import xb2gc
        x = self.one_column([b"Said your prayers?", b"Got no time for this."])
        self.assertEqual(xb2gc.dialogue(x, 1), x[:-22])

    def test_one_column_refuses_a_six_column_header(self):
        import struct, xb2gc
        x = struct.pack('<II', 0x10, 0)
        with self.assertRaises(ValueError): xb2gc.dialogue(x, 1)

    def test_one_column_refuses_an_offset_past_the_end(self):
        import struct, xb2gc
        x = struct.pack('<III', 1, 1, 400) + b'abc\0'
        with self.assertRaises(ValueError): xb2gc.dialogue(x, 1)
