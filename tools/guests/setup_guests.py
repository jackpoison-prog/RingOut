"""Build the guests into a GameCube install from the player's own disc images: the step setup.sh runs.

    python3 setup_guests.py [--spawn <xbox image>] [--heihachi <ps2 image>] --game <install>/game --log <file> [--check-image]

Every check runs before any conversion, and each refusal is one plain sentence. The new archive is built beside the
old one (game/guests.new) and swapped in only when complete, so a failure or an interruption leaves the install
exactly as it was. A guest already installed whose image is not given is refused, so nobody loses one by accident.
Exit status: 0 built (or already built, or with --check-image the images are usable), 1 the build failed, 3 refused.
"""
import argparse, contextlib, hashlib, json, os, shutil, sys, traceback

_HERE = os.path.dirname(os.path.abspath(__file__))
for d in ('.', '../spawn', '../heihachi'): sys.path.insert(0, os.path.join(_HERE, d))
import build_guests, build_heihachi, build_spawn, ps2iso, releases, xiso

BUILT, FAILED, REFUSED = 0, 1, 3
HEADROOM = 64 << 20
SPAWN_ID, HEIHACHI_ID = 29, 28


class _Refused(Exception):
    pass


def _sha1(path):
    h = hashlib.sha1()
    with open(path, 'rb') as f:
        for c in iter(lambda: f.read(1 << 24), b''): h.update(c)
    return h.hexdigest()


def _repair(game):
    """Finish a swap that was cut short: guests.old without guests is the previous build; put it back."""
    cur, old, new = (os.path.join(game, n) for n in ('guests', 'guests.old', 'guests.new'))
    if os.path.isdir(old):
        if os.path.isdir(cur): shutil.rmtree(old)
        else: os.rename(old, cur)
    if os.path.isdir(new): shutil.rmtree(new)


def _manifest(game):
    try:
        with open(os.path.join(game, 'guests', 'guests.json')) as f: return json.load(f)
    except (OSError, ValueError):
        return None


def _installed(game):
    """IDs of the guests the install holds now. A format 1 install is Spawn."""
    m = _manifest(game)
    ids = {g.get('id') for g in m.get('guests', ())} if m else set()
    if os.path.isdir(os.path.join(game, 'spawn')): ids.add(SPAWN_ID)
    return ids


def _sources(spawn, heihachi):
    """guest ID -> what its manifest row's `source` must say."""
    out = {}
    if spawn:
        rel, _, xbe = spawn
        out[SPAWN_ID] = {'release': rel.name, 'root_sha1': rel.root_sha1, 'xbe_sha1': releases.sha1_view(xbe)}
    if heihachi:
        rel, _, _ = heihachi
        out[HEIHACHI_ID] = {'release': rel.name, 'root_sha1': rel.root_sha1, 'elf_sha1': rel.elf_sha1}
    return out


def _already_built(game, want, gc_sha1):
    m = _manifest(game)
    try:
        return (m is not None and m.get('format') == 2 and m.get('gc_root_sha1') == gc_sha1
                and m.get('converter') == build_guests._converter_version()
                and {g.get('id'): g.get('source') for g in m.get('guests', ())} == want
                and os.path.getsize(os.path.join(game, 'guests', build_guests.ARCHIVE)) == m.get('size'))
    except OSError:
        return False


def _identify(spawn_image, heihachi_image, spawn_table, heihachi_table):
    spawn = heihachi = None
    if spawn_image:
        try:
            spawn = releases.identify(xiso.open_image(spawn_image), table=spawn_table)
        except (xiso.ImageError, releases.Refusal) as e:
            raise _Refused(str(e))
        except OSError as e:
            raise _Refused('cannot read the Xbox disc image (%s)' % (e.strerror or e))
    if heihachi_image:
        try:
            heihachi = ps2iso.identify_release(ps2iso.open_image(heihachi_image), table=heihachi_table)
        except ps2iso.Refusal as e:
            raise _Refused(str(e))
        except OSError as e:
            raise _Refused('cannot read the PS2 disc image (%s)' % (e.strerror or e))
    return spawn, heihachi


