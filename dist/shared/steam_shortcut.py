#!/usr/bin/env python3
# Ring Out : add the game to Steam as a non-Steam shortcut.
#
#   steam_shortcut.py find
#   steam_shortcut.py add --exe <launcher> --start-dir <folder> --name "Ring Out" [--icon <file>] [--yes]
#
# setup.sh and setup.ps1 call this at the end of a successful setup. Standard
# library only: Windows runs it with the bundled embeddable Python.
#
# Steam keeps non-Steam games in userdata/<account>/config/shortcuts.vdf, a
# BINARY KeyValues file, and rewrites it from memory when it exits -- so an
# edit made while Steam runs is silently lost. This never writes while Steam
# is running.
import argparse
import ntpath
import os
import pathlib
import posixpath
import re
import shutil
import struct
import subprocess
import sys
import time
import urllib.parse
import zlib

# --- binary KeyValues ------------------------------------------------------
# A file is one map. Each item is a type byte, a NUL-terminated key, then the
# value; 0x08 ends a map. Only the types Steam writes into shortcuts.vdf are
# understood, and anything else is an error rather than a guess: a value whose
# size we do not know cannot be skipped, and writing back a misread file would
# drop the player's other shortcuts.
#
# Node = list of (type_byte, key, value), in file order, so a file we only add
# to comes back byte for byte -- keys a newer Steam added included.
T_MAP, T_STR, T_INT, T_FLOAT, T_U64, T_END = 0x00, 0x01, 0x02, 0x03, 0x07, 0x08


class VdfError(Exception):
    pass


def _cstr(data, pos):
    end = data.find(b"\x00", pos)
    if end < 0:
        raise VdfError(f"unterminated string at byte {pos}")
    return data[pos:end].decode("utf-8", "surrogateescape"), end + 1


def _fixed(data, pos, n):
    if pos + n > len(data):
        raise VdfError(f"truncated value at byte {pos}")
    return data[pos:pos + n], pos + n


def _parse_map(data, pos):
    node = []
    while True:
        if pos >= len(data):
            raise VdfError("truncated: a map is not closed")
        t = data[pos]
        pos += 1
        if t == T_END:
            return node, pos
        key, pos = _cstr(data, pos)
        if t == T_MAP:
            value, pos = _parse_map(data, pos)
        elif t == T_STR:
            value, pos = _cstr(data, pos)
        elif t == T_INT:
            raw, pos = _fixed(data, pos, 4)
            value = struct.unpack("<i", raw)[0]
        elif t == T_U64:
            raw, pos = _fixed(data, pos, 8)
            value = struct.unpack("<Q", raw)[0]
        elif t == T_FLOAT:
            value, pos = _fixed(data, pos, 4)
        else:
            raise VdfError(f"unknown type 0x{t:02x} at byte {pos - 1}")
        node.append((t, key, value))


def parse_binary_vdf(data):
    node, pos = _parse_map(data, 0)
    if pos != len(data):
        raise VdfError(f"{len(data) - pos} bytes after the end")
    return node


def _dump_map(node, out):
    for t, key, value in node:
        out += bytes([t]) + key.encode("utf-8", "surrogateescape") + b"\x00"
        if t == T_MAP:
            _dump_map(value, out)
        elif t == T_STR:
            out += value.encode("utf-8", "surrogateescape") + b"\x00"
        elif t == T_INT:
            out += struct.pack("<i", value)
        elif t == T_U64:
            out += struct.pack("<Q", value)
        elif t == T_FLOAT:
            out += value
        else:
            raise VdfError(f"cannot write type 0x{t:02x}")
    out += bytes([T_END])


def dump_binary_vdf(node):
    out = bytearray()
    _dump_map(node, out)
    return bytes(out)


# --- finding Steam and its user -------------------------------------------
STEAMID64_BASE = 76561197960265728
FLATPAK_ROOT = ".var/app/com.valvesoftware.Steam/.local/share/Steam"


def find_steam_root(env=None):
    """The Steam install folder, or None. RINGOUT_STEAM_ROOT overrides it."""
    env = os.environ if env is None else env
    if env.get("RINGOUT_STEAM_ROOT"):
        root = pathlib.Path(env["RINGOUT_STEAM_ROOT"])
        return root if root.is_dir() else None
    if sys.platform == "win32":
        try:
            import winreg
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam") as k:
                root = pathlib.Path(winreg.QueryValueEx(k, "SteamPath")[0])
        except OSError:
            return None
        return root if (root / "config" / "loginusers.vdf").is_file() else None
    home = pathlib.Path(env.get("HOME", "~")).expanduser()
    for rel in (".steam/steam", ".local/share/Steam", FLATPAK_ROOT):
        root = home / rel
        if (root / "config" / "loginusers.vdf").is_file():
            return root.resolve()
    return None


