"""PS2 sound containers (mmp, SPU ADPCM) -> GC mmg (DSP ADPCM). Record layout: sndconv.gc_record's, from PS2 fields."""
import os, struct, sys
_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_HERE, '..', 'guests'))
import sndconv

MAGIC = b'mmp\0'
_FILTERS = ((0, 0), (60, 0), (115, -52), (98, -55), (122, -60))
RATES = (11025, 22050, 32000, 44100, 48000, 16000, 24000, 8000, 12000)
REC, GC_REC = 0x50, 0xb0


def spu_decode(d):
    n = len(d) // 16; out = []; s1 = s2 = 0
    for b in range(1 if n and not any(d[:16]) else 0, n):
        at = b * 16; sh = d[at] & 15; fl = (d[at] >> 4) & 7; f0, f1 = _FILTERS[fl] if fl < 5 else (0, 0)
        for i in range(28):
            by = d[at + 2 + i // 2]; v = ((by >> 4) if i & 1 else (by & 15)) << 12
            if v & 0x8000: v -= 65536
            x = ((v >> sh) if sh <= 12 else 0) + ((s1 * f0 + s2 * f1 + 32) >> 6)
            x = max(-32768, min(32767, x)); s2 = s1; s1 = x; out.append(x)
    return out


def _rec(p, offset, samples, nibbles, rate, coefs, ps):
    r = bytearray(GC_REC)
    struct.pack_into('>I', r, 0, offset)
    r[6] = p[0x1e]; r[7] = 0x40; r[8] = 0x40; r[9] = p[0x11]
    r[0x14] = 0x42; r[0x1e] = 0x7c; r[0x1f] = 6; r[0x20:0x22] = b'\x3f\x80'
    r[0x2a] = p[0x10]; r[0x2b] = p[3]; r[0x2c] = 1; r[0x2d] = p[0x12]; r[0x2e] = 0x64; r[0x2f] = 0x64
    r[0x30:0x32] = b'\x3f\x80'; r[0x34:0x36] = b'\x3f\x80'; r[0x39] = 0x7f
    struct.pack_into('>IIIHHIII', r, 0x50, samples, nibbles, rate, 0, 0, 2, nibbles - 1, 2)
    struct.pack_into('>16h', r, 0x6c, *[v for c in coefs for v in c])
    struct.pack_into('>HH', r, 0x8c, 0, ps)
    return bytes(r)


def _bank(h, d):
    flags, count = struct.unpack_from('<HH', h, 6)
    recs, data, placed = [], bytearray(), {}
    for i in range(count):
        p = h[0x10 + i * REC:0x10 + (i + 1) * REC]; size, off = struct.unpack_from('<II', p, 0x38)
        rate = round(struct.unpack_from('<H', p, 0x18)[0] * 48000 / 4096)
        near = min(RATES, key=lambda x: abs(x - rate))
        if abs(near - rate) < 50: rate = near
        if (off, size) not in placed:
            pcm = spu_decode(d[off:off + size]) or [0] * 14
            co = sndconv.coefs_for(pcm); enc, _ = sndconv.dsp_encode(pcm, co)
            placed[(off, size)] = (len(data), len(pcm), len(pcm) + (len(pcm) + 13) // 14 * 2, co, enc[0])
            data += enc; data += bytes(-len(data) % 32)
        o, smp, nib, co, ps = placed[(off, size)]
        recs.append(_rec(p, o, smp, nib, rate, co, ps))
    return b'CHDp' + struct.pack('>BBHHHI', 1, 0x10, flags, count, count * 0xa0 + 0x10, len(data)) + b''.join(recs), bytes(data)


def convert_container(x):
    if x[:4] != MAGIC: raise ValueError('not a PS2 sound container')
    nb = struct.unpack_from('<I', x, 0x14)[0]; at = (0x18 + 16 * nb + 0x1f) & ~0x1f; table = []; parts = []
    for k in range(nb):
        ho, hs, do, ds = struct.unpack_from('<4I', x, 0x18 + 16 * k)
        h, d = (b'', x[do:do + ds]) if hs == 0 else _bank(x[ho:ho + hs], x[do:do + ds])
        ha = at; at = (at + len(h) + 0x1f) & ~0x1f; da = at; at = (at + len(d) + 0x1f) & ~0x1f
        table.append((ha, len(h), da, len(d))); parts.append((ha, h, da, d))
    out = bytearray(at); out[0:4] = b'mmg\0'; struct.pack_into('>I', out, 4, at)
    out[8:10] = x[8:10]; out[10:12] = x[10:12][::-1]; out[12:16] = x[12:16]
    struct.pack_into('>II', out, 0x10, 0x100, nb)
    for k, row in enumerate(table): struct.pack_into('>4I', out, 0x18 + 16 * k, *row)
    for ha, h, da, d in parts:
        out[ha:ha + len(h)] = h; out[da:da + len(d)] = d
    return bytes(out)
