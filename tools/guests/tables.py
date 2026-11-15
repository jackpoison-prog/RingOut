"""Spawn's rows in the GC DOL's per-character tables, read from the player's Xbox Default.xbe.

The US GC executable has a slot for character 29 in every per-ID table but leaves it empty; the Xbox
executable fills the same slot with Spawn, in the same layout. The rows are converted here, at build
time, from the player's own disc, and the runtime only writes them into the fixed slots below (after
checking those still hold what this DOL ships). Addresses and the evidence for each: MAP.md,
"Per-character tables".

    descriptor  0x80275A60 + id*0xF8   names, weapon, origin, age, stats, model IDs (0x82/0x86/0x88)
    costume     0x80279484 + id*6      one byte per costume: selectable (the validity check 800A8E04)
    roster      0x80250958 + id        guest-slot index (Link 27 -> 1 on GC; Spawn 29 -> 1 on Xbox)
    block       0x80277A58 + id*2      first root.olk group-4 record of the character's block
    strings     0x802761E8, 56 bytes   the unused tail of empty descriptor row 7; holds his names
    pools       0x8028B50C, 248 bytes  255-terminated character lists (opponent pools), reached through
                0x8028B604, 43 words   a pointer table; repacked with 29 after 27 where the Xbox has 29
"""
import os, struct

DESC_ADDR, DESC_STRIDE = 0x80275A60, 0xF8
COSTUME_ADDR, ROSTER_ADDR, BLOCK_ADDR = 0x80279484, 0x80250958, 0x80277A58
# Spawn's extra data lives in empty descriptor rows 7 and 8, in bytes that are not pointer slots. Rows 28
# and the shipped name strings of 28 and 29 stay untouched: ID 28 is Heihachi's.
SPAWN_HOME = DESC_ADDR + 7 * DESC_STRIDE
STRINGS_ADDR, STRINGS_SIZE = SPAWN_HOME + 0xC0, 0x38
SPAWN_ID = 29
# Select grid (MAP.md, Task 8): 36-byte cell records, rows of 4 + a -1 end marker. Cell 1 is Link's;
# his is cell 4, the free end of Link's row. His record is Link's with the ID changed.
CELL_ADDR, CELL_STRIDE, LINK_CELL, SPAWN_CELL = 0x80250520, 36, 1, 4
# Character pools (MAP.md, "Character pools"): 255-terminated ID lists, each padded to 4 bytes, packed in
# one block and reached through a table of 43 pointers. GC lists carry Link (27) exactly where the
# Xbox's carry Spawn (29); the block repacks unpadded with room for him beside Link.
POOL_ADDR, POOL_SIZE, POOL_TABLE, POOL_TABLE_SIZE = 0x8028B50C, 0xF8, 0x8028B604, 43 * 4
LINK_ID = 27

# Five {english, japanese} string pairs every 0x18 bytes, then a lone one (age); all else below 0x78 is 0.
POINTER_SLOTS = (0x00, 0x04, 0x18, 0x1C, 0x30, 0x34, 0x48, 0x4C, 0x60)
POINTER_AREA_END, HALF_END, WORD_END = 0x78, 0xA8, 0xB8    # 16-bit fields, then floats, then bytes

# IDs with a descriptor on both discs: the converter oracle. 27 (Link) is GC-only, 29 (Spawn) Xbox-only.
SHARED_IDS = (1, 2, 3, 4, 5, 6, 11, 12, 13, 15, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 30, 31, 32)


class Dol:
    def __init__(self, src):
        """`src`: a path, or an xiso.FileView into a disc image."""
        if isinstance(src, (str, bytes, os.PathLike)):
            with open(src, 'rb') as f: d = f.read()
        else:
            d = src.read_all()
        self.d = d
        self.secs = list(zip(struct.unpack_from('>18I', d, 0), struct.unpack_from('>18I', d, 0x48),
                             struct.unpack_from('>18I', d, 0x90)))

    def read(self, va, n):
        for off, addr, size in self.secs:
            if size and addr <= va and va + n <= addr + size:
                return self.d[off + va - addr:off + va - addr + n]
        raise ValueError('0x%08X+%d is not in the DOL' % (va, n))


