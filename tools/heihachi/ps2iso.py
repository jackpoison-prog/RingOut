"""The player's PS2 disc: a plain ISO 9660 image, and how to tell it is the release Heihachi can be built from.

The release is known by the SHA-1 of ROOT.OLK and of its executable. Anything else is refused: an unknown
disc would convert into something the game was never tested with."""
import dataclasses, hashlib, os, struct, sys
_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_HERE, '..', 'spawn'))
import xiso

SECTOR = 2048


class Refusal(Exception):
    """The disc cannot be used; the message is the sentence shown to the player."""


@dataclasses.dataclass(frozen=True)
class Release:
    name: str
    root_sha1: str
    elf_sha1: str
    elf_name: str


SLUS_20643 = Release('US 1.00', '392459db1229ce0d0c5b67b6fcfab10c0707413f', 'df545223b355de2121d2700fd87fea9412593def', 'SLUS_206.43')
RELEASES = (SLUS_20643,)


def open_image(path):
    """{FILE NAME: xiso.FileView} of the image's root directory. Refusal if it is no PS2 disc image."""
    try:
        size = os.path.getsize(path)
        with open(path, 'rb') as f:
            f.seek(16 * SECTOR); pvd = f.read(SECTOR)
            if len(pvd) < SECTOR or pvd[1:6] != b'CD001' or pvd[8:19] != b'PLAYSTATION':
                raise Refusal('%s is not a PS2 disc image' % os.path.basename(path))
            at, n = struct.unpack_from('<I', pvd, 158)[0], struct.unpack_from('<I', pvd, 166)[0]
            f.seek(at * SECTOR); d = f.read(n)
    except OSError as e:
        raise Refusal('cannot read the PS2 disc image (%s)' % (e.strerror or e))
    files, i = {}, 0
    while i < len(d):
        ln = d[i]
        if ln == 0:
            i = (i // SECTOR + 1) * SECTOR; continue
        ext, sz = struct.unpack_from('<I', d, i + 2)[0], struct.unpack_from('<I', d, i + 10)[0]
        name = d[i + 33:i + 33 + d[i + 32]].split(b';')[0].decode('latin-1')
        if not d[i + 25] & 2:
            if ext * SECTOR + sz > size:
                raise Refusal('%s is not a PS2 disc image: it is cut short' % os.path.basename(path))
            files[name] = xiso.FileView(path, ext * SECTOR, sz)
        i += ln
    return files


def sha1_view(view):
    h = hashlib.sha1()
    f = view.open()
    for c in iter(lambda: f.read(1 << 24), b''): h.update(c)
    f.close()
    return h.hexdigest()


def check_release(root_sha1, elf_sha1, table=RELEASES):
    """The release these hashes belong to; Refusal when there is none."""
    for r in table:
        if r.root_sha1 == root_sha1 and r.elf_sha1 == elf_sha1:
            return r
    raise Refusal('this PS2 disc is not a supported release of Soul Calibur II (supported: %s)'
                  % ', '.join(r.name for r in RELEASES))


def identify_release(files, table=RELEASES):
    """(release, ROOT.OLK view, executable view) of a supported release, hashing each file once. Refusal otherwise."""
    if 'ROOT.OLK' not in files or not any(r.elf_name in files for r in table or RELEASES):
        raise Refusal('this PS2 disc image is not Soul Calibur II')
    r0 = (table or RELEASES)[0]
    root, elf = files['ROOT.OLK'], files[r0.elf_name]
    return check_release(sha1_view(root), sha1_view(elf), table), root, elf


def identify(files, table=RELEASES):
    """(ROOT.OLK view, executable view) of a supported release. Refusal otherwise."""
    return identify_release(files, table)[1:]
