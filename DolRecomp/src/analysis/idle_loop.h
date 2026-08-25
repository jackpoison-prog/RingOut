#ifndef DOLRECOMP_ANALYSIS_IDLE_LOOP_H
#define DOLRECOMP_ANALYSIS_IDLE_LOOP_H

#include "common/types.h"
#include "frontend/container/dol.h"

/* Finds the OS idle spin loop in a GameCube DOL.
 *
 * Back-edges to that loop must stay dispatcher returns (--idle-pc) or the host
 * never sees the guest idling and idle-skip stops working -- which is worth
 * about half the frame rate, silently. The address is per-build, so hard-coding
 * one disc's value gets it wrong for every other region; this matches the loop
 * by shape instead:
 *
 *     lwz    rD, d(r13)      ; the scheduler's wake flag, via the small-data base
 *     cmplwi rD, 0
 *     beq-   -8              ; back to the lwz
 *
 * Three-instruction spins of that shape appear a handful of times in a DOL, but
 * only the scheduler's reads its flag through r13, which is what separates it.
 *
 * Returns 1 and writes the address when exactly one candidate is found; returns
 * 0 (leaving *out_pc alone) when there is none or more than one, since guessing
 * between them would be worse than the extra dispatcher return costs.
 */
int dol_find_idle_pc(const DOLFile* dol, u32* out_pc);

#endif /* DOLRECOMP_ANALYSIS_IDLE_LOOP_H */
