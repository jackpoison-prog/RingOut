/* RingOut.app's entry point (Contents/MacOS/RingOut).
 *
 * This was a bash script, and Finder will not launch an app whose executable is
 * a script: double-clicking RingOut.app did nothing, and only running the script
 * from Terminal worked (RingOut#20). So it is a small Mach-O instead, doing what
 * the script did.
 *
 * The app sits in the Ring Out folder beside game/, bin/ and userdata/, as the
 * runtime does on Linux; this points the runtime at them, since its own
 * directory is inside the bundle. It then exec()s the runtime, so the process
 * Finder started IS the game, with the bundle's identity -- which is what
 * GameController needs to hand it the pads.
 *
 * Plain POSIX apart from finding its own path and showing the alert, so
 * tests/test_macos_launcher.sh builds and runs it on Linux too.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <mach-o/dyld.h>
#endif

static int self_path(char *out) {
  char raw[PATH_MAX];
#ifdef __APPLE__
  uint32_t size = sizeof raw;
  if (_NSGetExecutablePath(raw, &size) != 0)
    return -1;
#else
  const ssize_t n = readlink("/proc/self/exe", raw, sizeof raw - 1);
  if (n < 0)
    return -1;
  raw[n] = '\0';
#endif
  /* Resolved, like the script's `pwd -P`: a symlinked app still finds its folder. */
  return realpath(raw, out) != NULL ? 0 : -1;
}

/* Cut the last path component off, in place. */
static void parent(char *path) {
  char *slash = strrchr(path, '/');
  if (slash == NULL)
    return;
  if (slash == path)
    slash[1] = '\0';
  else
    *slash = '\0';
}

static void alert(const char *title, const char *message) {
  fprintf(stderr, "%s: %s\n", title, message);
#ifdef __APPLE__
  /* Tests (and CI, which has nobody to click OK) need the message, not a dialog. */
  if (getenv("RINGOUT_LAUNCHER_NO_ALERT") != NULL)
    return;
  CFStringRef t = CFStringCreateWithCString(NULL, title, kCFStringEncodingUTF8);
  CFStringRef m = CFStringCreateWithCString(NULL, message, kCFStringEncodingUTF8);
  CFUserNotificationDisplayAlert(0, kCFUserNotificationStopAlertLevel, NULL, NULL,
                                 NULL, t, m, NULL, NULL, NULL, NULL);
  CFRelease(t);
  CFRelease(m);
#endif
}

/* dir + "/" + name into out, or -1 if it does not fit: a cut-off path would
 * name some other file, so it is refused rather than used. */
static int join(char *out, const char *dir, const char *name) {
  const int n = snprintf(out, PATH_MAX, "%s/%s", dir, name);
  return n >= 0 && n < PATH_MAX ? 0 : -1;
}

static int is_file(const char *p) {
  struct stat st;
  return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static int is_dir(const char *p) {
  struct stat st;
  return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

int main(int argc, char **argv) {
  char exe[PATH_MAX], macos[PATH_MAX], app[PATH_MAX], root[PATH_MAX];
  if (self_path(exe) != 0) {
    alert("Ring Out cannot start", "It could not find its own location.");
    return 1;
  }
  snprintf(macos, sizeof macos, "%s", exe);
  parent(macos); /* .../RingOut.app/Contents/MacOS */
  snprintf(app, sizeof app, "%s", macos);
  parent(app);
  parent(app); /* .../RingOut.app */
  snprintf(root, sizeof root, "%s", app);
  parent(root); /* the Ring Out folder */

  char runtime[PATH_MAX], module[PATH_MAX], game[PATH_MAX], user[PATH_MAX], log[PATH_MAX];
  const char *log_name = getenv("RINGOUT_LOG_NAME");
  if (log_name == NULL || *log_name == '\0')
    log_name = "launch.log";
  if (join(runtime, macos, "moderngekko-run") != 0 ||
      join(module, root, "bin/gGRSEAF_recomp.dylib") != 0 ||
      join(game, root, "game") != 0 || join(user, root, "userdata") != 0 ||
      join(log, user, log_name) != 0) {
    alert("Ring Out cannot start",
          "The Ring Out folder's path is too long. Move it somewhere shorter.");
    return 1;
  }

  /* No module means Setup has not run here -- or macOS moved the app to a random
   * read-only path because the download quarantine was still on it ("App
   * Translocation"), which Setup also cures by clearing that flag. */
  if (!is_file(module) || !is_dir(game)) {
    alert("Ring Out is not set up yet",
          "Double-click Setup.command in the Ring Out folder first. It recompiles "
          "your disc for this Mac, once (about 10 minutes).");
    return 1;
  }

  mkdir(user, 0755); /* EEXIST is fine; a real failure shows up as the log failing */
  const int fd = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd >= 0) {
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    if (fd > STDERR_FILENO)
      close(fd);
  }

  char **args = calloc((size_t)argc + 8, sizeof *args);
  if (args == NULL)
    return 1;
  int n = 0;
  args[n++] = runtime;
  args[n++] = "--user-dir";
  args[n++] = user;
  args[n++] = "--game";
  args[n++] = game;
  args[n++] = "--module";
  args[n++] = module;
  for (int i = 1; i < argc; ++i) {
    /* Older macOS passes a process serial number to an app Finder starts. */
    if (strncmp(argv[i], "-psn_", 5) == 0)
      continue;
    args[n++] = argv[i];
  }
  args[n] = NULL;

  execv(runtime, args);
  const int err = errno;
  char why[PATH_MAX + 128];
  snprintf(why, sizeof why, "%s could not be started: %s", runtime, strerror(err));
  alert("Ring Out cannot start", why);
  return 1;
}
