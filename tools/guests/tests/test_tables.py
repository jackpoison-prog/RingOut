"""tables: Spawn's per-character rows, read from the player's Xbox executable and converted for the GC
DOL's tables (MAP.md, "Per-character tables").

The oracle runs on the discs (RINGOUT_SPAWN_XBE = the Xbox Default.xbe, RINGOUT_SPAWN_DOL = the GC US
main.dol): the converter must turn every character's Xbox descriptor into the GC one, pointers aside.
The synthetic tests run anywhere."""
import os, struct, sys, unittest
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
import tables

XBE = os.environ.get('RINGOUT_SPAWN_XBE', '')
DOL = os.environ.get('RINGOUT_SPAWN_DOL', '')
HAVE = os.path.isfile(XBE) and os.path.isfile(DOL)


class ConvertSynthetic(unittest.TestCase):
    def row(self):
        r = bytearray(tables.DESC_STRIDE)
        struct.pack_into('<I', r, 0x00, 0x17CBF4)       # a string pointer
        struct.pack_into('<H', r, 0x78, 0x1234)         # a 16-bit field
        struct.pack_into('<f', r, 0xA8, 1.0)            # a float
        r[0xB8] = 7                                     # a byte field
        return bytes(r)

    def test_fields_swap_by_type(self):
        g = tables.convert_descriptor(self.row(), {0x17CBF4: 0x80277640})
        self.assertEqual(struct.unpack_from('>I', g, 0x00)[0], 0x80277640)
        self.assertEqual(struct.unpack_from('>H', g, 0x78)[0], 0x1234)
        self.assertEqual(struct.unpack_from('>f', g, 0xA8)[0], 1.0)
        self.assertEqual(g[0xB8], 7)
        self.assertEqual(len(g), tables.DESC_STRIDE)

    def test_unmapped_pointer_refused(self):
        with self.assertRaises(ValueError):
            tables.convert_descriptor(self.row(), {})

    def test_preview_record_swaps_to_big_endian(self):
        rec = struct.pack('<I6f', 3, 0.5, -1.0, 0.25, 1.5, -0.5, 2.0)
        self.assertEqual(tables.convert_preview_record(rec), struct.pack('>I6f', 3, 0.5, -1.0, 0.25, 1.5, -0.5, 2.0))
        with self.assertRaises(ValueError):
            tables.convert_preview_record(rec[:27])

    def test_stray_byte_in_pointer_area_refused(self):
        r = bytearray(self.row()); r[0x10] = 1
        with self.assertRaises(ValueError):
            tables.convert_descriptor(bytes(r), {0x17CBF4: 0x80277640})

    def test_string_area_layout(self):
        blob, where = tables.pack_strings([b'Spawn', b'SPAWN', b'Spawn'])
        self.assertEqual(blob, b'Spawn\0SPAWN\0')
        self.assertEqual(where, {b'Spawn': tables.STRINGS_ADDR, b'SPAWN': tables.STRINGS_ADDR + 6})

    def test_string_area_overflow_refused(self):
        with self.assertRaises(ValueError):
            tables.pack_strings([b'x' * tables.STRINGS_SIZE])

    def test_cell_with_faces_sets_count_and_portraits(self):
        cell = bytes(range(36))
        out = tables.cell_with_faces(cell, (77, 78))
        self.assertEqual(out[:2], cell[:2])
        self.assertEqual(out[2:4], b'\x00\x02')                 # one costume per face
        self.assertEqual(out[4:8], cell[4:8])
        self.assertEqual(out[8:16], bytes([77, 78, 0, 0, 0, 0, 0, 0]))
        self.assertEqual(out[16:], cell[16:])
        with self.assertRaises(ValueError): tables.cell_with_faces(cell, tuple(range(9)))

    def test_cell_with_silhouettes_sets_figures_and_xbox_tail(self):
        import struct
        cell = bytes(range(36))
        xcell = bytes(28) + struct.pack('<fHH', -0.29, 1, 4)    # his Xbox cell's +28..+35
        out = tables.cell_with_silhouettes(cell, (150, 151), xcell)
        self.assertEqual(out[:16], cell[:16])
        self.assertEqual(out[16:24], bytes([150, 151, 0, 0, 0, 0, 0, 0]))
        self.assertEqual(out[24:28], cell[24:28])
        self.assertEqual(out[28:], struct.pack('>fHH', -0.29, 1, 4))
        with self.assertRaises(ValueError): tables.cell_with_silhouettes(cell, (), xcell)