def is_flatpak_root(root):
    return "com.valvesoftware.Steam" in pathlib.Path(root).parts


_TOKEN = re.compile(r'"((?:[^"\\]|\\.)*)"|([{}])|//[^\n]*|\s+')


def parse_text_vdf(text):
    """Steam's text KeyValues (loginusers.vdf): nested dicts of strings."""
    stack, cur, key = [], {}, None
    for mt in _TOKEN.finditer(text):
        word, brace = mt.group(1), mt.group(2)
        if brace == "{":
            if key is None:
                raise VdfError("a map has no name")
            new = {}
            cur[key] = new
            stack.append(cur)
            cur, key = new, None
        elif brace == "}":
            if not stack:
                raise VdfError("unbalanced }")
            cur = stack.pop()
        elif word is not None:
            word = word.replace('\\"', '"').replace("\\\\", "\\")
            if key is None:
                key = word
            else:
                cur[key], key = word, None
    return cur


def pick_user(root):
    """(account id, display name) of the account to add the shortcut for."""
    root = pathlib.Path(root)
    users = parse_text_vdf((root / "config" / "loginusers.vdf").read_text(
        encoding="utf-8", errors="replace")).get("users", {})
    users = {k: v for k, v in users.items() if k.isdigit() and isinstance(v, dict)}
    if not users:
        raise LookupError("no Steam account has logged in on this machine")
    recent = [k for k, v in users.items() if v.get("MostRecent") == "1"]
    if len(recent) == 1:
        sid = recent[0]
    elif len(users) == 1:
        sid = next(iter(users))
    else:
        # Several accounts and no single "most recent": the one whose config
        # Steam touched last. Named in the output, so a wrong pick is visible.
        def mtime(k):
            d = shortcuts_path(root, int(k) - STEAMID64_BASE).parent
            return d.stat().st_mtime if d.exists() else 0.0
        sid = max(sorted(users), key=mtime)
    u = users[sid]
    return int(sid) - STEAMID64_BASE, u.get("PersonaName") or u.get("AccountName") or sid


def shortcuts_path(root, account_id):
    return pathlib.Path(root) / "userdata" / str(account_id) / "config" / "shortcuts.vdf"


# --- the shortcut entry -----------------------------------------------------
def shortcut_appid(exe_field, name):
    """Steam's id for a non-Steam shortcut: crc32 of the stored Exe + AppName
    with the top bit set, stored as a signed int32."""
    v = zlib.crc32((exe_field + name).encode("utf-8")) | 0x80000000
    return struct.unpack("<i", struct.pack("<I", v))[0]


def _norm(path, windows):
    path = path.strip().strip('"')
    if windows:
        return ntpath.normcase(ntpath.normpath(path))
    return posixpath.normpath(path)


def _get(entry, key):
    for t, k, v in entry:
        if k == key:
            return v
    return None


def _set(entry, t, key, value):
    for i, (_, k, _) in enumerate(entry):
        if k == key:
            entry[i] = (t, key, value)
            return
    entry.append((t, key, value))


