"""olkw rewriter: identity rewrite and appending, on small synthetic archives."""
import os, struct, sys, tempfile, unittest
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
import olk, olkw

ALIGN = 0x800


def _dir(entries, header):
    """One olnk directory: header, records, entry data. Record 0 = (count, 'olnk'), record 1 = total
    descriptor (as on disc: header size at 0x10, data size at 0x14), entries from record 2 on."""
    body = b''; recs = []
    for e in entries:
        recs.append((len(body), len(e), 0, 0))
        body += e + bytes((-len(e)) % ALIGN)
    cnt = len(recs)
    h = bytearray(header)
    struct.pack_into('<I4s', h, 0, cnt, b'olnk')
    struct.pack_into('<II', h, 0x10, header, len(body))
    for i, r in enumerate(recs):
        struct.pack_into('<4I', h, 0x20 + 16 * i, *r)
    return bytes(h) + body


def make_olk(groups, header=ALIGN):
    """Top-level archive of `groups` (each a list of entry bytes)."""
    return _dir([_dir(g, header) for g in groups], header)


GROUPS = [[b'top0' * 3], [b'a' * 100, b'', b'b' * 3000, b'c' * 7]]


class OlkwTest(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.TemporaryDirectory(); self.addCleanup(self.d.cleanup)
        self.src = os.path.join(self.d.name, 'src.olk'); self.out = os.path.join(self.d.name, 'out.olk')

    def put(self, data):
        with open(self.src, 'wb') as f: f.write(data)

    def test_identity_rewrite_is_byte_exact(self):
        self.put(make_olk(GROUPS))
        olkw.write(self.src, self.out, 1, {})
        self.assertEqual(open(self.out, 'rb').read(), open(self.src, 'rb').read())

    def test_append_keeps_existing_entries(self):
        self.put(make_olk(GROUPS))
        olkw.write(self.src, self.out, 1, {}, append=[b'A' * 5000, b'B' * 10])
        a, b = olk.Olk(self.src), olk.Olk(self.out)
        for g, ents in enumerate(GROUPS):
            for i in range(len(ents)):
                self.assertEqual(b.entry(g, i), a.entry(g, i), (g, i))
        n = len(GROUPS[1])
        self.assertEqual(b.entry(1, n), b'A' * 5000)
        self.assertEqual(b.entry(1, n + 1), b'B' * 10)
        self.assertEqual(len(b.group(1)[1]), n + 2)

    def test_append_over_capacity_grows_the_header(self):
        # 0x60-byte header: room for (0x60 - 0x20) / 16 = 4 records; group 1 has 4 already.
        self.put(make_olk(GROUPS, header=0x60))
        olkw.write(self.src, self.out, 1, {}, append=[b'x' * 10])
        a, b = olk.Olk(self.src), olk.Olk(self.out)
        self.assertEqual(b.entry(1, len(a.group(1)[1])), b'x' * 10)
        b.f.seek(b.top[0] + b.top[1][1][0]); hs = struct.unpack_from('<I', b.f.read(0x20), 0x10)[0]
        self.assertEqual(hs % 0x800, 0)
        for gi in range(len(a.top[1])):
            for i in range(len(a.group(gi)[1])):
                self.assertEqual(b.entry(gi, i), a.entry(gi, i), (gi, i))

    def test_append_within_capacity_keeps_the_header(self):
        self.put(make_olk(GROUPS, header=0x800))
        olkw.write(self.src, self.out, 1, {}, append=[b'x'])
        b = olk.Olk(self.out)
        b.f.seek(b.top[0] + b.top[1][1][0])
        self.assertEqual(struct.unpack_from('<I', b.f.read(0x20), 0x10)[0], 0x800)


if __name__ == '__main__':
    unittest.main()
