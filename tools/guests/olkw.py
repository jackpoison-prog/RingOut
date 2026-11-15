"""Rewrite a root.olk with some entries of one group replaced and/or appended (streams to disk)."""
import os, struct
from olk import Olk
def write(src_path, out_path, group, repl, append=()):
    """Write `out_path`: `src_path` with group `group`'s entries `repl` (index -> bytes) replaced and
    `append` added after its last record. A group header without room for the appended records grows to
    the next 0x800 boundary, as the discs round theirs; a header with room stays as it is. Written to a
    temporary file and renamed, so a failed write never leaves a partial archive at `out_path`."""
    src = Olk(src_path); base, tops = src.top
    f = src.f
    tmp = out_path + '.tmp'
    try:
        _write(src, tmp, group, repl, list(append))
        os.replace(tmp, out_path)
    except BaseException:
        if os.path.exists(tmp): os.remove(tmp)
        raise


def _write(src, out_path, group, repl, append):
    base, tops = src.top
    f = src.f
    def read_raw(at, n): f.seek(at); return f.read(n)
    with open(out_path, 'wb') as out:
        # top directory: same header, entries rewritten
        f.seek(0); hdr = bytearray(f.read(0x20)); hs = struct.unpack_from('<I', hdr, 0x10)[0]
        out.write(bytes(hs))  # placeholder
        new_tops = []; cur = 0  # offsets relative to data base (hs)
        for gi, (o, sz, t, z) in enumerate(tops):
            if gi != group or not sz:
                out.seek(hs + cur)
                # copy group bytes as they are
                rem = sz; at = base + o
                while rem:
                    n = min(rem, 1 << 24); out.write(read_raw(at, n)); at += n; rem -= n
                new_tops.append((cur, sz, t, z)); cur += sz; tlast = cur; cur += (-cur) % 0x800; continue
            gbase, ents = src._dir(base + o)
            f.seek(base + o); ghdr = bytearray(f.read(0x20)); ghs = struct.unpack_from('<I', ghdr, 0x10)[0]
            ghs = max(ghs, (0x20 + 16 * (len(ents) + len(append)) + 0x7ff) & ~0x7ff)
            struct.pack_into('<I', ghdr, 0x10, ghs)
            gstart = hs + cur; gcur = 0; last = 0; new_ents = []
            appended = [(None, None, 0, 0)] * len(append)
            for i, (eo, es, et, ez) in enumerate(ents + appended):
                if i >= len(ents): data = append[i - len(ents)]
                else: data = repl[i] if i in repl else (read_raw(gbase + eo, es) if es else b'')
                if not data and i not in repl and i < len(ents):
                    new_ents.append((gcur if es == 0 else gcur, 0, et, ez)); continue
                out.seek(gstart + ghs + gcur); out.write(data)
                new_ents.append((gcur, len(data), et, ez)); gcur += len(data); last = gcur; gcur += (-gcur) % 0x800
            gsize = ghs + gcur
            struct.pack_into('<I', ghdr, 0, len(new_ents))
            struct.pack_into('<I', ghdr, 0x14, gcur if struct.unpack_from('<I', ghdr, 0x14)[0] else 0)
            out.seek(gstart); out.write(bytes(ghdr))
            for e in new_ents: out.write(struct.pack('<4I', *e))
            new_tops.append((cur, gsize, t, z)); cur += gsize; tlast = cur; cur += (-cur) % 0x800
        total = hs + cur
        out.seek(total - 1); out.write(b'\0') if out.tell() < total else None
        if struct.unpack_from('<I', hdr, 0x14)[0]: struct.pack_into('<I', hdr, 0x14, tlast)
        out.seek(0); out.write(bytes(hdr))
        for e in new_tops: out.write(struct.pack('<4I', *e))
