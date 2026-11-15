"""spawn-id-scan.py: per-character code sites in a GC DOL (compares against 27/28/29 and ID-indexed
jump tables near a load of a character-ID field). Synthetic DOLs, so it runs anywhere."""
import importlib.util, os, struct, tempfile, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
SPEC = importlib.util.spec_from_file_location(
    'idscan', os.path.join(HERE, '..', '..', '..', '.github', 'scripts', 'spawn-id-scan.py'))
idscan = importlib.util.module_from_spec(SPEC); SPEC.loader.exec_module(idscan)

BASE = 0x80003100
NOP = 0x60000000


def lha(rd, d, ra): return (42 << 26) | (rd << 21) | (ra << 16) | (d & 0xffff)
def cmpwi(ra, v): return (11 << 26) | (ra << 16) | (v & 0xffff)
def cmplwi(ra, v): return (10 << 26) | (ra << 16) | (v & 0xffff)
def bgt(off): return (16 << 26) | (12 << 21) | (1 << 16) | (off & 0xfffc)
BCTR = 0x4E800420


def dol(words):
    text = b''.join(struct.pack('>I', w) for w in words)
    hdr = bytearray(0x100)
    struct.pack_into('>I', hdr, 0x00, 0x100)          # text0 file offset
    struct.pack_into('>I', hdr, 0x48, BASE)           # text0 address
    struct.pack_into('>I', hdr, 0x90, len(text))      # text0 size
    f = tempfile.NamedTemporaryFile(suffix='.dol', delete=False)
    f.write(bytes(hdr) + text); f.close()
    return f.name


class Scan(unittest.TestCase):
    def scan(self, words, **kw):
        p = dol(words); self.addCleanup(os.remove, p)
        return idscan.scan(p, **kw)

    def test_compare_after_id_load_reported(self):
        sites = self.scan([lha(3, 0x5C, 31), NOP, cmpwi(3, 27)])
        self.assertEqual([(s.pc, s.kind, s.value) for s in sites], [(BASE + 8, 'cmp', 27)])

    def test_compare_far_from_any_id_load_ignored(self):
        self.assertEqual(self.scan([lha(3, 0x5C, 31)] + [NOP] * 40 + [cmpwi(3, 27)]), [])

    def test_compare_on_other_value_ignored(self):
        self.assertEqual(self.scan([lha(3, 0x5C, 31), cmpwi(3, 26)]), [])

    def test_all_flag_reports_unanchored_compares(self):
        sites = self.scan([cmpwi(4, 29)], all_sites=True)
        self.assertEqual([(s.kind, s.value) for s in sites], [('cmp', 29)])

    def test_jump_table_sized_by_its_bound(self):
        sites = self.scan([lha(0, 0x5A, 3), cmplwi(0, 29), bgt(0x40), NOP, NOP, BCTR])
        self.assertEqual([(s.pc, s.kind, s.value) for s in sites], [(BASE + 4, 'switch', 30)])

    def test_bound_without_bctr_is_a_plain_compare(self):
        sites = self.scan([lha(0, 0x5A, 3), cmplwi(0, 28), bgt(0x40)])
        self.assertEqual([(s.kind, s.value) for s in sites], [('cmp', 28)])


if __name__ == '__main__':
    unittest.main()