class Xbe:
    def __init__(self, src):
        """`src`: a path, or an xiso.FileView into a disc image."""
        if isinstance(src, (str, bytes, os.PathLike)):
            with open(src, 'rb') as f: d = f.read()
        else:
            d = src.read_all()
        self.d = d
        base, = struct.unpack_from('<I', d, 0x104)
        n, sh = struct.unpack_from('<II', d, 0x11C)
        self.secs = [struct.unpack_from('<5I', d, sh - base + k * 0x38)[1:] for k in range(n)]  # va, vsize, raw, rawsize

    def read(self, va, n):
        for v, _, raw, size in self.secs:
            if v <= va and va + n <= v + size:
                return self.d[raw + va - v:raw + va - v + n]
        raise ValueError('0x%08X+%d is not in the XBE' % (va, n))

    def va(self, file_off):
        for v, _, raw, size in self.secs:
            if raw <= file_off < raw + size:
                return v + file_off - raw
        raise ValueError('file offset 0x%X is in no section' % file_off)

    def cstr(self, va):
        out = b''
        while True:
            c = self.read(va + len(out), 1)
            if c == b'\0': return out
            out += c

    def find_unique(self, pattern, what):
        i = self.d.find(pattern)
        if i < 0 or self.d.find(pattern, i + 1) >= 0:
            raise ValueError('%s: expected exactly one match in the XBE' % what)
        return self.va(i)


def convert_descriptor(xrow, pointers):
    """One Xbox descriptor row -> the GC row. `pointers` maps each Xbox string address it holds to the
    GC address that string will have."""
    if len(xrow) != DESC_STRIDE:
        raise ValueError('descriptor row is %d bytes, expected %d' % (len(xrow), DESC_STRIDE))
    g = bytearray(DESC_STRIDE)
    for o in range(0, POINTER_AREA_END, 4):
        v, = struct.unpack_from('<I', xrow, o)
        if o in POINTER_SLOTS:
            if v:
                if v not in pointers: raise ValueError('no GC address for string 0x%X (slot 0x%X)' % (v, o))
                struct.pack_into('>I', g, o, pointers[v])
        elif v:
            raise ValueError('unexpected data at 0x%X in the pointer area' % o)
    for o in range(POINTER_AREA_END, HALF_END, 2):
        g[o:o + 2] = xrow[o:o + 2][::-1]
    for o in range(HALF_END, WORD_END, 4):
        g[o:o + 4] = xrow[o:o + 4][::-1]
    g[WORD_END:] = xrow[WORD_END:]
    return bytes(g)


def pack_strings(strings):
    """NUL-terminated strings for the GC string area, each stored once -> (blob, {string: GC address})."""
    blob, where = b'', {}
    for s in strings:
        if s in where: continue
        where[s] = STRINGS_ADDR + len(blob)
        blob += s + b'\0'
    if len(blob) > STRINGS_SIZE:
        raise ValueError('strings need %d bytes, the area holds %d' % (len(blob), STRINGS_SIZE))
    return blob, where


def locate(x, g):
    """The XBE addresses of the descriptor, costume and roster tables, found by their GC contents."""
    gd = g.read(DESC_ADDR + 22 * DESC_STRIDE + POINTER_AREA_END, HALF_END - POINTER_AREA_END)
    le = b''.join(gd[o:o + 2][::-1] for o in range(0, len(gd), 2))
    desc = x.find_unique(le, 'descriptor of ID 22') - POINTER_AREA_END - 22 * DESC_STRIDE
    costume = x.find_unique(g.read(COSTUME_ADDR + 6, 6 * 26), 'costume table IDs 1-26') - 6
    roster = x.find_unique(g.read(ROSTER_ADDR, 27), 'roster table IDs 0-26')
    return {'desc': desc, 'costume': costume, 'roster': roster}


XB_FACES = (51, 52)   # = ui.XB_SPAWN_FACES: his Xbox face indices, which identify his Xbox cell


