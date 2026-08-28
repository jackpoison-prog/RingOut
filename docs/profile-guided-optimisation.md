# Profile-guided optimisation

PGO is the single largest win in this project that is not a code change:
**10–14% of CPU time**, depending on the machine. This page is the reference for
how it works here, what has been measured, and the several ways it silently
does nothing.

For *how to use it*, see the package `README.txt` (`./setup.sh --pgo`) and
`dist/RingOut-1.0-deck/BUILD-ON-THE-DECK.md` section 2b. This page is the why.

## What it is worth

Measured on the fixed-work gameplay benchmark (`.github/scripts/module-bench.sh`,
6000 frames of `arcade-match.txt`), alternating arms with the order reversed on
even reps. The hash column proves both arms executed identical emulated work.

| machine | arms | result |
| --- | --- | --- |
| desktop, Zen 3, 12 threads | unprofiled 176.95 → shipped profile 158.66 Gcyc | **−10.34%** |
| desktop, Zen 3, 12 threads | unprofiled 176.95 → self-trained 156.72 Gcyc | **−11.44%** |
| Steam Deck, Zen 2, 8 threads | unprofiled 233.99 → self-trained 201.07 Gcyc | **−14.07%** |

The Deck run: 8 reps an arm, spread 0.5–0.8%, ranges not overlapping (the
profiled arm's worst run beat the unprofiled arm's best), IPC 2.005 → 2.136,
wall clock 75.5s → 68.3s, all 16 runs one hash.

An earlier revision of the top-level README listed PGO under *measured and
rejected* — "does not build, SIGBUS before the first frame". That was a real
failure at the time and it is long fixed; the row is now wrong and has been
moved to what shipped.

## A shipped profile cannot serve every machine

Two independent reasons, and the first one is fatal on most Linux installs:

1. **Format version.** An indexed profile can only be read by an LLVM at least
   as new as the one that wrote it. The profiles in `module-src/profiles/` are
   written by LLVM 22, so clang 20 (SteamOS 3.8, Fedora), 18 (Ubuntu 24.04) and
   14 (Debian 12) all refuse them with *"unsupported instrumentation profile
   format version"*. Confirmed on a stock Steam Deck rather than assumed:
   `MODULE_PGO_USABLE` came back empty in its CMakeCache and the module built
   23 MB instead of 39 MB.
2. **CFG hashes.** A profile keys its counts to a hash the *instrumenting*
   compiler computed for each function. Where two clang versions build a
   different CFG for the same function, the counts for it are dropped — and
   silently, because `-Wprofile-instr-out-of-date` is suppressed in this build
   (unprofiled chunks are expected and would otherwise bury the log). A real
   user's log shows exactly this: `function control flow change detected (hash
   mismatch) mem_write64_slow … count discarded`.

### Function names carried a path, and no flag fixed it

LLVM keys an **internal-linkage** function as `<source path>;<symbol>`, using
the path as the compiler received it — which CMake passes absolute. So three
functions (`chassis_dispatch`, `chassis_on_state_loaded`, `resolve_addr`) were
recorded under the *training* machine's path, matched nothing on anyone else's,
and silently got no profile data at all. The same strings were the last
identifying data in the repository.

`-ffile-prefix-map` does **not** fix it, though it looks like it should: its
documented scope is debug info, coverage mapping, preprocessor macros and
`__builtin_FILE()`, and there is no `-fprofile-prefix-map`. Verified rather than
assumed — a full retrain with the flag confirmed present in `build.ninja` still
carried the path, and one file compiled both ways gives `sub/t.c;helper`
relative against the full path absolute.

The fix is **external linkage** for those three, making the key the bare symbol,
which is identical everywhere. Measured on a Steam Deck, 8 reps an arm, both
arms already having full profile coverage so nothing masked a regression:

| arm | mean Gcyc | spread |
| --- | --- | --- |
| `static` | 201.18 | 0.3% |
| external | 201.50 | 0.5% |

**+0.16% cycles, +0.03% instructions**, ranges overlapping — free, because
ThinLTO with hidden visibility still inlines them. All three shipped profiles
were retrained afterwards and carry no path at all.

`setup.sh --pgo` exists because of this: it trains a profile with the compiler
that is about to do the build. That profile also came out **1.22% ahead** of the
shipped one on the desktop A/B, ranges not overlapping — so it is the better
profile, not a degraded fallback.

## Profiles are per disc, not per CPU

* **Per region matters.** Training a JP module on a JP profile gave −12.33%
  against the shipped US profile, because the US profile matches 1 of that
  module's 130 chunks. `setup.sh` picks `profiles/$DISC_ID.profdata` and
  deliberately does **not** fall back to another region's — measured, that is
  worse than building with none.
* **Per CPU does not.** A desktop-trained profile gives −12.55% on the Deck's
  Zen 2 with no retraining. Retrain when the *emitter* changes, not when the
  host CPU does.
* SC2 Plus (`GRSEPS`) is deliberately given the US profile: it appends its code
  and hooks the base text in place, so its chunks are the US disc's chunks.

## The training set: smaller won

Twice-measured and counter-intuitive. Broadening the training route made the
result **worse** (+2.18% and +1.26%), because weight moved to code the player
does not run. Menus are not a substitute for gameplay either — they carry ~0.1%
of a session's paired-single traffic. One 6000-frame arcade match is the shape
that wins.

Context-sensitive PGO (CS-PGO) was measured as worth nothing, twice.

## How it silently does nothing

Each of these produces a build that looks successful:

* **A profile that reads as all-cold.** `llvm-profdata` will happily write a
  structurally valid profile from a run that collected nothing useful, and
  `-fprofile-use` accepts it. The result is *worse than no profile*. The module
  CMakeLists refuses a profile whose total count is zero for this reason.
* **A training run that never reached gameplay.** A run parked on the
  memory-card dialog still exits cleanly and still writes a valid profile.
  `--pgo` therefore checks `RINGOUT_GX_STATS` and refuses to rebuild below 10
  draw calls per frame. Measured on the same module and route: **2.4 draws/frame
  parked, 84.1 playing**. This guard caught a real bug on its first run.
* **No save data.** A package ships no memory card by design, and without one
  the route never leaves that dialog. Play once before `--pgo`; the save lands
  in `userdata/GC/` and is picked up automatically.
* **An unarmed harness.** `RINGOUT_DETERMINISM_LOG` is what *arms* the
  determinism harness — the frame-keyed input and the frame limit are both inert
  without it. Setting only `_FRAMES` and `_INPUT` gives a run that receives no
  input and never stops. Pair it with `_NOHASH=1` for training: that keeps the
  input and the counter but drops the per-frame hash, which is ~29% of cycles
  and would otherwise spend the profile's weight on the harness's own crc32.
* **Instrumentation with LTO on.** Instrumented objects are LTO IR; a non-LTO
  link produces a 21 KB stub that loads and collects nothing. `--pgo` passes
  `-DMODULE_LTO=OFF` for the instrumented build only.

Because none of these announce themselves, `setup.sh` prints an explicit verdict
at the end saying whether the module is profiled, and if not, why and what it
costs. That state is read back from CMake's own configure output — passing
`-DMODULE_PGO_PROFILE` proves nothing, since most clangs accept the flag and
then refuse the file.

## Cost

| | desktop | Steam Deck |
| --- | --- | --- |
| plain build | ~8 min | 7m 24s |
| whole `--pgo` run | 19 min | 33m 53s |
| added by `--pgo` | ~11 min | ~26 min |

Module size roughly doubles, 23 MB → 39 MB. It changes speed only: 18 benchmark
runs, profiled and not, produced one identical guest-RAM hash.