def upsert_shortcut(tree, exe, start_dir, name, icon, windows):
    """Add or update our entry. Returns (tree, "added" | "updated")."""
    if tree is None:
        tree = [(T_MAP, "shortcuts", [])]
    body = next((v for t, k, v in tree if t == T_MAP and k.lower() == "shortcuts"), None)
    if body is None:
        raise VdfError("no shortcuts map in the file")
    exe_field, dir_field = f'"{exe}"', f'"{start_dir}"'
    want = _norm(str(exe), windows)

    def same_exe(e):
        stored = _get(e, "Exe") or _get(e, "exe")
        return isinstance(stored, str) and _norm(stored, windows) == want

    base = (ntpath if windows else posixpath).basename(want)

    def stale_ours(e):
        # Setup run from another folder -- moved, or a new version unpacked
        # beside the old one: our name and our launcher's file name. The newest
        # setup takes the entry rather than leaving an identical-looking twin.
        stored = _get(e, "Exe") or _get(e, "exe")
        return (_get(e, "AppName") or _get(e, "appname")) == name and isinstance(stored, str) and \
            (ntpath if windows else posixpath).basename(_norm(stored, windows)) == base

    maps = [v for t, k, v in body if t == T_MAP]
    found = next((e for e in maps if same_exe(e)), None) or \
        next((e for e in maps if stale_ours(e)), None)
    fields = [(T_INT, "appid", shortcut_appid(exe_field, name)),
              (T_STR, "AppName", name),
              (T_STR, "Exe", exe_field),
              (T_STR, "StartDir", dir_field),
              (T_STR, "icon", str(icon) if icon else "")]
    if found is not None:
        for t, k, v in fields:
            # Steam writes some keys lower-case (appname, exe); update in place.
            key = next((kk for _, kk, _ in found if kk.lower() == k.lower()), k)
            _set(found, t, key, v)
        return tree, "updated"
    entry = fields + [(T_STR, "LaunchOptions", ""),
                      (T_INT, "IsHidden", 0),
                      (T_INT, "AllowDesktopConfig", 1),
                      (T_INT, "AllowOverlay", 1),
                      (T_INT, "OpenVR", 0),
                      (T_INT, "LastPlayTime", 0),
                      (T_MAP, "tags", [])]
    used = [int(k) for t, k, _ in body if k.isdigit()]
    body.append((T_MAP, str(max(used) + 1 if used else 0), entry))
    return tree, "added"


def _has_exe(tree, exe, windows):
    """Is there already a shortcut that launches exe?"""
    want = _norm(str(exe), windows)
    body = next((v for t, k, v in tree or [] if t == T_MAP and k.lower() == "shortcuts"), [])
    for t, _, e in body:
        stored = (_get(e, "Exe") or _get(e, "exe")) if t == T_MAP else None
        if isinstance(stored, str) and _norm(stored, windows) == want:
            return True
    return False


# --- Steam the program ------------------------------------------------------
# Module-level so the tests can replace each one; nothing below them touches a
# real Steam.
WINDOWS = sys.platform == "win32"
ADDNONSTEAM_FLAG = "/tmp/addnonsteamgamefile"
SHUTDOWN_WAIT_S = 30


def steam_running():
    if WINDOWS:
        # Bytes: the "no tasks" line is localised, in the OEM code page.
        out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq steam.exe", "/NH"],
                             capture_output=True).stdout
        return b"steam.exe" in out.lower()
    return subprocess.run(["pgrep", "-x", "steam"], capture_output=True).returncode == 0


def _steam_cmd(root):
    if WINDOWS:
        return [str(pathlib.Path(root) / "steam.exe")]
    if is_flatpak_root(root):
        return ["flatpak", "run", "com.valvesoftware.Steam"]
    return ["steam"]


def shutdown_steam(root):
    """Ask Steam to exit and wait for it. Never kills it: a Steam that will not
    close (an update in progress) is left alone and nothing is written."""
    subprocess.run(_steam_cmd(root) + ["-shutdown"], stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)
    for _ in range(SHUTDOWN_WAIT_S):
        time.sleep(1)
        if not steam_running():
            return True
    return False


def start_steam(root):
    kw = {"creationflags": 0x00000008} if WINDOWS else {"start_new_session": True}  # DETACHED_PROCESS
    subprocess.Popen(_steam_cmd(root), stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                     stderr=subprocess.DEVNULL, **kw)


def ask(question):
    # The prompt goes to stderr: setup pipes stdout through sed/ForEach-Object,
    # which hold back a line with no newline, and the question would be invisible.
    sys.stderr.write(f"  {question} [Y/n] ")
    sys.stderr.flush()
    try:
        answer = input()
    except EOFError:
        return False
    return not answer.strip().lower().startswith("n")


def is_steamos():
    try:
        with open("/etc/os-release") as f:
            return any(line.strip() == "ID=steamos" for line in f)
    except OSError:
        return False


def flatpak_can_see(path):
    """Whether Flatpak Steam's sandbox has filesystem access covering path."""
    try:
        out = subprocess.run(["flatpak", "info", "--show-permissions", "com.valvesoftware.Steam"],
                             capture_output=True, text=True).stdout
    except OSError:
        return False
    path = pathlib.Path(path).resolve()
    home = pathlib.Path.home()
    for line in out.splitlines():
        if not line.startswith("filesystems="):
            continue
        for fs in line.split("=", 1)[1].strip(";").split(";"):
            fs = fs.split(":")[0]
            if fs in ("host", "host-os"):
                return True
            base = home if fs == "home" else pathlib.Path(fs.replace("~", str(home), 1))
            if path == base or base in path.parents:
                return True
    return False


