"""Heihachi as a guest: his block, scene, art and table rows from the player's PS2 disc, in GC formats."""
import math, os, struct, sys
_HERE = os.path.dirname(os.path.abspath(__file__))
for d in ('guests', 'spawn'): sys.path.insert(0, os.path.join(_HERE, '..', d))
import build_spawn, guest, ps2iso, ps2snd, ps2tables, ps2ui, ui, vmp2vmg, xb2gc
from olk import Olk, pack

ID = 28
PS2_BLOCK = 46 + 39 * 23
SCENES = {18: 18, 34: 34}


_C80, _S80 = math.cos(math.radians(80)), math.sin(math.radians(80))
# The GC runtime's grip per weapon half, (R, t) of the mount in its hand's frame, read from guest RAM (the select pose and
# two fight frames agree), by the weapon joint a half hangs from: 0 the right hand, 1 the left (weapon 25 stores its
# left half first). His halves are built in the bare hand's frame, so each is placed by the grip's inverse.
GRIP = {0: (((0.0, 0.0, -1.0), (0.0, 1.0, 0.0), (1.0, 0.0, 0.0)), (0.0768, 0.0384, 0.0)),
        1: (((_C80, -_S80, 0.0), (_S80, _C80, 0.0), (0.0, 0.0, 1.0)), (-0.03744, 0.0576, 0.0))}


def weapon(e):
    a = pack(e, False)
    return xb2gc.build_pack([vmp2vmg.convert(a[0], build_spawn.GC_MODEL_MAX, GRIP), ps2snd.convert_container(a[1]), xb2gc.records(a[2])])


def costume(e):
    a = pack(e, False); out = list(a)
    out[0] = vmp2vmg.convert(a[0], build_spawn.GC_MODEL_MAX); out[1] = xb2gc.records(a[1]); out[2] = ps2snd.convert_container(a[2])
    out[3] = xb2gc.bank(a[3], pose=False); out[12] = ps2snd.convert_container(a[12]); out[13] = weapon(a[13])
    built = xb2gc.build_pack(out)
    if len(built) > build_spawn.GC_COSTUME_MAX:
        raise ValueError('costume is %d bytes, over the %d budget' % (len(built), build_spawn.GC_COSTUME_MAX))
    return built


def heihachi_block(root):
    p = Olk(root); blk = {}
    for s in (0, 1, 8, 9): blk[s] = costume(p.entry(4, PS2_BLOCK + s))
    for s in range(16, 28): blk[s] = weapon(p.entry(4, PS2_BLOCK + s))
    for s in (28, 29): blk[s] = build_spawn.moveset(p.entry(4, PS2_BLOCK + s))
    for s in range(30, 37): blk[s] = ps2snd.convert_container(p.entry(4, PS2_BLOCK + s))
    return blk


def heihachi_scenes(root):
    p = Olk(root)
    return {gc: xb2gc.build_pack(xb2gc.scene(pack(p.entry(4, r), False), ps2snd.convert_container, ps2snd.MAGIC))
            for r, gc in ps2tables.home_scene(root).items()}


def heihachi_guest(root, elf, gc, dol, release=ps2iso.SLUS_20643):
    """His GuestData. root, elf: views of the PS2 disc's ROOT.OLK and executable."""
    p, g = Olk(root), Olk(gc)
    t = ps2tables.heihachi_tables(elf, dol)
    return guest.GuestData(
        id=ID, cell=ps2tables.CELL, name='Heihachi', block=heihachi_block(root), scenes=heihachi_scenes(root),
        ending_art=ps2ui.ending_art(p.entry(2, ui.ENDING_ART_BASE + ID), g.entry(2, ui.ENDING_ART_BASE + 22)),
        faces=ps2ui.faces(p.entry(*ui.FACE_ENTRY), g.entry(*ui.FACE_ENTRY)),
        silhouettes=ps2ui.silhouettes(p.entry(*ui.SILHOUETTE_ENTRY), g.entry(*ui.SILHOUETTE_ENTRY)),
        camera=ps2ui.camera(p.entry(*ui.PROFILE_CAMERA)),
        tables=t, pool_lists=t['pool_lists'],
        sources={'release': release.name, 'root_sha1': release.root_sha1, 'elf_sha1': release.elf_sha1})