class PoolsSynthetic(unittest.TestCase):
    """Character pools: 255-terminated ID lists packed in one block, reached through a pointer table."""
    BASE = 0x80001000

    def block(self, lists, size):
        out = bytearray()
        for l in lists:
            out += bytes(l) + b'\xff'
            out += bytes(-len(out) % 4)          # the shipped DOL pads each list to 4
        return bytes(out) + bytes(size - len(out))

    def table(self, addrs):
        return b''.join(struct.pack('>I', a) for a in addrs)

    def test_spawn_follows_link_where_the_xbox_has_him(self):
        gc = self.block([[1, 27, 3], [4, 5], [2, 27]], 32)
        xb = [[1, 29, 3], [4, 5], [2, 6]]
        b, t = tables.repack_pools(gc, self.BASE, self.table([self.BASE, self.BASE + 4, 0x80460000, self.BASE + 8]), {29: xb})
        self.assertEqual(b, bytes([1, 27, 29, 3, 255, 4, 5, 255, 2, 27, 255]) + bytes(21))
        self.assertEqual(t, self.table([self.BASE, self.BASE + 5, 0x80460000, self.BASE + 8]))

    def test_list_without_his_xbox_counterpart_is_unchanged(self):
        gc = self.block([[2, 27]], 8)
        b, _ = tables.repack_pools(gc, self.BASE, self.table([self.BASE]), {29: [[2, 6]]})
        self.assertEqual(b, bytes([2, 27, 255]) + bytes(5))

    def test_no_room_refused(self):
        full = bytes([1, 27, 255, 1, 27, 255, 1, 27, 255])
        with self.assertRaises(ValueError):
            tables.repack_pools(full, self.BASE, self.table([self.BASE]), {29: [[1, 29]]})

    def test_pointer_into_a_list_refused(self):
        gc = self.block([[1, 27, 3]], 8)
        with self.assertRaises(ValueError):
            tables.repack_pools(gc, self.BASE, self.table([self.BASE + 1]), {29: [[1, 29, 3]]})


class EndingSynthetic(unittest.TestCase):
    def test_row_converts_halves_and_the_text_word(self):
        # Xbox row: s16 illustration entry, s16 page count, u32 text index, little-endian
        self.assertEqual(tables.convert_ending_row(struct.pack('<hhI', 86, 11, 85)),
                         struct.pack('>hhI', 86, 11, 85))

    def test_short_row_refused(self):
        with self.assertRaises(ValueError):
            tables.convert_ending_row(b'\x00' * 6)


