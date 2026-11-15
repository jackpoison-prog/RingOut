"""The Xbox releases of Soul Calibur II that Spawn can be built from, and how to tell which one an image is.

Each release is known by the SHA-1 of its root.olk and of its executable (the original, and known patched
copies such as a region-free one), and carries the entry numbers that differ between releases. Anything
not in the table is refused: an unknown disc would convert into something the game was never tested with.
"""
import dataclasses, hashlib
import xiso


class Refusal(Exception):
    """The disc cannot be used; the message is the sentence shown to the player."""


@dataclasses.dataclass(frozen=True)
class Release:
    name: str
    root_sha1: str
    xbe_sha1: tuple
    profile_camera: int        # group 2 entry: the Character Profile camera pack
    ending_art_base: int       # group 2 entry of character 0's ending art (Spawn's = base + 29)
    face_entry: int            # group 2 entry of the select-screen face bank
    faces: tuple               # his face indices in it (one per costume)
    silhouette_entry: int      # group 2 entry of the select-screen silhouette bank
    silhouettes: tuple         # his figure indices in it (one per costume)
    scene_records: dict        # his home-stage scene (Japanese, English) on the Xbox -> the GC records ID 29
                               # reaches (both empty on GC); MAP.md, "Home-stage scene"
    dialogue_columns: int      # pre-fight lines: 6 language columns (PAL) or 1
    spawn_block: int = 46 + 39 * 23   # first group 4 record of his 39-record block


PAL = Release('PAL', '57b06abd671fbc8dc15b60d706b57ffb620d56cc', ('599dc9c1c456933de52fe6ef79bcc2e4a5300619',),
              profile_camera=516, ending_art_base=57, face_entry=5, faces=(51, 52), silhouette_entry=10,
              silhouettes=(106, 107), scene_records={17: 7, 33: 23}, dialogue_columns=6)

# The US release (version 2049, region 7): same layout as PAL except one-column pre-fight lines, one fewer
# language copy of most group-2 entries (so the profile camera is 503), and a silhouette bank 6 textures
# shorter. Entries located 2026-11-04 by matching PAL's bytes (MAP.md, "US Xbox release").
US = Release('US', '5e342e6e8c942d000898af050a2003cdf88206d5',
             ('e5d4fb1f709154436fe25cf5c3d7ba5b7f42da84',     # orig_Default.xbe
              'e32386cc1b1964108aee9c5e9e7e9f291f5c5bba'),    # Default.xbe, region-free patched
             profile_camera=503, ending_art_base=57, face_entry=5, faces=(51, 52), silhouette_entry=10,
             silhouettes=(100, 101), scene_records={17: 7, 33: 23}, dialogue_columns=1)

RELEASES = (PAL, US)

EXECUTABLES = ('orig_Default.xbe', 'Default.xbe')   # a patched disc keeps the original under orig_


def sha1_view(v):
    h = hashlib.sha1()
    for at in range(0, v.size, 1 << 24):
        h.update(v.read(at, min(1 << 24, v.size - at)))
    return h.hexdigest()


def identify(files, table=RELEASES):
    """files: xiso.open_image's result -> (Release, root.olk view, executable view). Raises Refusal."""
    root = xiso.find(files, 'root.olk')
    if root is None:
        raise Refusal('not Soul Calibur II (no root.olk on the disc)')
    exes = [v for v in (xiso.find(files, n) for n in EXECUTABLES) if v is not None]
    if not exes:
        raise Refusal('not Soul Calibur II (no Default.xbe on the disc)')
    rh = sha1_view(root)
    rel = next((r for r in table if r.root_sha1 == rh), None)
    if rel is None:
        raise Refusal('this Xbox disc is not a supported release of Soul Calibur II (supported: %s)'
                      % ', '.join(r.name for r in table if r.name))
    for v in exes:
        if sha1_view(v) in rel.xbe_sha1:
            return rel, root, v
    raise Refusal('unrecognised Default.xbe (patched?) for the %s release' % rel.name)
