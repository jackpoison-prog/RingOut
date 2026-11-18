"""Build root-guests.olk (the GC US root.olk plus every guest) and its manifest guests.json.

Each guest's block is appended to group 4 in a fixed order (Spawn first when present), his home-stage
scene goes in empty group 4 records, his ending art in empty record 2/(57 + ID), and his portraits and
silhouettes are appended to the banks. The manifest is written last, after the archive is complete and
renamed into place, so a build that stops part-way never leaves a manifest the runtime would trust."""
import hashlib, json, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from olk import Olk
import olkw, tables, ui

GRSEAF_ROOT_SHA1 = '7ee2f6663e5b2386a2008d20428363763843677e'
GC_FIRST_RECORD = 982   # group 4's first free record on GC
BLOCK_RECORDS = 39
ARCHIVE, MANIFEST = 'root-guests.olk', 'guests.json'
_HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE_DIRS = tuple(os.path.join(_HERE, '..', d) for d in ('guests', 'spawn', 'heihachi'))


def _sha1_file(path):
    h = hashlib.sha1()
    with open(path, 'rb') as f:
        for c in iter(lambda: f.read(1 << 24), b''): h.update(c)
    return h.hexdigest()


def _converter_version(src_dir=None):
    """sha1 over the converter's own sources (top-level *.py and *.c in each folder, sorted by name): what
    an "already built" check compares. No git: a player's install has none."""
    h = hashlib.sha1()
    for d in ((src_dir,) if src_dir else SOURCE_DIRS):
        if not os.path.isdir(d): continue
        for n in sorted(os.listdir(d)):
            p = os.path.join(d, n)
            if n.endswith(('.py', '.c')) and os.path.isfile(p):
                h.update(n.encode() + b'\0')
                with open(p, 'rb') as f: h.update(f.read())
    return h.hexdigest()


def _rows(g, block, cell):
    t = g.tables
    m = {'id': g.id, 'block': block, 'roster': t['roster'], 'voice': t['voice'], 'costume': t['costume'].hex(),
         'descriptor': t['descriptor'].hex(), 'strings': t['strings'].hex(), 'cell': cell.hex(),
         'ending': t['ending'].hex(), 'ending_old': t['ending_old'].hex(), 'preview': t['preview'].hex(),
         'source': g.sources}
    if t.get('name'): m['name'] = t['name'].hex()
    return m


def build(guests, gc, out_dir, dol):
    """Write out_dir/root-guests.olk and out_dir/guests.json; return the manifest dict. ValueError, with
    nothing left behind, when a record a guest needs is taken or two guests want the same one."""
    guests = sorted(guests, key=lambda g: -g.id)
    if not guests: raise ValueError('no guests to build')
    archive, stage, manifest = (os.path.join(out_dir, n) for n in (ARCHIVE, '.' + ARCHIVE + '.stage1', MANIFEST))
    try:
        g = Olk(gc)
        if len(g.group(4)[1]) != GC_FIRST_RECORD:
            raise ValueError('group 4 has %d records, expected %d' % (len(g.group(4)[1]), GC_FIRST_RECORD))
        scenes = {}
        for s in guests:
            for r, e in s.scenes.items():
                if r in scenes: raise ValueError('two guests want group 4 record %d' % r)
                if g.entry(4, r): raise ValueError('group 4 record %d is not empty on this disc' % r)
                scenes[r] = e
        appended, faces, sils, camera, group2, rows = [], g.entry(*ui.FACE_ENTRY), g.entry(*ui.SILHOUETTE_ENTRY), \
            g.entry(*ui.PROFILE_CAMERA), {}, []
        for k, s in enumerate(guests):
            art_at = ui.ENDING_ART_BASE + s.id
            if g.entry(2, art_at): raise ValueError('group 2 record %d is not empty on this disc' % art_at)
            group2[art_at] = s.ending_art
            faces, first = ui.append_textures(faces, ui.FACE_PART, s.faces)
            sils, sfirst = ui.append_textures(sils, ui.SILHOUETTE_PART, s.silhouettes)
            camera = s.camera(camera)
            cell = tables.cell_with_faces(s.tables['cell'], range(first, first + len(s.faces)))
            cell = tables.cell_with_silhouettes(cell, range(sfirst, sfirst + len(s.silhouettes)), s.tables['xcell'])
            appended += [s.block.get(i, b'') for i in range(BLOCK_RECORDS)]
            rows.append(_rows(s, GC_FIRST_RECORD + k * BLOCK_RECORDS, cell))
        pools = tables.pools(tables.Dol(dol), {s.id: s.pool_lists for s in guests})
        group2.update({ui.FACE_ENTRY[1]: faces, ui.PROFILE_CAMERA[1]: camera, ui.SILHOUETTE_ENTRY[1]: sils})
        olkw.write(gc, stage, 4, scenes, append=appended)
        olkw.write(stage, archive, 2, group2)
        os.remove(stage)
        h = hashlib.sha1()
        for d in appended + [scenes[r] for r in sorted(scenes)] + [group2[r] for r in sorted(group2)] + \
                [pools['pool_block'], pools['pool_table']]: h.update(d)
        for r in rows: h.update(json.dumps({k: v for k, v in r.items() if k != 'source'}, sort_keys=True).encode())
        m = {'format': 2, 'disc': 'GRSEAF', 'converter': _converter_version(), 'gc_root_sha1': GRSEAF_ROOT_SHA1,
             'sha1': _sha1_file(archive), 'size': os.path.getsize(archive), 'guests_sha1': h.hexdigest(),
             'edits': [{'group': 4, 'append_first': GC_FIRST_RECORD, 'count': len(appended)},
                       *({'group': 4, 'replace': r} for r in sorted(scenes)),
                       *({'group': 2, 'replace': r} for r in sorted(group2))],
             **{k: v.hex() for k, v in pools.items()}, 'guests': rows}
        # newline: the same bytes on every host (Windows would write CRLF).
        with open(manifest + '.tmp', 'w', newline='\n') as f: json.dump(m, f, indent=1)
        os.replace(manifest + '.tmp', manifest)
        return m
    except BaseException:
        for p in (stage, stage + '.tmp', archive, archive + '.tmp', manifest + '.tmp', manifest):
            if os.path.exists(p): os.remove(p)
        raise
