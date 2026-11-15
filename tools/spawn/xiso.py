"""Read files straight out of an Xbox disc image (XDVDFS), without extracting anything.

The game partition starts at 0 in a trimmed image ("XISO"), at 0x18300000 in a full XGD1 dump and at
0x0FD90000 in a full XGD2 dump. Its volume descriptor ("MICROSOFT*XBOX*MEDIA") sits at sector 32 and gives
the root directory's sector and size. A directory is a binary tree of entries
{u16 left, u16 right, u32 sector, u32 size, u8 attributes, u8 name length, name}, links in 4-byte units.
"""
import os, struct

SECTOR = 2048
PARTITIONS = (0, 0x18300000, 0x0FD90000)
MAGIC = b'MICROSOFT*XBOX*MEDIA'


class ImageError(Exception):
    """The image cannot be used; the message is the sentence shown to the player."""


class FileView:
    """A read-only window onto one file inside the image."""

    def __init__(self, path, offset, size):
        self.path, self.offset, self.size = path, offset, size

    def read(self, off, n):
        if off < 0 or n < 0 or off + n > self.size:
            raise ValueError('read past the end of the file (%d+%d > %d)' % (off, n, self.size))
        with open(self.path, 'rb') as f:
            f.seek(self.offset + off)
            b = f.read(n)
        if len(b) != n:
            raise ImageError('the image is incomplete')
        return b

    def read_all(self):
        return self.read(0, self.size)

    def open(self):
        """A seekable read-only file object over just this file."""
        return _ViewFile(self)


class _ViewFile:
    def __init__(self, view):
        self.v, self.pos, self.f = view, 0, open(view.path, 'rb')

    def seek(self, pos, whence=0):
        self.pos = pos if whence == 0 else self.pos + pos if whence == 1 else self.v.size + pos

    def tell(self):
        return self.pos

    def read(self, n=-1):
        n = self.v.size - self.pos if n < 0 else max(0, min(n, self.v.size - self.pos))
        self.f.seek(self.v.offset + self.pos)
        b = self.f.read(n); self.pos += len(b)
        return b

    def close(self):
        self.f.close()


def _partition(f):
    for p in PARTITIONS:
        f.seek(p + 32 * SECTOR)
        h = f.read(28)
        if h[:20] == MAGIC:
            return p, struct.unpack('<II', h[20:28])
    raise ImageError('not an Xbox disc image')


def open_image(path):
    """The root directory as {name: FileView}. Raises ImageError for a non-Xbox or truncated image."""
    end = os.path.getsize(path)
    with open(path, 'rb') as f:
        part, (sec, size) = _partition(f)
        if part + sec * SECTOR + size > end:
            raise ImageError('the image is incomplete')
        f.seek(part + sec * SECTOR)
        d = f.read(size)
    files, seen, todo = {}, set(), [0]
    while todo:
        o = todo.pop()
        if o in seen or o * 4 + 14 > len(d) or d[o * 4:o * 4 + 2] == b'\xff\xff':
            continue
        seen.add(o)
        left, right, s, sz, attr, nl = struct.unpack_from('<HHIIBB', d, o * 4)
        name = d[o * 4 + 14:o * 4 + 14 + nl].decode('latin-1')
        if not attr & 0x10:  # a file, not a subdirectory
            if part + s * SECTOR + sz > end:
                raise ImageError('the image is incomplete (it ends before %s does)' % name)
            files[name] = FileView(path, part + s * SECTOR, sz)
        todo += [x for x in (left, right) if x]
    return files


def find(files, name):
    """Case-insensitive lookup (Xbox file names are case-insensitive)."""
    for n, v in files.items():
        if n.lower() == name.lower():
            return v
    return None
