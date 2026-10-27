#!/usr/bin/env python3
# Unit tests for steam_shortcut.py. Standard library only, like the helper.
#
#   python3 -m unittest discover -s dist/shared -p 'test_*.py' -v
#
# Fixtures are synthetic bytes built here: a real shortcuts.vdf carries its
# owner's paths and game list, and this file is public.
import os
import pathlib
import struct
import sys
import tempfile
import contextlib
import io
import unittest
import zlib
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import steam_shortcut as ss  # noqa: E402


def s(key, value):
    return b"\x01" + key.encode() + b"\x00" + value.encode() + b"\x00"


def i32(key, value):
    return b"\x02" + key.encode() + b"\x00" + struct.pack("<i", value)


def u64(key, value):
    return b"\x07" + key.encode() + b"\x00" + struct.pack("<Q", value)


def f32(key, value):
    return b"\x03" + key.encode() + b"\x00" + struct.pack("<f", value)


def m(key, *body):
    return b"\x00" + key.encode() + b"\x00" + b"".join(body) + b"\x08"


def vdf(*entries):
    return m("shortcuts", *entries) + b"\x08"


OTHER_GAME = m("0",
               i32("appid", -1234567890),
               s("AppName", "Other Game"),
               s("Exe", '"/opt/other/run"'),
               s("StartDir", '"/opt/other/"'),
               s("FlatpakAppID", ""),
               s("sortas", "other"),
               u64("DevkitGameID", 0x1122334455667788),
               f32("Weird", 1.5),
               m("tags", s("0", "favorite")))


class Codec(unittest.TestCase):
    def test_roundtrip_minimal(self):
        data = b"\x00shortcuts\x00\x08\x08"
        self.assertEqual(ss.dump_binary_vdf(ss.parse_binary_vdf(data)), data)

    def test_roundtrip_unknown_keys_and_uint64(self):
        data = vdf(OTHER_GAME)
        self.assertEqual(ss.dump_binary_vdf(ss.parse_binary_vdf(data)), data)

    def test_unknown_type_raises(self):
        data = vdf(m("0", b"\x05key\x00\x00\x00"))
        with self.assertRaises(ss.VdfError):
            ss.parse_binary_vdf(data)

    def test_truncated_raises(self):
        with self.assertRaises(ss.VdfError):
            ss.parse_binary_vdf(vdf(OTHER_GAME)[:-1])


def entries(tree):
    """The shortcuts map's entries, as {index: {key: value}}."""
    (_, _, body), = tree
    return {k: {kk: vv for _, kk, vv in v} for _, k, v in body}


LOGINUSERS = """"users"
{
	"76561198000000001"
	{
		"AccountName"		"alpha"
		"PersonaName"		"Alpha"
		"MostRecent"		"%s"
	}
	"76561198000000002"
	{
		"AccountName"		"beta"
		"PersonaName"		"Beta"
		"MostRecent"		"%s"
	}
}
"""


class Users(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmp.name)
        (self.root / "config").mkdir()

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, text):
        (self.root / "config" / "loginusers.vdf").write_text(text)

    def test_pick_user_mostrecent(self):
        self.write(LOGINUSERS % ("0", "1"))
        self.assertEqual(ss.pick_user(self.root), (76561198000000002 - 76561197960265728, "Beta"))

    def test_pick_user_single(self):
        self.write('"users"\n{\n\t"76561198000000001"\n\t{\n\t\t"PersonaName" "Alpha"\n\t}\n}\n')
        self.assertEqual(ss.pick_user(self.root), (76561198000000001 - 76561197960265728, "Alpha"))

    def test_pick_user_newest_config_when_no_mostrecent(self):
        self.write(LOGINUSERS % ("0", "0"))
        for n, t in ((1, 2000000000), (2, 1000000000)):
            d = self.root / "userdata" / str(76561198000000000 + n - 76561197960265728) / "config"
            d.mkdir(parents=True)
            os.utime(d, (t, t))
        self.assertEqual(ss.pick_user(self.root)[1], "Alpha")

    def test_find_steam_root_override(self):
        self.write(LOGINUSERS % ("1", "0"))
        self.assertEqual(ss.find_steam_root({"RINGOUT_STEAM_ROOT": str(self.root)}), self.root)

    def test_shortcuts_path(self):
        self.assertEqual(ss.shortcuts_path(self.root, 42),
                         self.root / "userdata" / "42" / "config" / "shortcuts.vdf")


