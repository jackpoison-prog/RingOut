"""XDVDFS reader on synthetic images."""
import os, sys, tempfile, unittest
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'guests'))
sys.path.insert(0, os.path.dirname(__file__))
import xiso
from ximage import make_image

FILES = {'root.olk': bytes(range(256)) * 40, 'Default.xbe': b'XBEH' + b'\x01' * 5000}


class XisoTest(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.TemporaryDirectory(); self.p = os.path.join(self.d.name, 'g.iso')

    def tearDown(self):
        self.d.cleanup()

    def test_trimmed_image_lists_root(self):
        make_image(self.p, FILES)
        fs = xiso.open_image(self.p)
        self.assertEqual(set(fs), {'root.olk', 'Default.xbe'})
        self.assertEqual(xiso.find(fs, 'default.xbe').read_all(), FILES['Default.xbe'])
        self.assertEqual(xiso.find(fs, 'ROOT.OLK').read(256, 4), bytes([0, 1, 2, 3]))
        self.assertIsNone(xiso.find(fs, 'nope'))

    def test_full_dump_offsets(self):
        for part in (0x18300000, 0x0FD90000):
            make_image(self.p, FILES, partition=part)
            fs = xiso.open_image(self.p)
            self.assertEqual(xiso.find(fs, 'root.olk').read_all(), FILES['root.olk'], hex(part))

    def test_not_xbox(self):
        with open(self.p, 'wb') as f: f.write(bytes(65536))
        with self.assertRaises(xiso.ImageError) as c: xiso.open_image(self.p)
        self.assertEqual(str(c.exception), 'not an Xbox disc image')

    def test_truncated(self):
        make_image(self.p, {'Default.xbe': FILES['Default.xbe'], 'root.olk': FILES['root.olk']}, truncate=1)
        with self.assertRaises(xiso.ImageError) as c: xiso.open_image(self.p)
        self.assertEqual(str(c.exception), 'the image is incomplete (it ends before root.olk does)')

    def test_read_bounds(self):
        make_image(self.p, FILES)
        v = xiso.find(xiso.open_image(self.p), 'root.olk')
        with self.assertRaises(ValueError): v.read(v.size - 4, 8)


if __name__ == '__main__':
    unittest.main()