# --- command line -----------------------------------------------------------
def _write(target, data):
    target.parent.mkdir(parents=True, exist_ok=True)
    if target.exists():
        shutil.copyfile(target, target.with_name("shortcuts.vdf.bak-ringout"))
    tmp = target.with_name("shortcuts.vdf.tmp")
    tmp.write_bytes(data)
    os.replace(tmp, target)


def _read(target):
    return parse_binary_vdf(target.read_bytes()) if target.exists() else None


def _add(args, env):
    root = find_steam_root(env)
    if root is None:
        return 2, "not found"
    # With RINGOUT_STEAM_ROOT set (tests, debugging) the root is not the Steam
    # that is running here, so the running one is never asked, closed or used.
    real = not (os.environ if env is None else env).get("RINGOUT_STEAM_ROOT")
    try:
        account, who = pick_user(root)
    except (OSError, LookupError, VdfError) as e:
        return 2, f"no usable Steam account ({e})"
    exe = pathlib.Path(args.exe)
    start = pathlib.Path(args.start_dir)
    later = (f'to add {args.name} later, close Steam and run: '
             f'"{sys.executable}" "{pathlib.Path(__file__).resolve()}" add --exe "{exe}" '
             f'--start-dir "{start}" --name "{args.name}"' + (f' --icon "{args.icon}"' if args.icon else ''))

    target = shortcuts_path(root, account)
    if real and is_steamos() and steam_running():
        # SteamOS: Steam adds it live, as steamos-add-to-steam does. Steam
        # chooses the icon then, and for a script it has none. It also adds a
        # twin on every run, so an entry already there is left as it is.
        try:
            if _has_exe(_read(target), exe, WINDOWS):
                return 0, f"{args.name} is already in your Steam library"
        except (OSError, VdfError):
            pass  # unreadable: let Steam add it, it owns the file
        pathlib.Path(ADDNONSTEAM_FLAG).touch()
        subprocess.run(["steam", "steam://addnonsteamgame/" + urllib.parse.quote(str(exe), safe="")])
        return 0, f"asked Steam to add {args.name} (Steam picks the icon itself; set one in its Properties)"

    try:
        _read(target)  # refuse a file we cannot read before closing anyone's Steam
    except VdfError as e:
        return 1, f"cannot read {target} ({e}); left it unchanged"

    restart = False
    if real and steam_running():
        if not (args.yes or ask(f"Steam is running. Close it now to add {args.name}?")):
            return 3, f"Steam is running; {later}"
        if not shutdown_steam(root):
            return 1, f"Steam did not close within {SHUTDOWN_WAIT_S} s; nothing changed. {later}"
        restart = True

    try:
        tree = _read(target)  # again: Steam rewrites it as it exits
        tree, what = upsert_shortcut(tree, exe, start, args.name,
                                     pathlib.Path(args.icon) if args.icon else None, WINDOWS)
        _write(target, dump_binary_vdf(tree))
    except (OSError, VdfError) as e:
        return 1, f"could not update {target} ({e})"
    finally:
        if restart:
            start_steam(root)

    line = f"{what} {args.name} for {who}"
    if restart:
        line += "; Steam is starting again"
    if is_flatpak_root(root) and not flatpak_can_see(start):
        line += (f". Flatpak Steam cannot see that folder yet; run: "
                 f"flatpak override --user --filesystem={start} com.valvesoftware.Steam")
    return 0, line


def main(argv=None, env=None):
    p = argparse.ArgumentParser(prog="steam_shortcut.py")
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("find")
    a = sub.add_parser("add")
    a.add_argument("--exe", required=True)
    a.add_argument("--start-dir", required=True)
    a.add_argument("--name", required=True)
    a.add_argument("--icon")
    a.add_argument("--yes", action="store_true")
    args = p.parse_args(argv)
    # A Steam persona name or path the console code page cannot show (CJK,
    # emoji on Windows) must not turn the one result line into a traceback.
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="replace")

    if args.cmd == "find":
        root = find_steam_root(env)
        if root is None:
            code, line = 2, "not found"
        else:
            try:
                code, line = 0, f"{root} (account {pick_user(root)[1]})"
            except (OSError, LookupError, VdfError) as e:
                code, line = 2, f"{root} has no usable account ({e})"
    else:
        code, line = _add(args, env)
    print(f"steam: {line}", flush=True)
    return code


if __name__ == "__main__":
    sys.exit(main())