class Entry(unittest.TestCase):
    EXE = pathlib.Path("/opt/Ring Out/RingOut")
    DIR = pathlib.Path("/opt/Ring Out")

    def add(self, tree, exe=EXE, start=DIR, windows=False):
        return ss.upsert_shortcut(tree, exe, start, "Ring Out", None, windows)

    def test_appid_formula(self):
        want = struct.unpack("<i", struct.pack("<I", zlib.crc32(b'"/opt/Ring Out/RingOut"Ring Out') | 0x80000000))[0]
        self.assertEqual(ss.shortcut_appid('"/opt/Ring Out/RingOut"', "Ring Out"), want)

    def test_add_to_none_creates_tree(self):
        tree, what = ss.upsert_shortcut(None, self.EXE, self.DIR, "Ring Out",
                                        pathlib.Path("/opt/Ring Out/art/icon.png"), False)
        self.assertEqual(what, "added")
        e = entries(tree)["0"]
        self.assertEqual(e["AppName"], "Ring Out")
        self.assertEqual(e["Exe"], '"/opt/Ring Out/RingOut"')
        self.assertEqual(e["StartDir"], '"/opt/Ring Out"')
        self.assertEqual(e["icon"], "/opt/Ring Out/art/icon.png")
        self.assertEqual(e["appid"], ss.shortcut_appid('"/opt/Ring Out/RingOut"', "Ring Out"))
        for k, v in (("LaunchOptions", ""), ("IsHidden", 0), ("AllowDesktopConfig", 1),
                     ("AllowOverlay", 1), ("OpenVR", 0), ("LastPlayTime", 0), ("tags", [])):
            self.assertEqual(e[k], v, k)
        # and it is a writable file
        ss.parse_binary_vdf(ss.dump_binary_vdf(tree))

    def test_add_appends_next_index(self):
        second = OTHER_GAME.replace(b"\x00" + b"0" + b"\x00", b"\x00" + b"1" + b"\x00", 1)
        data = vdf(OTHER_GAME, second)
        tree, what = self.add(ss.parse_binary_vdf(data))
        self.assertEqual(what, "added")
        self.assertEqual(sorted(entries(tree)), ["0", "1", "2"])
        self.assertTrue(ss.dump_binary_vdf(tree).startswith(data[:-2]))

    def test_add_twice_with_space_in_path_updates(self):
        tree, _ = self.add(None)
        tree, what = self.add(tree)
        self.assertEqual(what, "updated")
        self.assertEqual(len(entries(tree)), 1)

    def test_windows_exe_match_case_insensitive(self):
        tree, _ = self.add(None, pathlib.Path(r"c:\games\ringout.exe"), pathlib.Path(r"c:\games"), True)
        tree, what = self.add(tree, pathlib.Path(r"C:\Games\RingOut.exe"), pathlib.Path(r"C:\Games"), True)
        self.assertEqual(what, "updated")
        self.assertEqual(entries(tree)["0"]["Exe"], r'"C:\Games\RingOut.exe"')

    def test_update_preserves_unknown_keys(self):
        ours = m("0", s("AppName", "Ring Out"), s("Exe", '"/opt/Ring Out/RingOut"'),
                 s("sortas", "ring"), s("FlatpakAppID", ""), s("StartDir", '"/old"'))
        tree, what = self.add(ss.parse_binary_vdf(vdf(ours)))
        self.assertEqual(what, "updated")
        (_, _, body), = tree
        keys = [k for _, k, _ in body[0][2]]
        self.assertEqual(keys[:5], ["AppName", "Exe", "sortas", "FlatpakAppID", "StartDir"])
        self.assertEqual(entries(tree)["0"]["StartDir"], '"/opt/Ring Out"')

    def test_moved_folder_replaces_stale_entry(self):
        stale = m("0", s("AppName", "Ring Out"), s("Exe", '"/nonexistent/old place/RingOut"'))
        tree, what = self.add(ss.parse_binary_vdf(vdf(OTHER_GAME.replace(b"\x000\x00", b"\x001\x00", 1), stale)))
        self.assertEqual(what, "updated")
        e = entries(tree)
        self.assertEqual(len(e), 2)
        self.assertEqual(e["0"]["Exe"], '"/opt/Ring Out/RingOut"')


