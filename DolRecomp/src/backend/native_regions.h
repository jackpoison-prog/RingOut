#ifndef DOLRECOMP_NATIVE_REGIONS_H
#define DOLRECOMP_NATIVE_REGIONS_H
/* --native-regions <file>: listed hot guest regions run as native C (see native_regions.c). */
#include <stdbool.h>
#include "common/types.h"

/* Reads the region list ("START END" in hex per line, # comments). false = unreadable or malformed. */
bool native_regions_load(const char* path);
unsigned native_regions_count(void);
/* Forget the loaded list (tests run several lists in one process). */
void native_regions_reset(void);
/* Post-pass over the written chunk files (after --native-skin). true unless a file could not be read or written. */
bool native_regions_apply(const char* chunks_dir);
/* --region-psq: float psq_l/psq_st in regions go straight to and from the nF/nP locals (default off). */
void native_regions_set_psq_direct(bool on);
/* --region-simd: two-lane paired singles in native regions with at least 8 ps ops (default off) */
void native_regions_set_simd(bool on);
/* --inline-leaves <N>: small leaf callees (at most N instructions) inlined at their native call sites.
 * Post-pass after native_regions_apply; 0 = off. */
void native_leaves_set(unsigned max_insns);
bool native_leaves_apply(const char* chunks_dir);
#endif
