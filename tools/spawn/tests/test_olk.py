"""Readers opened on a view into a disc image give what they give on a plain file."""
import os, shutil, struct, subprocess, sys, tempfile, unittest
from unittest import mock
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'guests'))
sys.path.insert(0, os.path.dirname(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'guests', 'tests'))
import olk, tables, xiso, build_spawn
from test_olkw import make_olk

GROUPS = [[b'top0' * 3], [b'a' * 100, b'', b'b' * 3000, b'c' * 7]]


def fake_xbe():
    """Base 0x10000, one section: va 0x11000, 0x100 bytes at file offset 0x1000."""
    d = bytearray(0x1100)
    struct.pack_into('<I', d, 0x104, 0x10000)
    struct.pack_into('<II', d, 0x11C, 1, 0x10200)
    struct.pack_into('<5I', d, 0x200, 0, 0x11000, 0x100, 0x1000, 0x100)
    d[0x1000:0x1100] = bytes(range(256))
    return bytes(d)


class ViewTest(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.TemporaryDirectory(); self.addCleanup(self.d.cleanup)

    def embed(self, data, at=4096):
        p = os.path.join(self.d.name, 'img')
        with open(p, 'wb') as f: f.write(b'\xee' * at + data + b'\xee' * 77)
        return xiso.FileView(p, at, len(data))

    def test_olk_from_view(self):
        data = make_olk(GROUPS)
        p = os.path.join(self.d.name, 'a.olk')
        with open(p, 'wb') as f: f.write(data)
        a, b = olk.Olk(p), olk.Olk(self.embed(data))
        for i in range(4):
            self.assertEqual(b.entry(1, i), a.entry(1, i))
        self.assertEqual(b.group(1), a.group(1))

    def test_xbe_from_view(self):
        x = tables.Xbe(self.embed(fake_xbe()))
        self.assertEqual(x.read(0x11010, 4), bytes([16, 17, 18, 19]))


class ConverterVersionTest(unittest.TestCase):
    def test_converter_version_without_git(self):
        with mock.patch.object(subprocess, 'run', side_effect=FileNotFoundError('git')):
            v = build_spawn._converter_version()
        self.assertRegex(v, '^[0-9a-f]{40}$')

    def test_converter_version_follows_source_bytes(self):
        here = os.path.dirname(os.path.abspath(build_spawn.__file__))
        with tempfile.TemporaryDirectory() as t:
            for n in os.listdir(here):
                if n.endswith(('.py', '.c')): shutil.copy(os.path.join(here, n), t)
            v1 = build_spawn._converter_version(t)
            with open(os.path.join(t, 'ui.py'), 'a') as f: f.write('\n# changed\n')
            self.assertNotEqual(build_spawn._converter_version(t), v1)
            os.makedirs(os.path.join(t, 'tests'))
            with open(os.path.join(t, 'tests', 'x.py'), 'w') as f: f.write('x')
            self.assertEqual(build_spawn._converter_version(t), build_spawn._converter_version(t))


if __name__ == '__main__':
    unittest.main()
