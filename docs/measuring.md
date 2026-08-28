# Measuring things here

Most of the wrong turns in this project were not bad ideas. They were good
measurements of the wrong thing. This page is the accumulated defence.

## The harness

`.github/scripts/module-bench.sh <package> <frames> <input> <reps> <tag>=<so>…`

Runs each module over the **same emulated frames**, driven by a frame-keyed
input script, counted in **retired cycles** (`perf stat -e cycles:u`) rather
than wall time. Because every arm executes identical work, wall time *is*
comparable too, and no measurement window has to be aligned with anything.

Properties that matter, each of which exists because its absence produced a
wrong answer:

* **Reps alternate, and the order reverses on even reps.** A result that tracks
  run order rather than the module is then visible instead of averaged away.
* **Every run gets a fresh user directory** seeded from the package, so no run
  inherits state the previous one wrote.
* **A short run is discarded, not scored.** Without that check a module that
  crashes at frame 300 wins every comparison.
* **The hash column is the control.** Identical hashes mean the arms did the
  same emulated work. A layout-only change *must* match; a codegen change need
  not, but a changed hash means the comparison is measuring different content.

## What the workload has to be

`.github/input-scripts/arcade-match.txt` drives an actual Arcade match. Frames
before ~4500 are boot and menus, so use a frame count where gameplay dominates.

Three workloads that were used before this one and were all wrong:

* **Boot and an idle menu.** Carries ~0.1% of a session's paired-single traffic.
  A change can look free there and cost 3% in a match.
* **Attract mode.** Reaches gameplay, but is *not reproducible across launches*
  — two runs of the same package put the intro movie 33 s apart and played the
  attract items in a different order, so a fixed window lands on different
  content each time.
* **A "reproducible" screen that was a dialog.** GX draw calls flat at 32/frame
  gave the game away. See the draw-call check below.

## Prove the run did what you think

The single most expensive recurring mistake here: a run finishes, the result
looks clean, and it is about *something else*. A day of per-stage analysis once
went into a route that never left the first fight.

Cheap checks, in the order they are worth doing:

* **Frames actually hashed** vs frames requested.
* **`RINGOUT_GX_STATS=<n>`** prints mean per-frame GX counters to stderr. This
  is how you tell gameplay from a menu: **~1–3 draw calls per frame parked on a
  dialog, 84–260 in a match.** A profile, a benchmark or a training run that
  never left a dialog looks entirely normal without it.
* **Save data exists.** No save means the game parks on a memory-card dialog and
  the whole run is wasted. Packages ship none by design.
* **The harness is armed.** `RINGOUT_DETERMINISM_LOG` enables the frame-keyed
  input *and* the frame limit; neither works without it. `_NOHASH=1` turns it
  into a performance harness — input and frame counter kept, per-frame hash
  dropped (the hash is ~29% of cycles and will dominate a profile).

## Machine noise

Measure on an idle machine, and say which machine.

A build-time table taken on a loaded desktop had every row inflated — the same
clean build read 3m04s–3m24s there against 144s idle — and the noise had
*structure*: it looked like a 7% linker win, then a 33% one, depending which
pair you compared. If a measurement disagrees with a recorded figure by tens of
seconds, suspect the machine before the code.

Observed run-to-run spread on the gameplay benchmark, idle:

| machine | spread over 6–8 reps |
| --- | --- |
| desktop, Zen 3 | 0.7–1.8% |
| Steam Deck, Zen 2 | 0.5–0.8% |

Report `n`, min, max and spread alongside the mean, and say whether the arms'
ranges overlap. A 1% difference between two arms whose ranges overlap is not a
result; a 14% difference with a clean gap is.

## Instrument choice

* **Retired cycles** for "is this faster over fixed work".
* **Instructions** as the less noisy companion — it moves for codegen changes
  and barely moves for scheduling noise.
* **Guest-level attribution** (`-g1` plus `hot-guest-code.sh`) for "where does
  the time go inside the recompiled module".
* **PMU attribution** for "is this front-end, back-end or dispatch bound".
  Established here: front-end starvation 2.4% of cycles, indirect mispredicts
  1.3%, back end saturated. Only codegen volume is left.

Profiling with the determinism hash left on once put 28.6% of cycles in
`crc32_fold_pclmulqdq` — the harness measuring itself. `RINGOUT_DETERMINISM_NOHASH=1`.

## When a check is added, break it on purpose

Every guard in the build scripts was mutation-tested: reintroduce the bug and
confirm the check fails. A guard that has never failed is not known to work —
the launcher's glibc floor and the packaging assertions were both verified this
way, and the PGO draw-call guard proved itself by catching a real bug on its
first run.
