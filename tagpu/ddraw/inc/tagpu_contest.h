#ifndef TAGPU_CONTEST_H
#define TAGPU_CONTEST_H
/* tagpu_contest.h -- the bookkeeping of THE CONTEST'S RULE (tagpu_puppet.c): which of the six
   sites hold what, and which a change of rule writes. Pure, so tests/codectest.c runs it on
   bytes of its own; tagpu_puppet.c owns the sites, the record of its writes and the page writes.

   A site holds the retail `je` (SITE_RETAIL), the NOP NOP this module wrote there and has not
   put back (SITE_OURS: its bit in the record of writes is set and the bytes confirm it), or
   anything else, another module's (SITE_FOREIGN) -- a NOP NOP included when the record says
   this module did not write it. */
#include <string.h>

#define CONTEST_SITES 6
enum { SITE_RETAIL, SITE_OURS, SITE_FOREIGN };

static const unsigned char CONTEST_NOP2[2] = { 0x90, 0x90 };

static inline int contest_classify(const unsigned char now[2], const unsigned char je[2],
                                   int written)
{
    if (!memcmp(now, je, 2)) return SITE_RETAIL;
    return written && !memcmp(now, CONTEST_NOP2, 2) ? SITE_OURS : SITE_FOREIGN;
}

static inline int contest_any(const int site[CONTEST_SITES], int what)
{
    unsigned i;
    for (i = 0; i < CONTEST_SITES; i++)
        if (site[i] == what) return 1;
    return 0;
}

/* The sites a change to `keep`'s rule writes, as a mask: the recording machine's rule (keep)
   NOPs every retail site and refuses (*refuse = 1, no site written) when one holds another's
   bytes, since it needs all six; the engine's rule puts back this module's NOPs alone and leaves
   another's bytes as they are. */
static inline unsigned contest_plan(int keep, const int site[CONTEST_SITES], int* refuse)
{
    unsigned i, mask = 0;
    *refuse = keep && contest_any(site, SITE_FOREIGN);
    if (*refuse) return 0;
    for (i = 0; i < CONTEST_SITES; i++)
        if (site[i] == (keep ? SITE_RETAIL : SITE_OURS)) mask |= 1u << i;
    return mask;
}

#endif
