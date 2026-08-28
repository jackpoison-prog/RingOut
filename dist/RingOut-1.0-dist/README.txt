Ring Out - Ver 1.1
=======================

This package contains NO game data and NO game code. You supply a
GameCube disc image that you already have; the setup step extracts it
and recompiles its executable on your machine.

QUICK START
  1. Run  ./RingOut
  2. It asks for your disc image (.iso / .gcm / .nkit.iso / .rvz)
  3. Setup extracts and recompiles it -- several minutes, one time only
  4. It launches, and every run after this starts straight away

  You can also do the setup step directly:
      ./setup.sh /path/to/your/disc.iso
      ./setup.sh --deck /path/to/your/disc.iso    (module for a Steam Deck)
      ./setup.sh --pgo  /path/to/your/disc.iso    (see below -- 10-14% faster)

WANT THE LAST 10-14%?  ./setup.sh --pgo
  A profile-guided build is worth 10-14% of CPU time -- measured over 6000
  fixed frames of an arcade match, 10.3% on a desktop and 14.1% on a Steam
  Deck. Which end of that range you land on is your machine, not luck. Profiles for the three stock discs ship in
  module-src/profiles/ and are used automatically -- but ONLY if your clang
  is new enough to read them. They are written by LLVM 22, and a profile can
  only be read by an LLVM at least as new as the one that wrote it, so the
  clang on SteamOS (20), Ubuntu 24.04 (18) and Debian 12 (14) all refuse
  them. If setup says "this clang cannot read the shipped PGO profile", that
  is what happened, and your module is correct but 10-14% slower.

  --pgo fixes it on any clang: it builds an instrumented module, plays 6000
  frames of an arcade match to collect counts, and rebuilds using those. It
  needs llvm-profdata installed (SteamOS/Arch: pacman -S llvm;
  Debian/Ubuntu: apt install llvm). Measured cost on a desktop: about 11
  minutes on top of a normal build, once. A Deck takes considerably longer.

  PLAY THE GAME FIRST. The training route needs your save data to get past
  the memory-card screen; without it the game sits on that screen for the
  whole run, and setup will say so and keep your existing module. One
  session that writes a save is enough -- it lives in userdata/GC/ and is
  picked up automatically.

  WHAT IT COSTS YOU, AND WHAT IT BUYS
    For:
      * 10-14% less CPU time. Measured over 6000 fixed frames: 176.95 ->
        158.66 Gcyc on a desktop, and 233.99 -> 201.07 on a Steam Deck
        (8 runs an arm, ranges not overlapping, one identical hash).
        On the Deck that is 75.5s of wall clock down to 68.3s.
      * On a Deck that is the margin that decides whether the heavier
        stages hold 60 fps.
      * A profile you train yourself is not second best. It came out 1.2%
        AHEAD of the one that ships, and the two did not overlap over six
        runs each, because it matches your compiler exactly.
      * It changes speed only. The guest state is untouched -- all 18
        benchmark runs produced one identical hash, profiled or not, so
        saves, netplay and replays behave the same.
    Against:
      * Time, once: about 11 minutes on a desktop, 26 on a Steam Deck, on
        top of the normal build.
      * Needs clang and llvm-profdata. gcc cannot do it.
      * Needs save data to train against (see above).
      * The module roughly doubles on disk, 23 MB -> 39 MB.
      * A profile belongs to ONE disc. Change region and you train again.
      * Interrupting the training run leaves your existing module alone --
        setup would rather keep a working module than fit a profile to a
        run that stopped early.

  You never have to do this. An unprofiled module is correct and plays
  fine; it is simply slower.

ON A STEAM DECK?
  Download RingOut-1.1-steamdeck-x86_64.zip instead -- it ships prebuilt
  binaries, because a stock SteamOS has no compiler and mounts /usr
  read-only, so out of the box nothing can be built on the device.

  You can lift that: unlock the filesystem and install a toolchain, and the
  Deck compiles its own module perfectly well -- tested end to end on
  SteamOS 3.8.25, and it produces a module that needs no bundled libraries
  at all. BUILD-ON-THE-DECK.md in the Steam Deck package has the commands.

  Otherwise build it on a desktop from THIS package, with the --deck flag:

      ./setup.sh --deck /path/to/your/disc.iso

  then copy game/ and bin/g<ID>_recomp.so into the Deck package. Add its
  RingOut launcher to Steam as a non-Steam game to run it from Game Mode.

  Do not skip --deck. A normal build targets THIS machine, and on a Zen 4/5
  or recent Intel desktop that means instructions the Deck's Zen 2 cannot
  execute -- the module crashes instantly there rather than reporting
  anything. --deck also checks the module's glibc floor, which is the other
  way a copied module fails: built on a current distro it needs glibc 2.38
  and SteamOS has about 2.37, so it will not load. That one --deck can only
  tell you about; the fix is to build on Debian 12 or Ubuntu 22.04.

REQUIREMENTS
  - A GameCube disc image you already have
  - cmake, ninja, python3, and clang (or gcc)
      Arch:    sudo pacman -S cmake ninja clang python
      Debian:  sudo apt install cmake ninja-build clang python3
  - A working Vulkan driver
  - About 1.5 GB free space for the extracted disc and build output

CONTROLS
  Escape          settings menu
  Arrow keys      navigate; Left/Right change a value or switch tab
  Space           confirm / activate
  Alt+W           toggle widescreen (16:9)
  Alt+Enter       fullscreen
  F1-F8           load state      Shift+F1-F8   save state
  Shift+Escape    quit

SETTINGS MENU TABS
  VIDEO     widescreen, internal resolution, aspect ratio, v-sync,
            anti-aliasing, anisotropic + texture filtering,
            texture packs, show FPS, free camera, fullscreen
  AUDIO     volume, mute, latency, fill audio gaps
  SYSTEM    emulation speed, save/load state, quit
  CONTROLS  input backend and device at the top, then rebind any pad
            button (Space to rebind, Left to clear). Space on either of
            the top two rows re-scans for pads plugged in since.
  CHEATS    master switch, plus any codes you add yourself to
            userdata/GameSettings/<DISCID>.ini

FREE CAMERA (enable in the VIDEO tab)
  Shift + W/A/S/D   move          Shift + Q/E   down / up
  Shift + arrows    look          Shift + Z/C   roll
  Shift + 1/2       speed         Shift + R     reset view

WHAT IS IN THIS FOLDER
  RingOut    launcher; runs setup on first use
  setup.sh      extracts your disc and builds your module
  bin/          the runtime
  lib/          bundled support libraries
  tools/        dolrecomp, the static recompiler
  module-src/   build recipe and sources for the module

  Created on your machine by setup:
  game/         your extracted disc
  work/         recompiler output and build files
  bin/g<ID>_recomp.so   your recompiled module
  userdata/     settings, save states, screenshots

CREDITS, DISCLAIMER AND LICENSING
  See CREDITS.txt. Short version: this is an unofficial fan project by
  Jack Poison, made with AI assistance, built almost entirely on the
  work of the Dolphin Emulator Project, ExpansionPak's ModernGekko and
  DolRecomp, and Dear ImGui. The game is Bandai Namco's and is not
  included here.

  The runtime is GPL-2.0-or-later, so the matching source ships in
  source/ -- keep it with this package if you pass it on.

  Anything setup builds from your disc is derived from the game itself.
  Keep it to yourself.
