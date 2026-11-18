/* Ring Out - Ver 1.0 : Windows launcher.
 *
 * The native equivalent of the RingOut bash script on Linux. First run asks for
 * a GameCube disc image the user already has, runs setup, then starts the
 * runtime. Nothing game-derived ships with the package; everything personal is
 * produced here and stays in this folder.
 *
 * This exists so players double-click a real .exe. The earlier RingOut.cmd had
 * to bypass PowerShell's execution policy, which Windows applies to downloaded
 * .ps1 files, and flashed up a console window on the way through.
 *
 * Built by .github/scripts/package-windows.ps1 with the bundled clang:
 *   clang RingOut.c -o RingOut.exe -municode -O2 -lcomdlg32
 */

#include <windows.h>
#include <commdlg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static wchar_t g_here[MAX_PATH];

static void die(const wchar_t *msg)
{
    fwprintf(stderr, L"%ls\n", msg);
    MessageBoxW(NULL, msg, L"Ring Out", MB_OK | MB_ICONERROR);
    ExitProcess(1);
}

/* Path to <exe dir>\rest, into out. */
static void here_path(wchar_t *out, size_t n, const wchar_t *rest)
{
    _snwprintf(out, n, L"%ls\\%ls", g_here, rest);
    out[n - 1] = L'\0';
}

