"""Heihachi's rows in the GC DOL's per-character tables, read from the player's PS2 executable.

The GC DOL leaves ID 28 empty in every table except his kerned name string and his weapon rows, which it
kept. The PS2 executable fills the same slots, in the same layouts, little-endian. His descriptor goes in
its own row; the strings it points at go in row 9 from +0x64 and his preview records in row 10's tail, both
empty rows and clear of pointer slots. Each table is found by the GC rows of the cast both discs share, and
must convert to them, or the executable is refused as unknown."""
import os, struct, sys
_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_HERE, '..', 'guests'))
import tables
from olk import Olk, pack

ID = 28
CELL = 9
STRINGS_ADDR, STRINGS_SIZE = tables.DESC_ADDR + 9 * tables.DESC_STRIDE + 0x64, 0x80
PREVIEW_HOME = tables.DESC_ADDR + 10 * tables.DESC_STRIDE + 0xC0
PS2_FACES, PS2_SILHOUETTES = (0x33, 0x34), (0x64, 0x65)
SWITCH_TABLE = 0x48B5C0   # the 504-510 switch, IDs 1-28 (GC 800834DC)
WEAPON_ADDR, WEAPON_STRIDE, WEAPON_ROWS = 0x802A81E0, 0x78, 33
LINK_CELL = 1


class Elf:
    """Loadable segments of a PS2 executable."""
    def __init__(self, view):
        self.d = view.read_all() if hasattr(view, 'read_all') else open(view, 'rb').read()
        d = self.d
        if d[:4] != b'\x7fELF': raise ValueError('not an ELF executable')
        ph, pn = struct.unpack_from('<I', d, 0x1c)[0], struct.unpack_from('<H', d, 0x2c)[0]
        size = struct.unpack_from('<H', d, 0x2a)[0]
        self.segs = []
        for k in range(pn):
            t, off, va, _, fs = struct.unpack_from('<5I', d, ph + k * size)
            if t == 1: self.segs.append((off, va, fs))

    def read(self, va, n):
        for off, a, fs in self.segs:
            if a <= va and va + n <= a + fs: return self.d[off + va - a:off + va - a + n]
        raise ValueError('0x%08X+%d is not in the executable' % (va, n))

    def va(self, off):
        for o, a, fs in self.segs:
            if o <= off < o + fs: return a + off - o
        raise ValueError('file offset 0x%X is in no segment' % off)

    def cstr(self, va):
        out = b''
        while True:
            c = self.read(va + len(out), 1)
            if c == b'\0': return out
            out += c

    def find_unique(self, pattern, what):
        i = self.d.find(pattern)
        if i < 0 or self.d.find(pattern, i + 1) >= 0:
            raise ValueError('%s: expected exactly one match in the executable' % what)
        return self.va(i)


def _unknown(what):
    return ValueError('%s does not convert to the GC one: unknown executable' % what)


def _strings(vals):
    blob, where = b'', {}
    for s in vals:
        if s in where: continue
        where[s] = STRINGS_ADDR + len(blob)
        blob += s + b'\0'
    if len(blob) > STRINGS_SIZE:
        raise ValueError('strings need %d bytes, the area holds %d' % (len(blob), STRINGS_SIZE))
    return blob, where


def _descriptor(e, g):
    gd = g.read(tables.DESC_ADDR + 22 * tables.DESC_STRIDE + tables.POINTER_AREA_END, tables.HALF_END - tables.POINTER_AREA_END)
    le = b''.join(gd[o:o + 2][::-1] for o in range(0, len(gd), 2))
    at = e.find_unique(le, 'descriptor of ID 22') - tables.POINTER_AREA_END - 22 * tables.DESC_STRIDE
    for cid in tables.SHARED_IDS:
        er = e.read(at + cid * tables.DESC_STRIDE, tables.DESC_STRIDE)
        gr = g.read(tables.DESC_ADDR + cid * tables.DESC_STRIDE, tables.DESC_STRIDE)
        ptrs = {struct.unpack_from('<I', er, o)[0]: struct.unpack_from('>I', gr, o)[0]
                for o in tables.POINTER_SLOTS if struct.unpack_from('<I', er, o)[0]}
        if tables.convert_descriptor(er, ptrs) != gr:
            raise _unknown('descriptor of ID %d' % cid)
    er = e.read(at + ID * tables.DESC_STRIDE, tables.DESC_STRIDE)
    ptrs = [struct.unpack_from('<I', er, o)[0] for o in tables.POINTER_SLOTS]
    blob, where = _strings([e.cstr(p) for p in ptrs if p])
    return tables.convert_descriptor(er, {p: where[e.cstr(p)] for p in ptrs if p}), blob


def _ending(e, g):
    gc = g.read(tables.ENDING_ADDR, tables.ENDING_STRIDE * 34)
    le = b''.join(struct.pack('<hhI', *struct.unpack_from('>hhI', gc, 8 * i)) for i in range(1, 27))
    at = e.find_unique(le, 'ending table IDs 1-26') - 8
    return tables.convert_ending_row(e.read(at + ID * 8, 8)), gc[ID * 8:ID * 8 + 8]


def _voice(e, g):
    gc = g.read(tables.VOICE_ADDR, 34)
    at = e.find_unique(gc[1:27], 'English-voice table IDs 1-26') - 1
    return e.read(at + ID, 1)[0]