def spawn_tables(xbe_path, dol_path, faces=XB_FACES):
    """Spawn's converted rows: {'descriptor': 248 bytes, 'strings': blob, 'costume': 6 bytes, 'roster': int,
    'cell': 36 bytes, 'pool_block'/'pool_table', 'ending' (8 bytes) and their '_old' shipped bytes}."""
    x, g = Xbe(xbe_path), Dol(dol_path)
    loc = locate(x, g)
    for cid in SHARED_IDS:                       # the layout must hold for every character both discs share
        xr = x.read(loc['desc'] + cid * DESC_STRIDE, DESC_STRIDE)
        gr = g.read(DESC_ADDR + cid * DESC_STRIDE, DESC_STRIDE)
        ptrs = {struct.unpack_from('<I', xr, o)[0]: struct.unpack_from('>I', gr, o)[0]
                for o in POINTER_SLOTS if struct.unpack_from('<I', xr, o)[0]}
        if convert_descriptor(xr, ptrs) != gr:
            raise ValueError('descriptor of ID %d does not convert to the GC one: unknown executable' % cid)
    xr = x.read(loc['desc'] + SPAWN_ID * DESC_STRIDE, DESC_STRIDE)
    xptrs = [struct.unpack_from('<I', xr, o)[0] for o in POINTER_SLOTS]
    blob, where = pack_strings([x.cstr(p) for p in xptrs if p])
    desc = convert_descriptor(xr, {p: where[x.cstr(p)] for p in xptrs if p})
    link = g.read(CELL_ADDR + LINK_CELL * CELL_STRIDE, CELL_STRIDE)
    if link[:2] != b'\x00\x1b':
        raise ValueError('cell %d is not Link\'s: unknown executable' % LINK_CELL)
    return {'descriptor': desc, 'strings': blob, 'cell': struct.pack('>H', SPAWN_ID) + link[2:],
            'costume': x.read(loc['costume'] + SPAWN_ID * 6, 6),
            'roster': x.read(loc['roster'] + SPAWN_ID, 1)[0], **spawn_pools(x, g), **spawn_ending(x, g),
            **spawn_voice(x, g), **spawn_name(x, g), **spawn_preview(x, g), 'xcell': xbox_cell(x, faces)}


ENDING_ADDR, ENDING_STRIDE = 0x802A3748, 8   # per ID: s16 illustration entry, s16 pages, u32 text index


def convert_ending_row(xrow):
    """One ending row, Xbox (little-endian) to GC (big-endian)."""
    if len(xrow) != ENDING_STRIDE:
        raise ValueError('an ending row is %d bytes, got %d' % (ENDING_STRIDE, len(xrow)))
    return struct.pack('>hhI', *struct.unpack('<hhI', xrow))


def spawn_ending(x, g):
    """His ending row and the GC bytes it replaces. The XBE table is found by the GC rows of IDs 1-26
    and must convert to GC's for every shared ID. Without his row the ending asks for the size of
    group-3 entry -1, gets the whole group's, and places the next buffer past the end of RAM."""
    gc = g.read(ENDING_ADDR, ENDING_STRIDE * 34)
    le = b''.join(struct.pack('<hhI', *struct.unpack_from('>hhI', gc, ENDING_STRIDE * i)) for i in range(1, 27))
    xa = x.find_unique(le, 'ending table IDs 1-26') - ENDING_STRIDE
    for cid in SHARED_IDS:
        if convert_ending_row(x.read(xa + cid * ENDING_STRIDE, ENDING_STRIDE)) != gc[cid * 8:cid * 8 + 8]:
            raise ValueError('ending row of ID %d does not convert to the GC one: unknown executable' % cid)
    return {'ending': convert_ending_row(x.read(xa + SPAWN_ID * ENDING_STRIDE, ENDING_STRIDE)),
            'ending_old': gc[SPAWN_ID * 8:SPAWN_ID * 8 + 8]}


VOICE_ADDR = 0x80277A9C   # u8 per ID: 1 = has English voice slots (34-36 beside the Japanese 30-32)


def spawn_voice(x, g):
    """His English-voice flag from the XBE. The table is found by the GC bytes of IDs 1-26 and must
    match for every shared ID. 80098A04 moves a voice slot from 30-32 to 34-36 when the game's voice
    language is English and this byte is set; GC ships 0 for 29 (the dummy Q.Takagi), so he spoke
    Japanese while everyone else spoke English."""
    gc = g.read(VOICE_ADDR, 34)
    xa = x.find_unique(gc[1:27], 'English-voice table IDs 1-26') - 1
    xt = x.read(xa, 34)
    for cid in SHARED_IDS:
        if xt[cid] != gc[cid]:
            raise ValueError('English-voice flag of ID %d differs from the GC one: unknown executable' % cid)
    return {'voice': xt[SPAWN_ID]}


NAME_TABLE_ADDR = 0x8028E9D8   # u32 per ID: the kerned name string the VS screen and HUD draw
OLD_NAME_28 = 0x8028ED6A       # the shipped strings of IDs 28 (Heihachi) and 29 (Q.Takagi), left as they are
NAME_AT = DESC_ADDR + 8 * DESC_STRIDE + 0xC0   # his string, in empty descriptor row 8
NAME_ROOM = 36


