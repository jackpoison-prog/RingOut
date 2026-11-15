"""Builds minimal XDVDFS images for tests (synthetic: no disc data)."""
import struct

S = 2048


def make_image(path, files, partition=0, truncate=0):
    """Write an image holding `files` (name -> bytes) at `partition`; cut `truncate` bytes off the end."""
    names = list(files)
    ents, off = [], 0
    for i, n in enumerate(names):
        ln = 14 + len(n)
        ents.append(ln + (-ln) % 4)
    offs = [sum(ents[:i]) // 4 for i in range(len(names))]
    dir_size = sum(ents)
    dir_size += (-dir_size) % S
    sector = 33 + dir_size // S
    d = bytearray(b'\xff' * dir_size)
    sectors = {}
    for i, n in enumerate(names):
        sectors[n] = sector
        sector += (len(files[n]) + S - 1) // S or 1
        right = offs[i + 1] if i + 1 < len(names) else 0
        e = struct.pack('<HHIIBB', 0, right, sectors[n], len(files[n]), 0x20, len(n)) + n.encode()
        d[offs[i] * 4:offs[i] * 4 + len(e)] = e
    desc = b'MICROSOFT*XBOX*MEDIA' + struct.pack('<II', 33, dir_size)
    with open(path, 'wb') as f:
        f.seek(partition + 32 * S); f.write(desc)
        f.seek(partition + 33 * S); f.write(d)
        end = partition + 34 * S
        for n in names:
            f.seek(partition + sectors[n] * S); f.write(files[n])
            end = max(end, partition + sectors[n] * S + len(files[n]))
        f.truncate(end - truncate)