def _preview(e, g):
    rows = [struct.unpack('>II', g.read(tables.PREVIEW_TABLE + 8 * i, 8)) for i in range(2)]
    rec = g.read(rows[1][1], tables.PREVIEW_REC * rows[1][0])
    le = b''.join(struct.pack('<I6f', *struct.unpack_from('>I6f', rec, k)) for k in range(0, len(rec), tables.PREVIEW_REC))
    first = e.find_unique(le, 'preview records of ID 1')
    at = e.find_unique(struct.pack('<II', rows[1][0], first), 'preview table row 1') - 8
    n, p = struct.unpack('<II', e.read(at + ID * 8, 8))
    if n != 2:
        raise ValueError('preview row 28 has %d records, expected 2: unknown executable' % n)
    return b''.join(tables.convert_preview_record(e.read(p + k * tables.PREVIEW_REC, tables.PREVIEW_REC)) for k in range(n))


def _cell(e):
    at = _cell_at(e)
    return e.d[at:at + tables.CELL_STRIDE]


def _cell_at(e):
    head, hits, i = struct.pack('<HH', ID, len(PS2_FACES)), [], e.d.find(struct.pack('<HH', ID, len(PS2_FACES)))
    while i >= 0:
        if e.d[i + 8:i + 10] == bytes(PS2_FACES) and e.d[i + 16:i + 18] == bytes(PS2_SILHOUETTES): hits.append(i)
        i = e.d.find(head, i + 1)
    if len(hits) != 1:
        raise ValueError('his PS2 select cell: %d matches, expected 1' % len(hits))
    return hits[0]


def ps2_cell(e, cid):
    """The PS2 select cell of a fighter: his own row, found from Heihachi's by the table's stride."""
    base = _cell_at(e)
    for k in range(-1, 40):
        at = base + k * tables.CELL_STRIDE
        if struct.unpack_from('<H', e.d, at)[0] == cid: return e.d[at:at + tables.CELL_STRIDE]
    raise ValueError('no PS2 cell for ID %d' % cid)


def _pool_lists(e, g):
    # Each list is found by itself: the PS2's are not one packed run like the Xbox's.
    found = []
    for l in tables.pool_lists(g.read(tables.POOL_ADDR, tables.POOL_SIZE)):
        if tables.LINK_ID not in l: continue
        pat = bytes(ID if v == tables.LINK_ID else v for v in l) + b'\xff'
        if e.d.find(pat) < 0:
            raise ValueError('pool list with him in Link\'s place is missing: unknown executable')
        found.append([ID if v == tables.LINK_ID else v for v in l])
    return found


def weapon_row_differences(e, g):
    gw = g.read(WEAPON_ADDR, WEAPON_STRIDE * WEAPON_ROWS)
    swap = lambda b: b''.join(b[o:o + 2][::-1] for o in range(0, len(b), 2))
    at = e.find_unique(swap(gw[WEAPON_STRIDE:WEAPON_STRIDE * 7]), 'weapon rows 1-6') - WEAPON_STRIDE
    return [i for i in range(WEAPON_ROWS)
            if swap(e.read(at + i * WEAPON_STRIDE, WEAPON_STRIDE)) != gw[i * WEAPON_STRIDE:(i + 1) * WEAPON_STRIDE]]


def switch_as_id(e):
    """The GC ID whose 504-510 case is his. PS2's table runs IDs 1-28, each a `jr ra; li v0, N` pair."""
    t = struct.unpack('<28I', e.read(SWITCH_TABLE, 4 * 28))
    val = lambda a: struct.unpack('<I', e.read(a + 4, 4))[0] & 0xffff
    his = val(t[ID - 1])
    if not 504 <= his <= 510 or val(t[24 - 1]) != his:
        raise ValueError('his 504-510 case is %d, not the one of ID 24: unknown executable' % his)
    return 24


def home_scene(root_view):
    """PS2 record -> GC record of his home-stage scene, checked by the ID in the first motion record."""
    p = Olk(root_view)
    out = {}
    for r in (18, 34):
        q = pack(p.entry(4, r), False)
        if struct.unpack_from('<H', q[1], 0x10)[0] != ID:
            raise ValueError('group 4 record %d is not his scene: unknown disc' % r)
        out[r] = r
    return out


def heihachi_tables(elf_view, dol):
    e, g = Elf(elf_view), tables.Dol(dol)
    desc, blob = _descriptor(e, g)
    costume_at = e.find_unique(g.read(tables.COSTUME_ADDR + 6, 6 * 26), 'costume table IDs 1-26') - 6
    roster_at = e.find_unique(g.read(tables.ROSTER_ADDR, 27), 'roster table IDs 0-26')
    ending, ending_old = _ending(e, g)
    link = g.read(tables.CELL_ADDR + LINK_CELL * tables.CELL_STRIDE, tables.CELL_STRIDE)
    if link[:2] != b'\x00\x1b':
        raise ValueError('cell %d is not Link\'s: unknown executable' % LINK_CELL)
    return {'descriptor': desc, 'strings': blob, 'costume': e.read(costume_at + ID * 6, 6),
            'roster': e.read(roster_at + ID, 1)[0], 'ending': ending, 'ending_old': ending_old,
            'voice': _voice(e, g), 'preview': _preview(e, g), 'cell': struct.pack('>H', ID) + link[2:],
            'xcell': _cell(e), 'pool_lists': _pool_lists(e, g)}
