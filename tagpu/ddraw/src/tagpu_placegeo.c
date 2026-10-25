/* tagpu_placegeo.c -- the shapes of a placement gesture and Build Split's shares
   (tagpu_placegeo.h). Pure arithmetic: the callers bound every input (footprint 1..64,
   spacing 0..PLACE_SPACING_MAX, cells within the map), and every product below fits int64. */
#include "tagpu_placegeo.h"

static int64_t iabs64(int64_t v) { return v < 0 ? -v : v; }

/* Floor division, b > 0. */
static int64_t fdiv(int64_t a, int64_t b)
{
    int64_t q = a / b;
    return (a % b != 0 && a < 0) ? q - 1 : q;
}

static unsigned put(PlaceCell* out, unsigned n, unsigned cap, int64_t cx, int64_t cz)
{
    if (n >= cap) return n;
    out[n].cx = (int16_t)cx;
    out[n].cz = (int16_t)cz;
    return n + 1;
}

/* Recoil's (|delta| + 1.4 size) / size, with the delta and the pitch in cells. */
static int64_t count_along(int64_t d, int64_t pitch)
{
    return (10 * iabs64(d) + 14 * pitch) / (10 * pitch);
}

unsigned place_shape(int kind, int fx, int fz, int spacing, PlaceCell from, PlaceCell to,
                     PlaceCell* out, unsigned cap)
{
    const int64_t px = fx + spacing, pz = fz + spacing;
    const int64_t dx = (int64_t)to.cx - from.cx, dz = (int64_t)to.cz - from.cz;
    const int64_t xnum = count_along(dx, px), znum = count_along(dz, pz);
    /* Recoil's `0 < delta ? size : -size`: a zero delta steps backwards, over one site only */
    const int64_t sx = dx > 0 ? px : -px, sz = dz > 0 ? pz : -pz;
    unsigned n = 0;
    int64_t i, j;

    if (kind == PLACE_LINE || kind == PLACE_AXIS) {
        /* The other axis follows the line's slope: the site i centres sit i pitches along the
           dominant axis and i * pitch * d_other / |d_dominant| cells along the other, snapped
           as 0x4197D0 snaps (half up) -- floor((|D| + 2 i p d) / 2|D|). */
        if (iabs64(dx) > iabs64(dz)) {
            for (i = 0; i < xnum && n < cap; i++)
                n = put(out, n, cap, from.cx + i * sx,
                        kind == PLACE_AXIS ? from.cz
                                           : from.cz + fdiv(iabs64(dx) + 2 * i * px * dz,
                                                            2 * iabs64(dx)));
        } else {
            /* dz == 0 here means dx == 0 too: znum is 1 and the one site is `from` */
            for (i = 0; i < znum && n < cap; i++)
                n = put(out, n, cap,
                        kind == PLACE_AXIS || dz == 0
                            ? from.cx
                            : from.cx + fdiv(iabs64(dz) + 2 * i * pz * dx, 2 * iabs64(dz)),
                        from.cz + i * sz);
        }
        return n;
    }

    if (kind == PLACE_RECT) {
        for (j = 0; j < znum && n < cap; j++)
            for (i = 0; i < xnum && n < cap; i++)
                n = put(out, n, cap, from.cx + ((j & 1) ? xnum - 1 - i : i) * sx,
                        from.cz + j * sz);
        return n;
    }

    if (kind == PLACE_HOLLOW) {
        if (xnum > 1 && znum > 1) {
            for (j = 0; j < znum - 1; j++)        /* down the first column */
                n = put(out, n, cap, from.cx, from.cz + (1 + j) * sz);
            for (j = 0; j < xnum - 1; j++)        /* along the far row */
                n = put(out, n, cap, from.cx + (1 + j) * sx, from.cz + (znum - 1) * sz);
            for (j = 0; j < znum - 1; j++)        /* up the far column */
                n = put(out, n, cap, from.cx + (xnum - 1) * sx, from.cz + (znum - 2 - j) * sz);
            for (j = 0; j < xnum - 1; j++)        /* back along the first row */
                n = put(out, n, cap, from.cx + (xnum - 2 - j) * sx, from.cz);
        } else if (xnum == 1) {
            for (j = 0; j < znum; j++) n = put(out, n, cap, from.cx, from.cz + j * sz);
        } else {
            for (j = 0; j < xnum; j++) n = put(out, n, cap, from.cx + j * sx, from.cz);
        }
        return n;
    }
    return 0;
}

unsigned place_ring(int fx, int fz, int tx, int tz, int tw, int th, PlaceCell* out, unsigned cap)
{
    const int64_t down = 1 + th / fz, across = 1 + tw / fx;
    unsigned n = 0;
    int64_t k;

    for (k = 0; k < down; k++)      /* right side, downwards from the top-right corner */
        n = put(out, n, cap, (int64_t)tx + tw, (int64_t)tz + k * fz);
    for (k = 0; k < across; k++)    /* bottom, leftwards from the bottom-right corner */
        n = put(out, n, cap, (int64_t)tx + tw - fx - k * fx, (int64_t)tz + th);
    for (k = 0; k < down; k++)      /* left side, upwards from the bottom-left corner */
        n = put(out, n, cap, (int64_t)tx - fx, (int64_t)tz + th - fz - k * fz);
    for (k = 0; k < across; k++)    /* top, rightwards from the top-left corner */
        n = put(out, n, cap, (int64_t)tx + k * fx, (int64_t)tz - fz);
    return n;
}

/* BAR's chunking with every cost equal: participant i keeps taking the next site while that
   moves its chunk no further from its share n * power[i] / P. With m sites taken, the next
   one is taken when |m + 1 - t| <= |m - t|, t = n * power[i] / P, i.e. (2m + 1) P <= 2 n
   power[i]. Its first site is always taken; the last participant takes what is left. A
   participant left without a site starts its followups at i * n / k. */
void place_split(const uint32_t* power, unsigned k, unsigned n, PlaceChunk* out)
{
    uint64_t total = 0;
    unsigned i, idx = 0;

    for (i = 0; i < k; i++) total += power[i];
    for (i = 0; i < k; i++) {
        unsigned m = 0;
        if (i + 1 == k) {
            m = n - idx;
        } else {
            while (idx + m < n &&
                   (m == 0 || (2 * (uint64_t)m + 1) * total <= 2 * (uint64_t)n * power[i]))
                m++;
        }
        out[i].first  = (uint16_t)idx;
        out[i].count  = (uint16_t)m;
        idx += m;
        out[i].follow = (uint16_t)(m ? idx : (uint64_t)i * n / k);
    }
}

void place_split_order(const PlaceChunk* c, unsigned n, uint16_t* out)
{
    unsigned at = 0, o;

    for (o = 0; o < c->count; o++) out[at++] = (uint16_t)(c->first + o);
    for (o = 0; o < n; o++) {
        unsigned s = (c->follow + o) % n;
        if (s >= c->first && s < (unsigned)c->first + c->count) continue;
        out[at++] = (uint16_t)s;
    }
}
