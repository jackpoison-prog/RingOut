# Ring Out

A native PC port of a GameCube fighting game (disc ID `GRSEAF`), produced by **static
recompilation** rather than emulation: the disc's PowerPC executable is translated
ahead of time into C, compiled for x86-64, and run as native code inside a
Dolphin-derived runtime that provides the graphics, audio, input and hardware
emulation around it.

**No game data and no game code are distributed here.** You supply a disc image you
already own; setup extracts it and recompiles it on your machine.

---

## Status

Fully playable — boots to menu and through gameplay, with **zero interpreter
fallback**: every executed block runs as native code. The recompiler translates
535,368 instructions with 0 unknown opcodes. Rendering is Vulkan on the GPU, with
the CPU emulation and the runtime on separate cores.

**Steam Deck**: supported, with its own prebuilt package — no toolchain, no
compile step. Runs in both Desktop and Game Mode at 45–49 fps in a match.

**Netplay**: working. Rollback over a deterministic dual-core setup, with a lobby
showing live ping and per-player game status; two peers stayed byte-identical
over 6,470 frames.

---

## Getting it

Two packages, from the [Releases](../../releases) page:

| | for | needs a toolchain? |
| --- | --- | --- |
| `RingOut-1.2.1-linux-x86_64.zip` | desktop Linux | yes — compiles on your machine |
| `RingOut-1.2.1-steamdeck-x86_64.zip` | Steam Deck / SteamOS | no — prebuilt |

The Deck package ships no module: build one on a desktop with the package below,
then copy `game/` and `bin/gGRSEAF_recomp.so` across. Add `RingOut` to Steam as a
non-Steam game to launch it from Game Mode.

For desktop, unzip and run:

```sh
./RingOut
```

On first run it asks for your disc image (`.iso` / `.gcm` / `.nkit.iso` / `.rvz`),
then extracts and recompiles it — several minutes, once. Every run after that starts
straight away. You can also pass the image directly:

```sh
./RingOut /path/to/disc.iso     # or: ./setup.sh /path/to/disc.iso
```

### Requirements

- A GameCube disc image you already own
- `cmake`, `ninja`, `python3`, and `clang` (or `gcc`)
  - Arch: `sudo pacman -S cmake ninja clang python`
  - Debian/Ubuntu: `sudo apt install cmake ninja-build clang python3`
- A working Vulkan driver
- ~1.5 GB free for the extracted disc and build output

