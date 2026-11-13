#if defined(__mips__) && defined(__mips_hard_float)
/* Overrides musl's lrintf, which rounds in software and rewrites the FPU
 * control register. cvt.w.s rounds in the current mode, so the returned values
 * are identical, out-of-range and NaN included (both give 0x7fffffff). The FPU
 * exception flags can end up different; nothing in the player reads them. */
#include <math.h>

long lrintf(float x)
{
    float r;
    long i;
    __asm__("cvt.w.s %0, %1" : "=f"(r) : "f"(x));
    __asm__("mfc1 %0, %1" : "=r"(i) : "f"(r));
    return i;
}
#endif