def spawn_name(x, g):
    """His kerned name string from the XBE, padded to NAME_ROOM for its place in row 8. The XBE table is found by its pointers to the strings of IDs 1-6 and must name every shared ID as GC
    does. GC 29 points at the dummy's 'Q.TAKAGI', the name his VS screen showed."""
    gp = struct.unpack('>34I', g.read(NAME_TABLE_ADDR, 4 * 34))
    if gp[28] != OLD_NAME_28 or gp[29] != OLD_NAME_28 + 18 or g.read(NAME_AT, NAME_ROOM) != bytes(NAME_ROOM):
        raise ValueError('the GC name strings of IDs 28 and 29 are not where expected: unknown executable')
    gstr = lambda cid: g.read(gp[cid], 64).split(b'\0')[0]
    xptr = [x.find_unique(b'\0' + gstr(cid) + b'\0', 'name string of ID %d' % cid) + 1 for cid in range(1, 7)]
    xa = x.find_unique(struct.pack('<6I', *xptr), 'name table IDs 1-6') - 4
    xp = struct.unpack('<34I', x.read(xa, 4 * 34))
    for cid in SHARED_IDS:
        if x.cstr(xp[cid]) != gstr(cid):
            raise ValueError('name string of ID %d differs from the GC one: unknown executable' % cid)
    his = x.cstr(xp[SPAWN_ID]) + b'\0'
    if len(his) > NAME_ROOM:
        raise ValueError('his name string (%d bytes) does not fit in %d' % (len(his), NAME_ROOM))
    return {'name': his.ljust(NAME_ROOM, b'\0')}

PREVIEW_TABLE = 0x80251904   # per ID {u32 count, u32 pointer} to the weapon-select preview's bone records
PREVIEW_REC = 28              # u32 parent bone, float xyz, float rotation xyz (read by 8017E998)
PREVIEW_HOME = SPAWN_HOME + 0x64   # 28 unused bytes of empty descriptor row 7


def convert_preview_record(xrec):
    """One preview bone record, Xbox (little-endian) to GC (big-endian)."""
    if len(xrec) != PREVIEW_REC:
        raise ValueError('a preview record is %d bytes, got %d' % (PREVIEW_REC, len(xrec)))
    return struct.pack('>I6f', *struct.unpack('<I6f', xrec))


def spawn_preview(x, g):
    """His weapon-select preview pose from the XBE. GC ships count 0 for 29: the preview then keeps the
    matrices its setup parks at y = 100 (800A3D98) and his weapon never shows. The XBE table is found by
    the records of ID 1 and must hold every shared ID's records as GC does."""
    rows = [struct.unpack('>II', g.read(PREVIEW_TABLE + 8 * i, 8)) for i in range(SPAWN_ID + 1)]
    grecs = lambda cid: g.read(rows[cid][1], PREVIEW_REC * rows[cid][0]) if rows[cid][0] else b''
    le = lambda b: b''.join(struct.pack('<I6f', *struct.unpack_from('>I6f', b, k)) for k in range(0, len(b), PREVIEW_REC))
    first = x.find_unique(le(grecs(1)), 'preview records of ID 1')
    xa = x.find_unique(struct.pack('<II', rows[1][0], first), 'preview table row 1') - 8
    xrows = [struct.unpack('<II', x.read(xa + 8 * i, 8)) for i in range(SPAWN_ID + 1)]
    for cid in SHARED_IDS:
        if cid > SPAWN_ID: continue
        n, p = xrows[cid]
        if n != rows[cid][0] or (n and x.read(p, PREVIEW_REC * n) != le(grecs(cid))):
            raise ValueError('preview records of ID %d differ from the GC ones: unknown executable' % cid)
    n, p = xrows[SPAWN_ID]
    if rows[SPAWN_ID] != (0, 0) or n != 1:
        raise ValueError('preview row 29: GC (%d, 0x%X), Xbox count %d; expected (0, 0) and 1' % (*rows[SPAWN_ID], n))
    if g.read(PREVIEW_HOME, PREVIEW_REC) != bytes(PREVIEW_REC):
        raise ValueError('the room for his preview record is not empty: unknown executable')
    return {'preview': convert_preview_record(x.read(p, PREVIEW_REC))}


def _pool_spans(buf):
    """[(offset, ids)] of the 255-terminated lists in buf; zero bytes between lists are padding, and four
    zeros in a row (or the end of buf) end the block."""
    out, at = [], 0
    while at < len(buf):
        if buf[at] == 0:
            if buf[at:at + 4] == bytes(4): break
            at += 1; continue
        end = buf.find(b'\xff', at)
        if end < 0: raise ValueError('pool list at +%d has no terminator' % at)
        out.append((at, list(buf[at:end]))); at = end + 1
    return out


