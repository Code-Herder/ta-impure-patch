#ifndef TAGPU_ADDR_H
#define TAGPU_ADDR_H

#include <windows.h>

/* THE TOP OF THIS PROCESS'S OWN ADDRESS SPACE, the upper end of every range filter on a pointer
   read out of engine memory (the passes' `ptr_ok`).

   It is the process's, not a constant. A 32-bit exe gets 2 GB (0x7FFEFFFF) unless its PE header
   sets IMAGE_FILE_LARGE_ADDRESS_AWARE; one that sets it gets nearly 4 GB (0xFFFEFFFF) under a
   64-bit Windows or Wine, and its heap is placed up there. Retail 3.1 does not set it
   (Characteristics 0x10B); TA:ESC's exe does (0x12B, research/notes/deep-ta-esc.md), and its
   unit array starts at 0x9FD50020 (MEASURED 2026-09-27, a skirmish on Wine) -- a 2 GB constant
   read that as garbage and every pass skipped every unit.

   The filters stay what CLAUDE.md allows them to be: a cheap test of a VALUE, never the reason a
   read is safe. Every caller computes the same number, so two threads racing the first call
   store the same aligned word. */
static __inline size_t tagpu_user_top(void)
{
    static volatile size_t top;
    size_t t = top;
    if (!t) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        t = (size_t)si.lpMaximumApplicationAddress;
        top = t;
    }
    return t;
}

#endif
