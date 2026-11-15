"""His portraits, silhouettes, ending art and profile camera from the PS2's group 2, in GC form.

The GC-side layouts and helpers are tools/guests/ui.py's. A PS2 bank is a GS upload program: each texture is
uploaded alone, so it is read back straight after its own upload (gs.indexed_texture)."""
import os, sys
_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_HERE, '..', 'guests'))
import gs, ui
from olk import pack

FACES, SILHOUETTES = (0x33, 0x34), (0x64, 0x65)


def _face(entry, k):
    return gs.indexed_texture(pack(entry, False)[ui.FACE_PART], k, gs.PSMT8, ui.FACE_W, ui.FACE_W)


def face_rows(entry, k):
    """PS2 face k -> rows of (r, g, b)."""
    idx, pal = _face(entry, k)
    return ui._rows(idx, ui.FACE_W, [p[:3] for p in pal])


def faces(ps2_entry, gc_entry, indices=FACES):
    """His faces as items for ui.append_textures on the GC face bank."""
    template = ui._face_rec(pack(gc_entry, True)[ui.FACE_PART], 0)[0]
    items = []
    for k in indices:
        idx, pal = _face(ps2_entry, k)
        items.append((template, ui._tiles8(idx, ui.FACE_W, ui.FACE_W)) + ui._gr_ab(pal) + (256,))
    return items


def _silhouette(entry, k):
    return gs.indexed_texture(pack(entry, False)[ui.SILHOUETTE_PART], k, gs.PSMT4, ui.SIL_W, ui.SIL_H)


def silhouette_indices(entry, k):
    return _silhouette(entry, k)[0]


def silhouettes(ps2_entry, gc_entry, indices=SILHOUETTES):
    """His silhouettes as items for ui.append_textures on the GC bank."""
    template = ui.silhouette_template(gc_entry)
    items = []
    for k in indices:
        idx, pal = _silhouette(ps2_entry, k)
        items.append((template, ui.tiles4(idx, ui.SIL_W, ui.SIL_H)) + ui._gr_ab(pal) + (16,))
    return items


def ending_art(ps2_entry, gc_template):
    """A PS2 ending-art entry in GC form: both pages are C8 640x448 on both discs, so the indices carry over. Every
    header byte is the template's. Part 1 (lines the US game reads from entry 87 instead) is zeroed: the PS2's holds
    another layout and would not fit."""
    gp = ui._parts(gc_template, True)
    out = bytearray(gc_template)
    t0 = gp[0][0]
    ui._template_layout(out[t0:gp[0][1]])
    part = pack(ps2_entry, False)[0]
    for k in range(2):
        idx, pal = gs.indexed_texture(part, k, gs.PSMT8, ui.GC_W, ui.GC_H)
        at = t0 + ui._PIXELS[k]; out[at:at + ui.GC_W * ui.GC_H] = ui._tiles8(idx, ui.GC_W, ui.GC_H)
        gr, ab = ui._gr_ab(pal)
        p = t0 + 0xe0 + 0x400 * k; out[p:p + 512] = gr; out[p + 0x200:p + 0x400] = ab
    out[gp[1][0]:gp[1][1]] = bytes(gp[1][1] - gp[1][0])
    return bytes(out)


def camera(ps2_entry, char_id=28):
    """GC profile-camera entry -> the entry with his three slots filled. The PS2's parts are the Xbox's layout."""
    return lambda gc_entry: ui.profile_camera(ps2_entry, gc_entry, char_id)
