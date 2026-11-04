#pragma once
/* no CPUID off x86: the game sees a CPU without the instruction's answers (zeros) */
static inline void __cpuid(int r[4], int leaf)
{
    (void)leaf;
    r[0] = r[1] = r[2] = r[3] = 0;
}