def _check_install(game, spawn, heihachi, gc_sha1):
    gc_root = os.path.join(game, 'files', 'root.olk')
    if not os.path.isfile(gc_root):
        raise _Refused('no GameCube install found in %s' % game)
    with open(os.path.join(game, 'sys', 'boot.bin'), 'rb') as f: disc = f.read(6).decode('latin-1')
    if disc != 'GRSEAF':
        raise _Refused('the guests need the US GameCube disc (this install is %s)' % disc)
    if _sha1(gc_root) != gc_sha1:
        raise _Refused('the game data is modified (mods applied): remove the mods or rerun setup first')
    have = _installed(game)
    if SPAWN_ID in have and not spawn:
        raise _Refused('Spawn is installed and would be lost: give his image too with --spawn')
    if HEIHACHI_ID in have and not heihachi:
        raise _Refused('Heihachi is installed and would be lost: give his image too with --heihachi')
    # The build holds a staging archive and the final one at the same time.
    need = 2 * os.path.getsize(gc_root) + HEADROOM
    if shutil.disk_usage(game).free < need:
        raise _Refused('not enough free space: the guests need %.1f GB' % (need / 1e9))


def build(out_dir, game, spawn, heihachi):
    gc, dol = os.path.join(game, 'files', 'root.olk'), os.path.join(game, 'sys', 'main.dol')
    guests = []
    if spawn:
        rel, root, xbe = spawn
        guests.append(build_spawn.spawn_guest(root, gc, xbe, dol, release=rel))
    if heihachi:
        rel, root, elf = heihachi
        guests.append(build_heihachi.heihachi_guest(root, elf, gc, dol, release=rel))
    return build_guests.build(guests, gc, out_dir, dol)


def _names(spawn, heihachi):
    return ' and '.join(n for n, on in (('Spawn', spawn), ('Heihachi', heihachi)) if on)


def run(spawn_image, heihachi_image, game, log, check_only=False, build=build, spawn_table=releases.RELEASES,
        heihachi_table=ps2iso.RELEASES, gc_sha1=build_guests.GRSEAF_ROOT_SHA1):
    """The whole step; prints one line for the player and returns the exit status."""
    if not spawn_image and not heihachi_image:
        print('nothing to build: give --spawn or --heihachi'); return REFUSED
    try:
        spawn, heihachi = _identify(spawn_image, heihachi_image, spawn_table, heihachi_table)
        if check_only:
            if spawn: print('Spawn: %s Xbox disc recognised' % spawn[0].name)
            if heihachi: print('Heihachi: %s PS2 disc recognised' % heihachi[0].name)
            return BUILT
        _repair(game)
        _check_install(game, spawn, heihachi, gc_sha1)
    except _Refused as e:
        print(e); return REFUSED
    names = _names(spawn, heihachi)
    if _already_built(game, _sources(spawn, heihachi), gc_sha1):
        print('%s already built' % names); return BUILT
    os.makedirs(os.path.dirname(os.path.abspath(log)), exist_ok=True)
    new, cur, old, old_format = (os.path.join(game, n) for n in ('guests.new', 'guests', 'guests.old', 'spawn'))
    with open(log, 'w') as lf:
        try:
            os.makedirs(new)
            with contextlib.redirect_stdout(lf):
                print('%s: %s %s' % (names, spawn_image or '', heihachi_image or ''), flush=True)
                build(new, game, spawn, heihachi)
            if os.path.isdir(cur): os.rename(cur, old)
            os.rename(new, cur)
            if os.path.isdir(old): shutil.rmtree(old)
        except BaseException:
            traceback.print_exc(file=lf)
            shutil.rmtree(new, ignore_errors=True)
            if os.path.isdir(old) and not os.path.isdir(cur): os.rename(old, cur)
            print('%s could not be built (details: %s)' % (names, log)); return FAILED
    # The old install goes only now, with the new archive in place. The runtime ignores it if this is cut short.
    if os.path.isdir(old_format): shutil.rmtree(old_format)
    print('%s built' % names)
    return BUILT


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--spawn'); ap.add_argument('--heihachi')
    ap.add_argument('--game', required=True); ap.add_argument('--log', required=True)
    ap.add_argument('--check-image', action='store_true')
    a = ap.parse_args()
    sys.exit(run(a.spawn, a.heihachi, a.game, a.log, a.check_image))