@unittest.skipUnless(HAVE, 'needs RINGOUT_SPAWN_XBE and RINGOUT_SPAWN_DOL')
class FromTheDiscs(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.x, cls.g = tables.Xbe(XBE), tables.Dol(DOL)
        cls.loc = tables.locate(cls.x, cls.g)
        cls.t = tables.spawn_tables(XBE, DOL)

    def test_converter_reproduces_every_gc_row(self):
        for cid in tables.SHARED_IDS:
            xr = self.x.read(self.loc['desc'] + cid * tables.DESC_STRIDE, tables.DESC_STRIDE)
            gr = self.g.read(tables.DESC_ADDR + cid * tables.DESC_STRIDE, tables.DESC_STRIDE)
            ptrs = {struct.unpack_from('<I', xr, o)[0]: struct.unpack_from('>I', gr, o)[0]
                    for o in tables.POINTER_SLOTS if struct.unpack_from('<I', xr, o)[0]}
            self.assertEqual(tables.convert_descriptor(xr, ptrs), gr, cid)

    def test_spawn_strings(self):
        d, blob = self.t['descriptor'], self.t['strings']
        def s(slot):
            p = struct.unpack_from('>I', d, slot)[0] - tables.STRINGS_ADDR
            return blob[p:blob.index(b'\0', p)]
        self.assertEqual((s(0x00), s(0x18), s(0x30), s(0x48), s(0x60)),
                         (b'Spawn', b'SPAWN', b'Agony', b'Detroit/Michigan', b'31'))

    def test_spawn_ids_are_his(self):
        for o in (0x82, 0x86, 0x88):
            self.assertEqual(struct.unpack_from('>H', self.t['descriptor'], o)[0], 29, hex(o))

    def test_costume_row_and_roster(self):
        self.assertEqual(self.t['costume'], bytes([1, 1, 1, 1, 0, 0]))
        self.assertEqual(self.t['roster'], 1)

    def test_gc_slots_are_free(self):
        self.assertEqual(self.g.read(tables.DESC_ADDR + 29 * tables.DESC_STRIDE, tables.DESC_STRIDE),
                         bytes(tables.DESC_STRIDE))
        self.assertEqual(self.g.read(tables.STRINGS_ADDR, tables.STRINGS_SIZE), bytes(tables.STRINGS_SIZE))
        self.assertEqual(self.g.read(tables.COSTUME_ADDR + 29 * 6, 6), bytes(6))
        self.assertEqual(self.g.read(tables.BLOCK_ADDR + 29 * 2, 2), b'\xff\xff')
        self.assertEqual(self.g.read(tables.ROSTER_ADDR + 29, 1), b'\x13')
        self.assertEqual(self.g.read(tables.CELL_ADDR + 4 * tables.CELL_STRIDE, tables.CELL_STRIDE),
                         b'\xff\xff' + bytes(tables.CELL_STRIDE - 2))

    def test_cell_is_links_record_as_29(self):
        link = self.g.read(tables.CELL_ADDR + 1 * tables.CELL_STRIDE, tables.CELL_STRIDE)
        self.assertEqual(link[:2], b'\x00\x1b')          # cell 1 is Link's (top row, second column)
        self.assertEqual(self.t['cell'], b'\x00\x1d' + link[2:])

    def test_pools_add_him_beside_link(self):
        old = self.g.read(tables.POOL_ADDR, tables.POOL_SIZE)
        pool = tables.pools(self.g, {29: self.t['pool_lists']})
        new = pool['pool_block']
        lo, ln = tables.pool_lists(old), tables.pool_lists(new)
        self.assertEqual(len(lo), len(ln))
        changed = [(a, b) for a, b in zip(lo, ln) if a != b]
        self.assertEqual(len(changed), 7)                 # the seven Xbox lists that carry 29
        for a, b in changed:
            i = a.index(27)
            self.assertEqual(b, a[:i + 1] + [29] + a[i + 1:])
        self.assertEqual(pool['pool_block_old'], old)
        told = self.g.read(tables.POOL_TABLE, tables.POOL_TABLE_SIZE)
        self.assertEqual(pool['pool_table_old'], told)
        for k in range(tables.POOL_TABLE_SIZE // 4):     # every entry still names the same list
            po = struct.unpack_from('>I', told, 4 * k)[0]
            pn = struct.unpack_from('>I', pool['pool_table'], 4 * k)[0]
            if tables.POOL_ADDR <= po < tables.POOL_ADDR + tables.POOL_SIZE:
                was = tables.pool_lists(old[po - tables.POOL_ADDR:])[0]
                now = tables.pool_lists(new[pn - tables.POOL_ADDR:])[0]
                self.assertEqual([v for v in now if v != 29], was)
            else:
                self.assertEqual(pn, po)

    def test_ending_row_is_his_xbox_row(self):
        # Without it the ending sizes group-3 entry -1 and the next buffer lands past RAM (MAP.md, Ending)
        self.assertEqual(self.t['ending_old'], b'\xff' * 8)
        self.assertEqual(self.g.read(tables.ENDING_ADDR + 29 * 8, 8), self.t['ending_old'])
        self.assertEqual(self.t['ending'], struct.pack('>hhI', 86, 11, 85))
        self.assertEqual(self.g.read(tables.ENDING_ADDR + 22 * 8, 8), struct.pack('>hhI', 79, 10, 57))

    def test_english_voice_flag_is_his_xbox_flag(self):
        # 80098A04 adds 4 to a voice slot (30-32 -> 34-36, English) only when this byte is set
        self.assertEqual(self.g.read(tables.VOICE_ADDR + 29, 1), b'\x00')
        self.assertEqual(self.t['voice'], 1)
        self.assertEqual(self.g.read(tables.VOICE_ADDR + 22, 1), b'\x01')   # Talim has English
        self.assertEqual(self.g.read(tables.VOICE_ADDR + 27, 1), b'\x00')   # Link has none


    def test_name_string_is_his_xbox_string(self):
        # 0x8028E9D8: a pointer per ID to the kerned name the VS screen and HUD draw. GC 29 is the dummy
        # Q.Takagi; his Xbox string lives in an empty descriptor row, and the strings of 28 and 29 stay.
        ptr = lambda cid: struct.unpack('>I', self.g.read(tables.NAME_TABLE_ADDR + 4 * cid, 4))[0]
        self.assertEqual(self.g.read(ptr(29), 18), b'Q.$l4T$l9AKA$l4GI\x00')
        self.assertEqual(ptr(28), tables.OLD_NAME_28)
        self.assertEqual(self.t['name'][:22], b'S$l4P$l12A$l6$d1W$u1N\x00')
        self.assertEqual(len(self.t['name']), 36)
        self.assertEqual(self.g.read(tables.NAME_AT, 36), bytes(36))     # an empty row, not 28's string
        self.assertEqual(self.g.read(tables.OLD_NAME_28, 18), b'HE$l2IHA$l6C$l1HI\x00')
        self.assertNotIn('name_old', self.t)

    def test_spawn_data_clear_of_id28(self):
        row28 = range(tables.DESC_ADDR + 28 * tables.DESC_STRIDE, tables.DESC_ADDR + 29 * tables.DESC_STRIDE)
        names = range(tables.OLD_NAME_28, tables.OLD_NAME_28 + 36)
        for a, n in ((tables.STRINGS_ADDR, tables.STRINGS_SIZE), (tables.PREVIEW_HOME, tables.PREVIEW_REC),
                     (tables.NAME_AT, tables.NAME_ROOM)):
            self.assertEqual(self.g.read(a, n), bytes(n), hex(a))
            for r in (row28, names):
                self.assertFalse(a in r or a + n - 1 in r, hex(a))

    def test_preview_pose_is_his_xbox_record(self):
        # 0x80251904: per ID {u32 count, u32 pointer} to 28-byte bone records (u32 parent, xyz, rotation)
        # that pose the weapon on the weapon-select screen (8017E998). GC ships count 0 for 29, so his
        # preview kept its parked matrices (y = 100) and Agony never showed (user report 2026-11-04).
        self.assertEqual(self.g.read(tables.PREVIEW_TABLE + 29 * 8, 8), bytes(8))
        rec = struct.unpack('>I6f', self.t['preview'])
        self.assertEqual(rec[0], 0)
        self.assertAlmostEqual(rec[1], -0.1711, places=3)
        self.assertAlmostEqual(rec[6], 0.1604, places=3)
        self.assertEqual(self.g.read(tables.PREVIEW_HOME, 28), bytes(28))   # empty row 7

if __name__ == '__main__':
    unittest.main()
