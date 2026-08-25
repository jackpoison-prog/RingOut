Ring Out - Ver 1.3
Steam Deck / SteamOS build
==========================

This package contains NO game data and NO game code. Unlike the desktop
package there is also no setup step here: SteamOS ships compilers without
the C library headers and mounts /usr read-only, so nothing can be built
on the Deck itself. The runtime is prebuilt against an old glibc instead.

  runtime  built against glibc 2.36; SteamOS ships ~2.37, and binaries run
           forward across glibc versions but never backward
  module   must be -march=x86-64-v3, which the Deck's Zen 2 supports, and
           must have a glibc floor of 2.37 or lower. The desktop package's
           setup.sh gets both right if you pass --deck; it checks the result
           and tells you if it could not.

GETTING YOUR GAME ONTO IT
  The recompiled module is derived from your own disc, so it cannot ship
  with this package. Produce it once on a desktop Linux machine using the
  normal package, then copy two things across:

    1. Run  ./setup.sh --deck /path/to/your/disc.iso  in the desktop package
    2. Copy its  game/  directory into this folder
    3. Copy its  bin/g<ID>_recomp.so  into this folder's  bin/

  The launcher checks for both and will tell you which one is missing.

  --deck MATTERS. Without it setup builds for the machine it runs on, and
  a module built on a Zen 4/5 or a recent Intel desktop carries AVX-512 or
  AVX-VNNI instructions the Deck's Zen 2 cannot execute -- which is not a
  polite error but an instant crash, possibly mid-match. --deck also checks
  the glibc floor, the other way a copied module fails: build on a current
  distro (Arch, Fedora, Ubuntu 24.04) and it wants glibc 2.38, SteamOS has
  ~2.37, and it dies at load. That one --deck can only diagnose, not fix --
  build the module on Debian 12 or Ubuntu 22.04 if you hit it.

  Both failures used to arrive silently at the end of a long recompile.
  setup.sh now says which one you have, and what to do about it.

INSTALLING
  Put this folder anywhere on the Deck -- internal storage or an SD card.
  In Desktop Mode, add RingOut to Steam as a non-Steam game, then launch
  it from Game Mode. Running ./RingOut directly from a terminal also
  works and is the quicker way to see errors.

  Game Mode gives you Steam's own controller configuration. The runtime
  reads the Deck's built-in pad through SDL, which is built into the
  binary rather than loaded from the system, so it sees whatever Steam
  Input presents to it; the CONTROLS tab rebinds anything that lands
  wrong.

CONTROLS
  Escape          settings menu
  Arrow keys      navigate; Left/Right change a value or switch tab
  Space           confirm / activate
  Alt+W           toggle widescreen (16:9)
  F1-F8           load state      Shift+F1-F8   save state
  Shift+Escape    quit

  Fullscreen is left to Steam. In Game Mode gamescope owns the display and
  scales the 4:3 image to the Deck's 1280x800 panel, so Alt+Enter and the
  fullscreen setting are not needed and are best left alone.

SETTINGS MENU TABS
  VIDEO     widescreen, internal resolution, aspect ratio, v-sync,
            anti-aliasing, anisotropic + texture filtering, filter,
            texture packs, show FPS, free camera, fullscreen
  AUDIO     volume, mute, latency, fill audio gaps
  SYSTEM    emulation speed, CPU overclock, save/load state, netplay, quit
  CONTROLS  backend and device at the top, then rebind any pad button
            (Space to rebind, Left to clear). The Deck's pad can show up
            under more than one backend -- Steam Deck, SDL and XInput2 are
            all built in -- so those two rows pick which one is used.
            Space on either of them re-scans for pads plugged in since.
  CHEATS    master switch, plus any codes you add yourself to
            userdata/GameSettings/<DISCID>.ini

  A word on internal resolution: the Deck renders this game comfortably at
  2x, and 2x is already well above the 1280x800 panel. Higher multipliers
  cost battery for nothing you can see.

NETPLAY (SYSTEM tab)
  Netplay          OFF / HOST / JOIN
  Join Address     the host's IP, shown only when joining
  Netplay Port     must match on both machines
  Start Netplay    restarts the game into the session

  One player hosts and the others join, so exactly one machine sets
  HOST and the rest set JOIN plus that machine's IP. Starting a session
  restarts the game -- netplay cannot be switched on mid-match, because
  every peer has to begin from the same first frame.

  Entering the address: Space starts editing, Left/Right pick which of
  the four numbers you are on, Up/Down change it, Space finishes. Hold a
  direction to move in larger steps. The address is remembered, so this
  is a once-per-opponent job rather than every session.

  Both machines must be on the same network, or the host's router must
  forward the port. A CPU overclock is forced off for everyone during
  netplay -- peers have to run at the same clock to stay in step.

FILTERS (VIDEO tab, Filter row)
  Two filters are bundled and installed into userdata/Shaders on first
  run: Scanlines and CRT. An edited filter is never overwritten, so your
  own tweaks survive an update. The row also cycles a selection of
  Dolphin's own shaders.

FREE CAMERA (enable in the VIDEO tab)
  Shift + W/A/S/D   move          Shift + Q/E   down / up
  Shift + arrows    look          Shift + Z/C   roll
  Shift + 1/2       speed         Shift + R     reset view

WHAT IS IN THIS FOLDER
  RingOut       launcher
  bin/          the runtime, and your recompiled module once you copy it in
  lib/          bundled support libraries
  shaders/      the bundled filters, installed to userdata on first run

  Created on first run, or copied from a desktop setup:
  game/         your extracted disc
  userdata/     settings, save states, screenshots

CREDITS, DISCLAIMER AND LICENSING
  See CREDITS.txt. Short version: this is an unofficial fan project by
  Jack Poison, made with AI assistance, built almost entirely on the
  work of the Dolphin Emulator Project, ExpansionPak's ModernGekko and
  DolRecomp, and Dear ImGui. The game is Bandai Namco's and is not
  included here.

  The runtime is GPL-2.0-or-later. Because these binaries are prebuilt,
  the matching source matters more here than in the desktop package --
  CREDITS.txt says where to get it.

  Anything built from your disc is derived from the game itself.
  Keep it to yourself.