def pool_lists(buf):
    """The ID lists in a pool block (or from any list start within one)."""
    return [ids for _, ids in _pool_spans(buf)]


def repack_pools(block, base, table, placements):
    """The pool block with each guest after 27 in every list another disc carries with that guest in 27's
    place, packed without padding into the same size, and the pointer table moved to match: (block, table).
    `placements`: guest ID -> that disc's lists. Guests go in ID order. ValueError if it does not fit, or a
    table entry points inside a list."""
    spans = _pool_spans(block)
    wanted = {cid: {tuple(l) for l in lists} for cid, lists in placements.items()}
    out, moved = bytearray(), {}
    for off, ids in spans:
        if LINK_ID in ids:
            i = ids.index(LINK_ID)
            add = [cid for cid in sorted(wanted) if tuple(cid if v == LINK_ID else v for v in ids) in wanted[cid]]
            ids = ids[:i + 1] + add + ids[i + 1:]
        moved[base + off] = base + len(out)
        out += bytes(ids) + b'\xff'
    if len(out) > len(block):
        raise ValueError('pools need %d bytes, the block holds %d' % (len(out), len(block)))
    new_table = bytearray()
    for k in range(len(table) // 4):
        p, = struct.unpack_from('>I', table, 4 * k)
        if base <= p < base + len(block):
            if p not in moved: raise ValueError('pool table entry %d points inside a list (0x%08X)' % (k, p))
            p = moved[p]
        new_table += struct.pack('>I', p)
    return bytes(out) + bytes(len(block) - len(out)), bytes(new_table)


def pools(g, placements):
    """{'pool_block', 'pool_table'} with every guest placed, and the shipped bytes they replace ('..._old')."""
    block, table = g.read(POOL_ADDR, POOL_SIZE), g.read(POOL_TABLE, POOL_TABLE_SIZE)
    new_block, new_table = repack_pools(block, POOL_ADDR, table, placements)
    return {'pool_block': new_block, 'pool_table': new_table, 'pool_block_old': block, 'pool_table_old': table}


def spawn_pools(x, g):
    """{'pool_lists'}: the Xbox lists that place him, found by the GC's first list with 27 read as 29."""
    first = pool_lists(g.read(POOL_ADDR, POOL_SIZE))[0]
    if LINK_ID not in first:
        raise ValueError('the first pool list has no Link: unknown executable')
    at = x.find_unique(bytes(SPAWN_ID if v == LINK_ID else v for v in first) + b'\xff', 'the full pool list')
    return {'pool_lists': pool_lists(x.read(at, 0x180))}


def xbox_cell(x, faces):
    """His Xbox select cell (36 bytes, little-endian): the one record with ID 29, len(faces) costumes and
    those face indices at +8."""
    head, hits, i = struct.pack('<HH', SPAWN_ID, len(faces)), [], x.d.find(struct.pack('<HH', SPAWN_ID, len(faces)))
    while i >= 0:
        if x.d[i + 8:i + 8 + len(faces)] == bytes(faces): hits.append(i)
        i = x.d.find(head, i + 1)
    if len(hits) != 1:
        raise ValueError('his Xbox select cell: %d matches, expected 1' % len(hits))
    return x.d[hits[0]:hits[0] + CELL_STRIDE]


def cell_with_silhouettes(cell, sils, xcell):
    """His cell with his silhouette indices (+16..+23, zero-filled) and the Xbox cell's +28..+35 (f32 and two
    u16: the same on both discs for every fighter they share; Link's differ from his)."""
    if not 0 < len(sils) <= 8:
        raise ValueError('a cell holds 1..8 silhouettes, not %d' % len(sils))
    tail = struct.pack('>fHH', *struct.unpack_from('<fHH', xcell, 28))
    return cell[:16] + bytes(sils) + bytes(8 - len(sils)) + cell[24:28] + tail


def cell_with_faces(cell, faces):
    """His cell record with one costume per face portrait: u16 count at +2 and the face-bank indices
    ("A" bytes, +8..+15, zero-filled). See ui.face_bank for where the indices come from."""
    if not 0 < len(faces) <= 8:
        raise ValueError('a cell holds 1..8 faces, not %d' % len(faces))
    return cell[:2] + struct.pack('>H', len(faces)) + cell[4:8] + bytes(faces) + bytes(8 - len(faces)) + cell[16:]
