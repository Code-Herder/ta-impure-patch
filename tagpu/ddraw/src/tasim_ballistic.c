/* tasim_ballistic.c -- the ballistic pitch solver 0x49A890 (tasim_proj.h), on doubles rounded once per operation.

   WHY DOUBLES ARE THE ENGINE'S ARITHMETIC HERE. The game runs the x87 under the CRT's control word 0x27F (53-bit
   precision, round to nearest; the acos below checks the word against it), so every fadd, fsub, fmul, fdiv and fsqrt
   rounds its significand to 53 bits as an SSE2 double operation does, and no value of this solver leaves the double's
   exponent range (|dx|, |dz| < 2^31 make h^4 < 2^126 and E < 2^256). Each line below is one x87 instruction of the
   listing, in its order, with -ffp-contract=off (the Makefile's SSE2 rule for every tasim_*.c); the algebra must not
   be simplified: E = B h^4 - (h^4 g2) D is weapons.md's D' times h^4, and it rounds differently.

   THE ONE OPERATION THAT IS NOT: fpatan in the CRT's acos returns a 64-bit significand, which precision control does
   not round. The low root's angle is stored to a double at once (0x49A9D3), so it differs from the core's only where
   fpatan and ts_atan2 round to different doubles; the high root's stays on the x87 stack through its two compares and
   the first multiply (0x49AA3D..0x49AA55), so it can also differ where it lies within half an ulp of minbarrel or of
   pi/4. Both are RESIDUAL R4 (t2-spec, the owner accepted it with no tolerance); the trace names a mismatch. */
#include <math.h>
#include "tasim_proj.h"
#include "tasim_fixed.h"

/* the exe's doubles, MEASURED 2026-11-12 from pristine/TotalA.exe.pristine: pi/2 and pi/4 TRUNCATED (true pi/2 ends
   ...D18) at 0x4FDA78 (and stored inline at 0x49A9D9) and 0x4FDA80; 1/pi at 0x4FDA90; -2.0, 0.0 and 32768.0 at 0x4FDA60,
   0x4FDA68 and 0x4FDA88 */
#define PI2_T  0x1.921fb54442d11p+0     /* 0x3FF921FB54442D11 */
#define PI4_T  0x1.921fb54442d11p-1     /* 0x3FE921FB54442D11 */
#define INV_PI 0x1.45f306dc9c889p-2     /* 0x3FD45F306DC9C889 */

/* The CRT acos 0x4E67F0, DISASSEMBLED 0x4E67F0..0x4E68BA with its helpers 0x4ECA18, 0x4EC9A5 and 0x4EC947: for
   |x| < 1, fpatan(fsqrt((1 + x)(1 - x)), x), each of the three products and sums rounded at the caller's precision
   (0x4EC9A5 keeps the control word's precision bits); x = 1 answers fldz, x = -1 fldpi; |x| > 1, an infinity or a NaN
   takes the domain-error path 0x4E688F, whose answer is the indefinite NaN at 0x50E3C0 handed through _matherr. A NaN
   is a NaN to every reader below: each compare is unordered, and _ftol of one is 0. */
static double crt_acos(double x)
{
    if (!(fabs(x) < 1.0)) {
        if (x == 1.0) return 0.0;
        if (x == -1.0) return 3.14159265358979323846;
        return NAN;
    }
    return ts_atan2(sqrt((1.0 + x) * (1.0 - x)), x);
}

uint16_t ts_ballistic_pitch(int32_t dx, int32_t dy, int32_t dz, int32_t v, float minbarrel, int32_t g)
{
    /* 0x49A89F..0x49A8C0: g2 a 32-bit imul (it wraps for |g| > 46 340); h the engine's _hypot of the fild'ed dx, dz */
    const int32_t g2 = (int32_t)((uint32_t)g * (uint32_t)g);
    const double h = ts_hypot(dx, dz);
    const double ddy = (double)dy, dv = (double)v;
    const double gdy = (double)g * ddy;            /* 0x49A8D4 */
    const double h2 = h * h;                       /* 0x49A8E2 */
    const double v2 = dv * dv;                     /* 0x49A8F2 */
    const double dy2 = ddy * ddy;                  /* 0x49A8FE */
    const double m2gdy = gdy * -2.0;               /* 0x49A908 */
    const double h4 = h2 * h2;                     /* 0x49A912 */
    const double A = v2 - m2gdy;                   /* 0x49A91C fsubr: v^2 + 2 g dy */
    const double Av2 = A * v2;                     /* 0x49A930 */
    const double dy2g2 = dy2 * (double)g2;         /* 0x49A93A */
    const double D = dy2 + h2;                     /* 0x49A940 */
    const double h4g2 = h4 * (double)g2;           /* 0x49A94A */
    const double B = Av2 + dy2g2;                  /* 0x49A950 */
    const double h4g2D = h4g2 * D;                 /* 0x49A958 */
    const double Bh4 = B * h4;                     /* 0x49A95E */
    const double E = Bh4 - h4g2D;                  /* 0x49A964 */
    double R, P, D2, qhi, qlo, t1, t2, ans;
    if (!(E >= 0.0)) return TS_PITCH_NONE;         /* 0x49A966: C0, E < 0 or unordered */
    R = sqrt(E);                                   /* 0x49A977 */
    P = (v2 + gdy) * h2;                           /* 0x49A97D, 0x49A989 */
    D2 = D + D;                                    /* 0x49A985 */
    qhi = (R + P) / D2;                            /* 0x49A999, 0x49A9A3 */
    qlo = (P - R) / D2;                            /* 0x49A99D, 0x49A9A9 */
    /* 0x49A9B3..0x49AA0B: a root at or below 0, or unordered, is the truncated pi/2; else acos(sqrt(q) / v). The low
       angle (the larger cosine) is the first. */
    t1 = qhi > 0.0 ? crt_acos(sqrt(qhi) / dv) : PI2_T;
    t2 = qlo > 0.0 ? crt_acos(sqrt(qlo) / dv) : PI2_T;
    /* 0x49AA11..0x49AA53: the low angle when minbarrel < it <= pi/4, each compare taking the angle on unordered; else
       the high one when minbarrel < it <= pi/4, refusing on unordered */
    if (!((double)minbarrel >= t1) && !(t1 > PI4_T)) ans = t1;
    else if (t2 > (double)minbarrel && t2 <= PI4_T) ans = t2;
    else return TS_PITCH_NONE;
    /* 0x49AA55..0x49AA61: radians to the engine's units, the two multiplies in this order, _ftol, the word in ax */
    return (uint16_t)ts_ftol((ans * 32768.0) * INV_PI);
}