The release binary is built on **glibc 2.44**. For older hosts the launcher falls
back to a bundled glibc in `libc-fallback/` — see [Known issues](#known-issues)
before relying on that.

---

## Features

- **Static recompilation** — native x86-64 execution, no interpreter fallback
- **Vulkan renderer** with internal-resolution scaling, AA, anisotropic filtering
- **Widescreen (16:9)** — `Alt+W`
- **HD texture pack support** — drop a pack in `userdata/Load/Textures/GRSEAF/`
- **In-game overlay**: pause menu with staged settings and Reset Game; Video, Audio,
  System, Controls and Cheats tabs
- **Full controller remapping**
- **Save states** — `Shift+F1`–`F8` to save, `F1`–`F8` to load
- **Free camera** — fly the camera anywhere in a match
- **FMV playback** via FFmpeg, replacing the software Sofdec decoder
- **23 verified cheat codes** shipped in `GameSettings/GRSEAF.ini`
- **Netplay** — rollback, lobby with live ping and per-player game status
- **Its own icon** — the disc banner and the memory-card icon are extracted from
  your disc and saves on your machine, and a desktop entry is written for you.
  None of that artwork ships; it is the publisher's.

### Controls

| Key | Action |
| --- | --- |
| `Escape` | settings menu |
| Arrow keys | navigate; Left/Right change a value or switch tab |
| `Space` | confirm / activate |
| `Alt+W` | toggle widescreen |
| `Alt+Enter` | fullscreen |
| `F1`–`F8` / `Shift+F1`–`F8` | load / save state |
| `Shift+Escape` | quit |

Free camera (enable in the Video tab): `Shift+WASD` move, `Shift+Q/E` down/up,
`Shift+arrows` look, `Shift+Z/C` roll, `Shift+1/2` speed, `Shift+R` reset.

---

## Performance work

Everything below is measured on a **fixed-work gameplay benchmark** — the same
emulated frames, driven by a frame-keyed input script, counted in retired CPU
cycles rather than wall time. That harness exists because the earlier one did
not measure the right thing: it ran boot and an idle menu, which carry ~0.1% of
a real session's paired-single traffic, so a change could look free there and
cost 3% in a match. **Earlier figures in this README were taken that way and have
been removed rather than restated.**

### What shipped

| Change | Effect |
| --- | --- |
| Inline the paired-single helper chain | psq cost per unit of work −43% |
| Compile loop back-edges as native `goto` | dispatches −66%, CPU −11.5% |
| Defer FPRF classification until FPSCR is observable | −2.14% cycles (Deck) |
| `-flto=auto` | module relink 22 min → 6 |

The FPRF one is the shape most of these take: the FP condition register is
written 3.07 billion times per run and read 15,885 times — mandatory by
architecture, dead in practice — so it is now computed only where something can
observe it, with frame hashes proving guest state is unchanged.

### What was measured and rejected

Kept here because the negative results cost as much to establish as the wins,
and each one looks plausible enough to be retried:

| Idea | Result |
| --- | --- |
| BOLT post-link layout | **8.1% slower** |
| `-O2` instead of `-O3` | **9.1% slower**, despite 12.5% less code |
| LLVM object backend | **4% slower**, even removing 43% of dispatches |
| Profile-guided optimisation | does not build — SIGBUS before the first frame |
| Leaders-only entry labels | 81% of execution fell out to the interpreter |
| Block-local register allocation | tied; no cross-iteration residency to exploit |
| CR / XER[CA] elision | ceiling ~0.05% and ~0.2% respectively |

The pattern: BOLT, `-O2` and the LLVM backend all bought instruction locality or
fewer dispatches, and all three lost by retiring more instructions at lower IPC.
PMU attribution explains why — front-end starvation is 2.4% of cycles and the
back end is saturated at IPC 1.92. The workload is not inefficient, just large:
86.4 million host instructions per emulated frame.


---

## Known issues

- **The Deck package is SteamOS-only.** Its runtime is built against glibc 2.36 in
  a Debian 12 container, which clears SteamOS and essentially every current
  distro. A build made natively on SteamOS instead has a 2.38 floor and will not
  run on older SteamOS releases — so the container build stays the shipped one.
- **Windows is retired.** There is no Windows CI job and no Windows package. The
  workflow, packaging script, installer and launcher scaffolding are kept, unbuilt
  and unmaintained, under `attic/windows/`. Linux and the Steam Deck are the
  supported targets.
- The `-march=native` build is machine-specific by design; setup compiles on your
  own machine. It matters in exactly one place: the Deck package ships no module
  and sends you here to build one, and a module built on a Zen 4/5 or recent
  Intel desktop carries instructions the Deck's Zen 2 cannot execute — an instant
  crash, not a load error. **Use `./setup.sh --deck <disc>` for a module you
  intend to copy to a Deck**; it builds `-march=x86-64-v3` and checks the glibc
  floor, the other way a copied module fails.

### Fixed since the last revision of this page

- **Netplay was described here as stubbed out.** It is not: there is a lobby with
  live ping and per-player game status, automatic pad assignment, and a
  host-controlled input buffer.
- **The shutdown abort** (`terminate called without an active exception`) was a
  real bug rather than a cosmetic one, and is fixed.
- **The bundled-glibc fallback on SteamOS** is no longer the plan. The predicted
  NSS failure is moot: the Deck package is built in a container against an old
  glibc instead, and is verified on hardware in both Desktop and Game Mode.

---

## Building from source

```sh
cmake -S ModernGekko -B ModernGekko/build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C ModernGekko/build
```

Then build the recompiled module for your disc with `dist/RingOut-1.0-dist/setup.sh`,
which drives `dolrecomp` and compiles the generated C.

Releases are assembled by script, never by zipping a working directory — those
hold the extracted disc, saves and build output. Each stage is built from an
allowlist and then checked: no disc-derived files, no personal data, and for the
desktop package, that the prebuilt runtime can actually load a module built from
the sources shipped beside it.

```sh
.github/scripts/package-deck.sh     # Steam Deck zip
.github/scripts/package-dist.sh     # desktop Linux zip
.github/scripts/regen-source.sh     # refresh the GPL source shipment
```

### Repository layout

| Path | What it is |
| --- | --- |
| `ModernGekko/` | the runtime fork — chassis, settings overlay, StaticRecomp core |
| `ModernGekko/vendor/dolphin/` | the vendored Dolphin tree the runtime is built from |
| `DolRecomp/` | the static recompiler fork (PowerPC → C) |
| `dist/RingOut-1.0-dist/` | the desktop redistributable: launcher, `setup.sh`, module build recipe |
| `dist/RingOut-1.0-deck/` | the Steam Deck package scaffolding |
| `dist/shared/gc-art.py` | extracts the game's banner and icon on the player's machine |
| `.github/scripts/` | packaging, the privacy scan, the GPL source shipment, benchmarks |
| `work/mg_userdir/GameSettings/GRSEAF.ini` | the verified cheat codes |

Everything is vendored as plain files — clone and build, no submodule init needed.

---

## Credits

Almost none of the heavy lifting here is original. This is a thin layer on other
people's work:

- **[Dolphin Emulator Project](https://dolphin-emu.org)** — the runtime *is* Dolphin.
  Graphics, audio, input, save states, GameCube hardware emulation and the Vulkan
  backend are all theirs. GPL-2.0-or-later.
- **[ExpansionPak — ModernGekko](https://github.com/ExpansionPak/ModernGekko)** — the
  chassis that hosts a statically recompiled game inside Dolphin, and the
  StaticRecomp core that hands execution between recompiled code and the emulator.
- **[ExpansionPak — DolRecomp](https://github.com/ExpansionPak/DolRecomp)** — the
  static recompiler. GPL-3.0-or-later.
- **Dear ImGui** (Omar Cornut and contributors) — the settings overlay UI. MIT.
- **FFmpeg** — used as a separate program to decode the game's Sofdec video.
- **Bandai Namco Entertainment** — the original game, its code and all its assets.
  Not included, not redistributed, and not ours. All rights remain theirs.
- Action Replay cheat codes are published community data (Codejunkies, via Almar's
  Guides). None were written here.
- The r/decomps post that described the recompilation workflow this project followed.

Assembled by **Jack Poison**, working with AI assistance (Anthropic's Claude). The AI
wrote and debugged a large share of the code — the settings overlay, controller
remapping and cheat pages, the free-camera wiring, the FMV replacement path, several
recompiler codegen fixes and the packaging — under direction, with testing and
decisions made by a human.

---

## Licence

**GPL-2.0-or-later.** See [`LICENSE`](LICENSE).

The runtime derives from Dolphin (GPL-2.0-or-later) and ModernGekko, whose sources
are tagged `GPL-2.0-or-later`; DolRecomp is GPL-3.0-or-later and is used as a
separate build-time tool. Because the GPL obliges anyone receiving a binary to be
able to get the matching source, the full Dolphin and recompiler trees are vendored
in this repository rather than referenced.

`ModernGekko/LICENSE` and `ModernGekko/vendor/dolphin/COPYING` are upstream's own
terms and are left untouched.

---

## Disclaimer

An unofficial fan project. Not affiliated with, endorsed by, or connected to any of
the projects or companies named above. No game data or game code is included, and
none will be provided — do not ask. Anything setup builds from your disc is derived
from the game itself; keep it to yourself.