class Cli(unittest.TestCase):
    """main() end to end, against a temp Steam root; the seams that would touch
    a real Steam (running?, shut down, start, ask, SteamOS, Flatpak) are patched."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmp.name) / "Steam"
        (self.root / "config").mkdir(parents=True)
        (self.root / "config" / "loginusers.vdf").write_text(LOGINUSERS % ("1", "0"))
        self.vdf = ss.shortcuts_path(self.root, 76561198000000001 - 76561197960265728)
        self.game = pathlib.Path(self.tmp.name) / "Ring Out"
        self.game.mkdir()
        self.env = {"RINGOUT_STEAM_ROOT": str(self.root)}
        self.flag = pathlib.Path(self.tmp.name) / "addnonsteamgamefile"
        self.calls = []
        self.patches = [
            mock.patch.object(ss, "steam_running", lambda: False),
            mock.patch.object(ss, "is_steamos", lambda: False),
            mock.patch.object(ss, "start_steam", lambda root: self.calls.append("start")),
            mock.patch.object(ss, "ADDNONSTEAM_FLAG", str(self.flag)),
        ]
        for p in self.patches:
            p.start()

    def tearDown(self):
        for p in self.patches:
            p.stop()
        self.tmp.cleanup()

    def run_main(self, *extra):
        if getattr(self, "real_install", False):
            env = dict(self.env, RINGOUT_STEAM_ROOT="")
            patch = mock.patch.object(ss, "find_steam_root", lambda e=None: self.root)
        else:
            env, patch = self.env, contextlib.nullcontext()
        out = io.StringIO()
        with patch, contextlib.redirect_stdout(out):
            code = ss.main(["add", "--exe", str(self.game / "RingOut"), "--start-dir", str(self.game),
                            "--name", "Ring Out", *extra], env)
        lines = out.getvalue().splitlines()
        self.assertEqual(len(lines), 1, lines)
        self.assertTrue(lines[0].startswith("steam: "), lines)
        return code, lines[0]

    def test_find_prints_root(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(ss.main(["find"], self.env), 0)
        self.assertIn("Alpha", out.getvalue())

    def test_first_add_creates_file(self):
        code, line = self.run_main()
        self.assertEqual(code, 0)
        self.assertIn("added", line)
        self.assertIn("Alpha", line)
        self.assertEqual(len(entries(ss.parse_binary_vdf(self.vdf.read_bytes()))), 1)

    def test_write_makes_backup_and_replaces_atomically(self):
        self.vdf.parent.mkdir(parents=True)
        old = vdf(OTHER_GAME)
        self.vdf.write_bytes(old)
        code, _ = self.run_main()
        self.assertEqual(code, 0)
        self.assertEqual(self.vdf.with_name("shortcuts.vdf.bak-ringout").read_bytes(), old)
        self.assertFalse(self.vdf.with_name("shortcuts.vdf.tmp").exists())
        self.assertEqual(len(entries(ss.parse_binary_vdf(self.vdf.read_bytes()))), 2)

    def test_corrupt_file_is_not_written(self):
        self.vdf.parent.mkdir(parents=True)
        self.vdf.write_bytes(b"not a vdf at all")
        code, line = self.run_main()
        self.assertEqual(code, 1)
        self.assertIn("shortcuts.vdf", line)
        self.assertEqual(self.vdf.read_bytes(), b"not a vdf at all")
        self.assertFalse(self.vdf.with_name("shortcuts.vdf.bak-ringout").exists())

    def test_not_found_exit_2(self):
        self.env = {"RINGOUT_STEAM_ROOT": str(pathlib.Path(self.tmp.name) / "nowhere")}
        code, line = self.run_main()
        self.assertEqual((code, line), (2, "steam: not found"))

    def test_running_declined_exit_3(self):
        self.real_install = True
        with mock.patch.object(ss, "steam_running", lambda: True), \
             mock.patch.object(ss, "ask", lambda q: False):
            code, line = self.run_main()
        self.assertEqual(code, 3)
        self.assertIn("steam_shortcut.py", line)
        self.assertIn('add --exe', line)
        self.assertFalse(self.vdf.exists())

    def test_running_accepted_restarts(self):
        self.real_install = True
        with mock.patch.object(ss, "steam_running", lambda: True), \
             mock.patch.object(ss, "ask", lambda q: True), \
             mock.patch.object(ss, "shutdown_steam", lambda root: True):
            code, _ = self.run_main()
        self.assertEqual(code, 0)
        self.assertTrue(self.vdf.exists())
        self.assertEqual(self.calls, ["start"])

    def test_yes_flag_skips_question(self):
        self.real_install = True
        def never(q):
            raise AssertionError("asked")
        with mock.patch.object(ss, "steam_running", lambda: True), \
             mock.patch.object(ss, "ask", never), \
             mock.patch.object(ss, "shutdown_steam", lambda root: True):
            code, _ = self.run_main("--yes")
        self.assertEqual(code, 0)

    def test_shutdown_timeout_does_not_write(self):
        self.real_install = True
        with mock.patch.object(ss, "steam_running", lambda: True), \
             mock.patch.object(ss, "ask", lambda q: True), \
             mock.patch.object(ss, "shutdown_steam", lambda root: False):
            code, line = self.run_main()
        self.assertEqual(code, 1)
        self.assertIn("30 s", line)
        self.assertFalse(self.vdf.exists())
        self.assertEqual(self.calls, [])

    def test_steamos_uses_live_route(self):
        self.real_install = True
        run = mock.Mock()
        with mock.patch.object(ss, "steam_running", lambda: True), \
             mock.patch.object(ss, "is_steamos", lambda: True), \
             mock.patch.object(ss.subprocess, "run", run):
            code, line = self.run_main()
        self.assertEqual(code, 0)
        self.assertTrue(self.flag.exists())
        from urllib.parse import quote
        self.assertEqual(run.call_args[0][0],
                         ["steam", "steam://addnonsteamgame/" + quote(str(self.game / "RingOut"), safe="")])
        self.assertIn("icon", line)
        self.assertFalse(self.vdf.exists())

    def test_steamos_live_route_skips_an_entry_already_there(self):
        # Steam's live add never deduplicates: each run made one more identical
        # entry on a real Deck (2026-10-25).
        self.real_install = True
        self.vdf.parent.mkdir(parents=True)
        tree, _ = ss.upsert_shortcut(None, self.game / "RingOut", self.game, "RingOut", None, False)
        self.vdf.write_bytes(ss.dump_binary_vdf(tree))
        run = mock.Mock()
        with mock.patch.object(ss, "steam_running", lambda: True), \
             mock.patch.object(ss, "is_steamos", lambda: True), \
             mock.patch.object(ss.subprocess, "run", run):
            code, line = self.run_main()
        self.assertEqual(code, 0)
        self.assertIn("already", line)
        run.assert_not_called()
        self.assertFalse(self.flag.exists())

    def test_override_root_never_controls_real_steam(self):
        # A RINGOUT_STEAM_ROOT run is a test or a debug session: the Steam that
        # is running is the machine's real one, not the root we were given.
        def never(*a):
            raise AssertionError("touched the real Steam")
        with mock.patch.object(ss, "steam_running", lambda: True), \
             mock.patch.object(ss, "is_steamos", lambda: True), \
             mock.patch.object(ss, "ask", never), \
             mock.patch.object(ss, "shutdown_steam", never):
            code, line = self.run_main()
        self.assertEqual(code, 0)
        self.assertTrue(self.vdf.exists())
        self.assertEqual(self.calls, [])

    def test_flatpak_without_access_prints_override(self):
        with mock.patch.object(ss, "is_flatpak_root", lambda root: True), \
             mock.patch.object(ss, "flatpak_can_see", lambda path: False):
            code, line = self.run_main()
        self.assertEqual(code, 0)
        self.assertTrue(self.vdf.exists())
        self.assertIn(f"flatpak override --user --filesystem={self.game} com.valvesoftware.Steam", line)


class ReviewFixes(unittest.TestCase):
    def test_ask_prompt_not_on_stdout(self):
        # setup pipes the helper's stdout through sed / ForEach-Object, which
        # hold back a line with no newline: a prompt there is invisible.
        out, err = io.StringIO(), io.StringIO()
        with mock.patch("builtins.input", lambda *a: "y"), \
             contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            self.assertTrue(ss.ask("Close Steam?"))
        self.assertEqual(out.getvalue(), "")
        self.assertIn("Close Steam?", err.getvalue())

    def test_non_ansi_name_does_not_crash_output(self):
        with tempfile.TemporaryDirectory() as d:
            root = pathlib.Path(d)
            (root / "config").mkdir()
            (root / "config" / "loginusers.vdf").write_text(
                '"users" { "76561198000000001" { "PersonaName" "\u540d\u524d\U0001f3ae" "MostRecent" "1" } }',
                encoding="utf-8")
            raw = io.BytesIO()
            ansi = io.TextIOWrapper(raw, encoding="cp1252")
            with contextlib.redirect_stdout(ansi):
                code = ss.main(["find"], {"RINGOUT_STEAM_ROOT": d})
            ansi.flush()
            self.assertEqual(code, 0)
            self.assertTrue(raw.getvalue().startswith(b"steam: "))

    def test_tasklist_output_in_oem_codepage(self):
        done = mock.Mock(stdout=b"INFORMATION: Es werden keine Tasks mit den angegebenen Kriterien ausgef\x81hrt.\r\n")
        with mock.patch.object(ss, "WINDOWS", True), \
             mock.patch.object(ss.subprocess, "run", lambda *a, **k: done):
            self.assertFalse(ss.steam_running())

    def test_new_version_folder_replaces_old_entry(self):
        with tempfile.TemporaryDirectory() as d:
            old = pathlib.Path(d) / "RingOut-1.6.11" / "RingOut"
            old.parent.mkdir()
            old.write_text("")  # the old install is still there
            new = pathlib.Path(d) / "RingOut-1.6.12" / "RingOut"
            tree, _ = ss.upsert_shortcut(None, old, old.parent, "Ring Out", None, False)
            tree, what = ss.upsert_shortcut(tree, new, new.parent, "Ring Out", None, False)
            self.assertEqual(what, "updated")
            e = entries(tree)
            self.assertEqual(len(e), 1)
            self.assertEqual(e["0"]["Exe"], f'"{new}"')


if __name__ == "__main__":
    unittest.main()
