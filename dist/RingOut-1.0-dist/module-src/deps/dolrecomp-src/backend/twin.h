#ifndef DOLRECOMP_TWIN_H
#define DOLRECOMP_TWIN_H

#include "common/types.h"
#include <stdio.h>

/* Twin chunks: see twin.c. */
int twin_load_hot(const char* path);      /* "PC [hits]" lines, hex PC */
void twin_set_cr(int enable);         /* --twin-cr: CR fields in locals too */
int twin_set_regs(const char* list);      /* e.g. "1,2,13,28,29,30,31" */
int twin_enabled(void);
u32 twin_hot_count(void);
/* Write the cold copy and the fast copy of one emitted chunk function. */
int twin_write(FILE* out, const char* text, size_t len, u32 func_addr);

#endif
