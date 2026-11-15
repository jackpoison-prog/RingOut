"""Spawn as a guest: converts his Xbox block, scene, art and table rows to GC formats.

    python3 build_spawn.py --xbox <Xbox root.olk> --xbe <Xbox Default.xbe> --gc <GC US root.olk>
                           --dol <GC US main.dol> --out <dir>

His block is the Xbox's, converted. His home-stage scene (Xbox 17/33) goes in the empty records 7/23, the
ones the GC's home-stage switch gives ID 29 by default, and his ending illustrations in the empty record
86 (= 57 + ID 29). tools/guests/build_guests.py writes the archive."""
import argparse, hashlib, json, os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'guests'))
from olk import Olk, pack, X
import releases
import xb2gc, sndconv, vmx2vmg, ui, tables, guest, build_guests
from build_guests import GRSEAF_ROOT_SHA1, _converter_version

SPAWN_ID = 29
# Per-release entry numbers (his block, scenes, faces...) live in releases.py.


def weapon(xp):
    p = pack(xp, False)
    return xb2gc.build_pack([vmx2vmg.convert(p[0]), sndconv.convert_container(p[1]), xb2gc.records(p[2])])


# The game loads each player's costume into a fixed buffer, so a costume bigger than GC's own largest
# crashes the fight load (null object, Invalid read at 0xc; MAP.md). GC's largest, measured on the disc:
# model part 1,178,880 and whole entry 1,959,296 bytes (Xianghua, slot 1). Spawn's costume 2 converts
# to 1,245,696 / 2,003,840 and its largest texture (512x256 DXT5, 3 mips) loses its top level to fit.
GC_MODEL_MAX, GC_COSTUME_MAX = 1178880, 1959296


def fitted_model(vmx):
    """vmx2vmg.convert, dropping the top mip level of the largest multi-level textures until the model
    fits GC_MODEL_MAX. ValueError if nothing is left to drop."""
    drop = set()
    while True:
        model = vmx2vmg.convert(vmx, frozenset(drop))
        if len(model) <= GC_MODEL_MAX:
            return model
        left = [(w * h, k) for k, (w, h, m) in enumerate(vmx2vmg.texture_sizes(vmx)) if m >= 2 and k not in drop]
        if not left:
            raise ValueError('model is %d bytes, over the %d budget, with no mip level left to drop' % (len(model), GC_MODEL_MAX))
        drop.add(max(left)[1])


def costume(xp, release=releases.PAL):
    p = pack(xp, False); out = list(p)
    out[0] = fitted_model(p[0]); out[1] = xb2gc.records(p[1]); out[2] = sndconv.convert_container(p[2])
    out[3] = xb2gc.bank(p[3], pose=True); out[6] = xb2gc.dialogue(p[6], release.dialogue_columns); out[12] = sndconv.convert_container(p[12]); out[13] = weapon(p[13])
    e = xb2gc.build_pack(out)
    if len(e) > GC_COSTUME_MAX:
        raise ValueError('costume is %d bytes, over the %d budget' % (len(e), GC_COSTUME_MAX))
    return e


def moveset(xp):
    return xb2gc.build_pack(xb2gc.moveset(pack(xp, False)))


def scene(xp):
    return xb2gc.build_pack(xb2gc.scene(pack(xp, False), sndconv.convert_container))


def spawn_scenes(xbox=None, release=releases.PAL):
    """GC record -> converted entry for his two home-stage scene records."""
    x = Olk(xbox or X)
    return {gc: scene(x.entry(4, xb)) for xb, gc in release.scene_records.items()}


def spawn_block(xbox=None, release=releases.PAL):
    """slot -> converted GC entry for every non-empty slot of Spawn's Xbox block."""
    x = Olk(xbox or X); blk = {}; XBOX_SPAWN = release.spawn_block
    t = time.time()
    for s in (0, 1, 8, 9):
        blk[s] = costume(x.entry(4, XBOX_SPAWN + s), release); print('costume', s, '%.0fs' % (time.time() - t), flush=True)
    for s in range(16, 28):
        blk[s] = weapon(x.entry(4, XBOX_SPAWN + s))
    for s in (28, 29):
        blk[s] = moveset(x.entry(4, XBOX_SPAWN + s))
    for s in range(30, 37):
        blk[s] = sndconv.convert_container(x.entry(4, XBOX_SPAWN + s))
    print('block done %.0fs' % (time.time() - t), flush=True)
    return blk


def _sha1_file(path):
    if not isinstance(path, (str, bytes, os.PathLike)):
        return releases.sha1_view(path)
    h = hashlib.sha1()
    with open(path, 'rb') as f:
        for c in iter(lambda: f.read(1 << 24), b''): h.update(c)
    return h.hexdigest()


def spawn_guest(xbox, gc, xbe, dol, block_fn=None, tables_fn=None, scenes_fn=None, release=releases.PAL):
    """His GuestData. xbox/xbe: paths or xiso.FileViews; `release`: which Xbox release they come from."""
    tab = (tables_fn or (lambda: tables.spawn_tables(xbe, dol, release.faces)))()
    blk = (block_fn or (lambda: spawn_block(xbox, release)))()
    scenes = (scenes_fn or (lambda: spawn_scenes(xbox, release)))()
    x, g = Olk(xbox), Olk(gc)
    return guest.GuestData(
        id=SPAWN_ID, cell=4, name='Spawn', block=blk, scenes=scenes,
        ending_art=ui.ending_art(x.entry(2, release.ending_art_base + SPAWN_ID), g.entry(2, ui.ENDING_ART_BASE + 22)),
        faces=ui.face_items(x.entry(2, release.face_entry), g.entry(*ui.FACE_ENTRY), release.faces),
        silhouettes=ui.silhouette_items(x.entry(2, release.silhouette_entry), g.entry(*ui.SILHOUETTE_ENTRY), release.silhouettes),
        camera=lambda e, xc=x.entry(2, release.profile_camera): ui.profile_camera(xc, e),
        tables=tab, pool_lists=tab['pool_lists'],
        sources={'release': release.name, 'root_sha1': release.root_sha1, 'xbe_sha1': _sha1_file(xbe)})


def build(xbox, gc, out_dir, xbe, dol, check_gc=True, block_fn=None, tables_fn=None, scenes_fn=None,
          release=releases.PAL):
    """Spawn alone: out_dir/root-guests.olk and out_dir/guests.json. Returns the manifest dict."""
    if check_gc and _sha1_file(gc) != GRSEAF_ROOT_SHA1:
        raise ValueError('%s is not the unmodified GRSEAF root.olk (SHA-1 mismatch)' % gc)
    return build_guests.build([spawn_guest(xbox, gc, xbe, dol, block_fn, tables_fn, scenes_fn, release)], gc, out_dir, dol)


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--xbox-image', help='the Xbox disc image (instead of --xbox and --xbe)')
    for k in ('--xbox', '--xbe'): ap.add_argument(k, help='extracted PAL root.olk / Default.xbe (development)')
    for k in ('--gc', '--dol', '--out'): ap.add_argument(k, required=True)
    a = ap.parse_args()
    if a.xbox_image:
        import xiso
        rel, xbox, xbe = releases.identify(xiso.open_image(a.xbox_image))
    elif a.xbox and a.xbe:
        rel, xbox, xbe = releases.PAL, a.xbox, a.xbe
    else:
        ap.error('give --xbox-image, or --xbox and --xbe')
    print(json.dumps(build(xbox, a.gc, a.out, xbe, a.dol, release=rel), indent=1))
