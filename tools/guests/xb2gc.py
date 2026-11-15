"""Xbox -> GameCube converters for Soulcalibur II character data (throwaway spike)."""
import struct

def sw16(b, at, n):
    for i in range(at, at + 2*n, 2): b[i], b[i+1] = b[i+1], b[i]
def sw32(b, at, n):
    for i in range(at, at + 4*n, 4): b[i:i+4] = b[i:i+4][::-1]

def build_pack(parts, be=True, align=0x80):
    e = '>' if be else '<'
    n = len(parts); hdr = 4 + 4*(n+1)
    at = (hdr + align - 1) // align * align
    offs = []; body = bytearray()
    for p in parts:
        offs.append(at + len(body)); body += p; body += bytes(-len(body) % align)
    offs.append(at + len(body))
    out = bytearray(struct.pack(e+'I', n) + struct.pack(e+'%dI' % (n+1), *offs))
    out += bytes(at - len(out)); out += body
    return bytes(out)

# ---- records (costume part 1, weapon part 2): u16 kind, u16 index, u32s; kind 0: 32 B, 1: 40 B
REC = {0: 32, 1: 40}
def records(x):
    out = bytearray(x); at = 0; size = 32
    while at + 4 <= len(x):
        kind = struct.unpack_from('<H', x, at)[0]
        size = REC.get(kind, size)
        if at + size > len(x): size = len(x) - at
        sw16(out, at, 2); sw32(out, at+4, (size-4)//4); at += size
    return bytes(out)

# ---- pre-fight lines (costume part 6): u32 columns, u32 lines, u32 offsets, NUL-terminated strings.
# Little-endian on both. The Xbox has 6 strings per line (column 0 and 1 English, then German, French,
# Spanish, Italian) under 0x10; the US GC has column 0 alone under 1.
def dialogue(x, columns=6):
    """Pre-fight lines -> the GC layout (u32 1, u32 n, n string offsets, strings). The PAL disc keeps six
    columns under 0x10 (column 0 is English); the US disc already has the GC layout, zero-padded."""
    cols, n = struct.unpack_from('<II', x, 0)
    if columns == 1:
        if cols != 1:
            raise ValueError('dialogue header %#x, expected 1' % cols)
        base = 8 + 4 * n
        offs = struct.unpack_from('<%dI' % n, x, 8) if n else ()
        end = base
        for o in offs:
            z = x.find(b'\0', base + o)
            if base + o >= len(x) or z < 0:
                raise ValueError('dialogue string offset %#x is past the end' % o)
            end = max(end, z + 1)
        if any(x[end:]):
            raise ValueError('dialogue has data after its last string')
        return x[:end]
    if columns != 6:
        raise ValueError('dialogue with %d columns is not supported' % columns)
    if cols != 0x10:
        raise ValueError('dialogue header %#x, expected 0x10' % cols)
    offs = struct.unpack_from('<%dI' % (6 * n), x, 8)
    base = 8 + 24 * n
    strs = [x[base + o:x.index(b'\0', base + o) + 1] for o in offs[::6]]
    at = [sum(len(t) for t in strs[:i]) for i in range(n)]
    return struct.pack('<II', 1, n) + struct.pack('<%dI' % n, *at) + b''.join(strs)

# ---- home-stage scene records (group 4, records 8-45): part 0 a motion bank, part 1 below, part 2 a
# u32 table whose bytes 0x48-0x4F are four u16s, parts 7/8 sound containers.
def scene_names(x):
    """part 1: u32 sum, u32 table offset T, 8 bytes; 16-byte records (u16 x2, u32 x3; the last u32 is a
    name offset) up to the first name; at T a u32 offset table whose first value / 4 is its length."""
    out = bytearray(x); _, t = struct.unpack_from('<II', x, 0); sw32(out, 0, 2)
    r, end = 0x10, t
    while r + 16 <= end:
        end = min(end, struct.unpack_from('<I', x, r + 12)[0])
        sw16(out, r, 2); sw32(out, r + 4, 3); r += 16
    sw32(out, t, struct.unpack_from('<I', x, t)[0] // 4)
    return bytes(out)
def scene_table(x):
    out = bytearray(x); sw32(out, 0, 0x48 // 4); sw16(out, 0x48, 4); sw32(out, 0x50, (len(x) - 0x50) // 4)
    return bytes(out)
def scene(parts, sound, magic=b'mmx\0'):
    """A whole record from its other-disc parts; `sound` converts a container with this `magic`."""
    out = list(parts)
    out[0] = bank(parts[0]); out[1] = scene_names(parts[1]); out[2] = scene_table(parts[2])
    for k in range(3, len(out)):
        if out[k]:
            if out[k][:4] != magic:
                raise ValueError('scene part %d is not a sound container' % k)
            out[k] = sound(out[k])
    return out

# ---- motion bank
def blocks_of(frames, flags):
    if flags & 8: frames = (frames + 3)//4
    elif flags & 4: frames = (frames + 1)//2
    return (frames + 7)//8
def motion(x):
    out = bytearray(x)
    if len(x) < 0x1c: return bytes(out)
    frames, cb, flags = struct.unpack_from('<HHI', x, 0)
    sw16(out, 0, 2); sw32(out, 4, 1); out[8:16] = x[8:16][::-1]; sw16(out, 0x10, 1)
    nb = blocks_of(frames, flags)
    sw16(out, 0x1c, nb)
    pos = 0x1c
    for step in struct.unpack_from('<%dh' % nb, x, 0x1c):
        pos += step; sw16(out, pos, cb//2)
    return bytes(out)
def bank(x, pose=False):
    if len(x) < 8: return bytes(x)
    n = struct.unpack_from('<I', x, 0)[0]
    offs = list(struct.unpack_from('<%dI' % n, x, 8)) + [len(x)]
    out = bytearray(x)
    if pose:  # Xbox keeps +0x18 (LE u16 on both) 0x20 higher
        for o in offs[:-1]:
            if o + 0x1a <= len(x):
                v = struct.unpack_from('<H', x, o+0x18)[0]; struct.pack_into('<H', out, o+0x18, (v-0x20) & 0xffff)
    src = bytes(out); out = bytearray(src)
    sw32(out, 0, 2+n)
    for i in range(n):
        a, b = offs[i], offs[i+1]
        if b > a: out[a:b] = motion(src[a:b])
    return bytes(out)
def moveset_tables(x):
    n = struct.unpack_from('<I', x, 0)[0]
    offs = struct.unpack_from('<%dI' % (n+1), x, 4)
    out = bytearray(x); sw32(out, 0, n+2)
    for p in range(n):
        a, b = offs[p], offs[p+1]
        if p == 0:
            for r in range(a, b-59, 60): sw16(out, r+48, 6)
        elif p == 1:
            sw16(out, a, 2); sw32(out, a+4, 1)
            for r in range(a+8, b-39, 40): sw16(out, r+16, 12)
        else:
            sw16(out, a, 4); sw32(out, a+8, (b-a-8)//4)
    return bytes(out)
def moveset(parts):
    out = list(parts)
    if out[1]: out[1] = bank(out[1])
    if out[2]: out[2] = moveset_tables(out[2])
    return out
