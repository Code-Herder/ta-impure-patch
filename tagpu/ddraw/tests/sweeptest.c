/* sweeptest.c -- the driver of tagpu_sweepgeo.c for tests/sweeptest.py, which holds the cases and
   the expected answers (a port of the mockup's orderSweep). One case a line on stdin, one answer
   a line on stdout:

     inside CX CZ R X Z                   -> 0 or 1
     order CX CZ N  X Z ...               -> the N indices, nearest first
     ticks E M                            -> the ticks (E and M parsed as floats)
     split NB NT  X Z SPEED REACH LOAD ROOM ...  X Z TICKS ...
                                          -> M, then M pairs WHO:SEQ, then each builder's
                                             X Z LOAD ROOM after the split (LOAD as %.17g)

   Exit 0 unless a line does not parse. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_sweepgeo.h"

#define MAXB 64

static int next_long(char** p, long long* v)
{
    char* end;
    *v = strtoll(*p, &end, 10);
    if (end == *p) return 0;
    *p = end;
    return 1;
}

static int next_double(char** p, double* v)
{
    char* end;
    *v = strtod(*p, &end);
    if (end == *p) return 0;
    *p = end;
    return 1;
}

static int line_case(char* s)
{
    long long a[6];
    double f[4];
    int i;
    if (!strncmp(s, "inside ", 7)) {
        s += 7;
        for (i = 0; i < 5; i++) if (!next_long(&s, &a[i])) return 0;
        printf("%d\n", sweep_inside((int32_t)a[0], (int32_t)a[1], (int32_t)a[2], (int32_t)a[3],
                                    (int32_t)a[4]));
        return 1;
    }
    if (!strncmp(s, "order ", 6)) {
        static SweepTarget t[SWEEP_MAX_TARGETS];
        uint16_t idx[SWEEP_MAX_TARGETS];
        long long n;
        s += 6;
        if (!next_long(&s, &a[0]) || !next_long(&s, &a[1]) || !next_long(&s, &n) || n < 0 ||
            n > (long long)SWEEP_MAX_TARGETS) return 0;
        for (i = 0; i < n; i++) {
            if (!next_long(&s, &a[2]) || !next_long(&s, &a[3])) return 0;
            t[i].x = (int32_t)a[2]; t[i].z = (int32_t)a[3]; t[i].ticks = 0;
        }
        sweep_order(t, (unsigned)n, (int32_t)a[0], (int32_t)a[1], idx);
        for (i = 0; i < n; i++) printf(i ? " %u" : "%u", (unsigned)idx[i]);
        printf("\n");
        return 1;
    }
    if (!strncmp(s, "ticks ", 6)) {
        s += 6;
        if (!next_double(&s, &f[0]) || !next_double(&s, &f[1])) return 0;
        printf("%u\n", (unsigned)sweep_reclaim_ticks((float)f[0], (float)f[1]));
        return 1;
    }
    if (!strncmp(s, "split ", 6)) {
        static SweepTarget t[SWEEP_MAX_TARGETS];
        SweepBuilder b[MAXB];
        uint16_t who[SWEEP_MAX_TARGETS], seq[SWEEP_MAX_TARGETS];
        long long nb, nt;
        unsigned m, k;
        s += 6;
        if (!next_long(&s, &nb) || !next_long(&s, &nt) || nb < 0 || nb > MAXB || nt < 0 ||
            nt > (long long)SWEEP_MAX_TARGETS) return 0;
        for (i = 0; i < nb; i++) {
            if (!next_long(&s, &a[0]) || !next_long(&s, &a[1]) || !next_double(&s, &f[0]) ||
                !next_double(&s, &f[1]) || !next_double(&s, &f[2]) || !next_long(&s, &a[2]) ||
                a[2] < 0) return 0;
            b[i].x = (int32_t)a[0]; b[i].z = (int32_t)a[1];
            b[i].speed = f[0]; b[i].reach = f[1]; b[i].load = f[2];
            b[i].room = (uint32_t)a[2];
        }
        for (i = 0; i < nt; i++) {
            if (!next_long(&s, &a[0]) || !next_long(&s, &a[1]) || !next_long(&s, &a[2]) || a[2] < 0)
                return 0;
            t[i].x = (int32_t)a[0]; t[i].z = (int32_t)a[1]; t[i].ticks = (uint32_t)a[2];
        }
        m = sweep_split(b, (unsigned)nb, t, (unsigned)nt, who, seq);
        printf("%u", m);
        for (k = 0; k < m; k++) printf(" %u:%u", (unsigned)who[k], (unsigned)seq[k]);
        for (i = 0; i < nb; i++)
            printf(" | %d %d %.17g %u", (int)b[i].x, (int)b[i].z, b[i].load, (unsigned)b[i].room);
        printf("\n");
        return 1;
    }
    return 0;
}

int main(void)
{
    static char line[1 << 20];
    while (fgets(line, sizeof line, stdin)) {
        if (!line_case(line)) {
            fprintf(stderr, "sweeptest: cannot read the case \"%.60s\"\n", line);
            return 2;
        }
    }
    return 0;
}