static BOOL path_exists(const wchar_t *p)
{
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

/* A module is g<DISCID>_recomp.dll, built from the user's own disc. */
static BOOL have_module(void)
{
    wchar_t pat[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;

    here_path(pat, MAX_PATH, L"bin\\g*_recomp.dll");
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return FALSE;
    FindClose(h);
    return TRUE;
}

static BOOL pick_file(wchar_t *out, DWORD n, const wchar_t *title, const wchar_t *filter)
{
    OPENFILENAMEW ofn;

    ZeroMemory(&ofn, sizeof ofn);
    out[0] = L'\0';
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = NULL;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = out;
    ofn.nMaxFile = n;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&ofn);
}

static BOOL pick_iso(wchar_t *out, DWORD n)
{
    /* Only what dolrecomp can actually read. Offering .rvz/.gcz/.gcm led
     * straight to "Disc extraction failed" after the user had already picked a
     * file -- the extractor rejects them by extension. */
    return pick_file(out, n, L"Ring Out - select your game disc image",
                     L"GameCube disc images (*.iso;*.wbfs)\0"
                     L"*.iso;*.wbfs\0"
                     L"All files (*.*)\0*.*\0");
}

/* The guest fighters' disc images: plain .iso, as the converters read them.
 * Cancel leaves out empty, which means "not this one". */
static void pick_guest(wchar_t *out, DWORD n, const wchar_t *title)
{
    if (!pick_file(out, n, title, L"Disc images (*.iso)\0*.iso\0All files (*.*)\0*.*\0"))
        out[0] = L'\0';
}

/* Run a command line and wait. Returns its exit code, or -1 if it never started. */
static int run_and_wait(wchar_t *cmdline, const wchar_t *workdir)
{
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD code = (DWORD)-1;

    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    ZeroMemory(&pi, sizeof pi);

    /* CreateProcessW may write to lpCommandLine, so it must be a writable buffer. */
    if (!CreateProcessW(NULL, cmdline, NULL, NULL, TRUE, 0, NULL, workdir, &si, &pi))
        return -1;

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

static BOOL has_disc_extension(const wchar_t *p)
{
    static const wchar_t *exts[] = { L".iso", L".gcm", L".rvz", L".wbfs", L".gcz" };
    size_t len = wcslen(p), i;

    for (i = 0; i < sizeof exts / sizeof *exts; ++i)
    {
        size_t e = wcslen(exts[i]);
        if (len >= e && _wcsicmp(p + len - e, exts[i]) == 0)
            return TRUE;
    }
    return FALSE;
}

int wmain(int argc, wchar_t **argv)
{
    wchar_t exe[MAX_PATH], userdir[MAX_PATH], game[MAX_PATH];
    wchar_t iso[MAX_PATH], xbox[MAX_PATH], ps2[MAX_PATH];
    wchar_t *cmd;
    size_t cap;
    int i, rc;
    BOOL have_iso_arg = FALSE, add_guests = FALSE;
    int first_passthrough = 1;

    /* Resolve our own folder; the package is relocatable, so nothing may be
     * assumed about the working directory (shortcuts often set it elsewhere). */
    if (!GetModuleFileNameW(NULL, g_here, MAX_PATH))
        die(L"Could not determine the Ring Out folder.");
    {
        wchar_t *slash = wcsrchr(g_here, L'\\');
        if (!slash)
            die(L"Could not determine the Ring Out folder.");
        *slash = L'\0';
    }

    here_path(userdir, MAX_PATH, L"userdata");
    CreateDirectoryW(userdir, NULL);
    here_path(game, MAX_PATH, L"game");
    here_path(exe, MAX_PATH, L"bin\\moderngekko-run.exe");

    /* A disc image given on the command line is consumed here and NOT forwarded
     * to the runtime, which would reject it as an unknown option. */
    iso[0] = xbox[0] = ps2[0] = L'\0';
    /* --add-guests (the "Add guest fighters" shortcut): run setup again on an
     * install that already works, asking for the guests' discs. First only, so
     * it is consumed like the disc image and never reaches the runtime. */
    if (argc > 1 && wcscmp(argv[1], L"--add-guests") == 0)
    {
        add_guests = TRUE;
        first_passthrough = 2;
    }
    for (i = first_passthrough; i < argc; ++i)
    {
        if (!have_iso_arg && has_disc_extension(argv[i]) && path_exists(argv[i]))
        {
            wcsncpy(iso, argv[i], MAX_PATH - 1);
            iso[MAX_PATH - 1] = L'\0';
            have_iso_arg = TRUE;
            first_passthrough = i + 1;
        }
    }

    if (add_guests || !have_module() || !path_exists(game))
    {
        BOOL ask_guests = add_guests;

        if (add_guests)
        {
            if (MessageBoxW(NULL,
                            L"Adding Spawn or Heihachi rebuilds the game, which takes as long as the "
                            L"first setup. Your saves and settings are kept.\n\n"
                            L"You will be asked for three disc images you already have:\n"
                            L"  1. your US GameCube disc\n"
                            L"  2. your Xbox disc, for Spawn (Cancel to skip him)\n"
                            L"  3. your PS2 disc, for Heihachi (Cancel to skip him)\n\n"
                            L"A guest already installed is kept only if you give his disc again.",
                            L"Ring Out - add guest fighters", MB_OKCANCEL | MB_ICONINFORMATION) != IDOK)
                return 0;
        }
        else
            wprintf(L"First run: this package contains no game data.\n"
                    L"You need a GameCube disc image you already have.\n\n");

        if (!have_iso_arg && !pick_iso(iso, MAX_PATH))
            die(L"No disc image selected. Setup cancelled.");

        /* The guests are optional, so the first run asks once and defaults to
         * No. A disc given on the command line is a scripted run: no question. */
        if (!add_guests && !have_iso_arg)
            ask_guests = MessageBoxW(NULL,
                                     L"Optional: Ring Out can also add the guest fighters from the other "
                                     L"versions of the game, if you have their disc images:\n\n"
                                     L"  Spawn, from your Xbox disc\n"
                                     L"  Heihachi, from your PS2 disc\n\n"
                                     L"This needs the US GameCube disc. You can also add them later from "
                                     L"the Start menu.\n\n"
                                     L"Add guest fighters now?",
                                     L"Ring Out - guest fighters",
                                     MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES;
        if (ask_guests)
        {
            pick_guest(xbox, MAX_PATH, L"Ring Out - Spawn: select your Xbox disc image (Cancel to skip)");
            pick_guest(ps2, MAX_PATH, L"Ring Out - Heihachi: select your PS2 disc image (Cancel to skip)");
            if (add_guests && !xbox[0] && !ps2[0])
                die(L"No guest disc image selected. Nothing was changed.");
        }

        {
            wchar_t setup[MAX_PATH];
            here_path(setup, MAX_PATH, L"setup.ps1");

            cap = wcslen(setup) + wcslen(iso) + wcslen(xbox) + wcslen(ps2) + 160;
            cmd = (wchar_t *)calloc(cap, sizeof *cmd);
            if (!cmd)
                die(L"Out of memory.");
            /* setup.ps1 stays PowerShell: it drives cmake and ninja, which is
             * script work, not launcher work. -ExecutionPolicy Bypass applies to
             * this process only and changes nothing system-wide. */
            _snwprintf(cmd, cap,
                       L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"%ls\" \"%ls\"",
                       setup, iso);
            if (xbox[0])
            {
                wcscat(cmd, L" -Spawn \"");
                wcscat(cmd, xbox);
                wcscat(cmd, L"\"");
            }
            if (ps2[0])
            {
                wcscat(cmd, L" -Heihachi \"");
                wcscat(cmd, ps2);
                wcscat(cmd, L"\"");
            }
            rc = run_and_wait(cmd, g_here);
            free(cmd);

            if (rc == -1)
                die(L"Could not start PowerShell to run setup.");
            if (rc != 0 || !have_module())
                die(L"Setup did not finish. See the messages in the setup window.");
        }
    }

    /* bin\moderngekko-run.exe --user-dir <userdata> [--game <game>] [args...] */
    cap = wcslen(exe) + wcslen(userdir) + wcslen(game) + 64;
    for (i = first_passthrough; i < argc; ++i)
        cap += wcslen(argv[i]) + 4;
    cmd = (wchar_t *)calloc(cap, sizeof *cmd);
    if (!cmd)
        die(L"Out of memory.");

    if (!path_exists(exe))
        die(L"bin\\moderngekko-run.exe is missing.");

    _snwprintf(cmd, cap, L"\"%ls\" --user-dir \"%ls\"", exe, userdir);
    if (path_exists(game))
    {
        wchar_t frag[MAX_PATH + 16];
        _snwprintf(frag, MAX_PATH + 16, L" --game \"%ls\"", game);
        wcscat(cmd, frag);
    }
    for (i = first_passthrough; i < argc; ++i)
    {
        wcscat(cmd, L" \"");
        wcscat(cmd, argv[i]);
        wcscat(cmd, L"\"");
    }

    rc = run_and_wait(cmd, g_here);
    free(cmd);
    if (rc == -1)
        die(L"Could not start bin\\moderngekko-run.exe.");
    return rc;
}
