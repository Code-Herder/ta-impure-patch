/* tagpu_wikitech -- the tech tree entry's tree (tagpu_wikitech.h): techtree.js `model` and
   `layout`, rule for rule. */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "tagpu_wikitech.h"

double tagpu_wikitech_card_h(uint32_t n)
{
    return WT_HEAD + (n ? (double)((n + WT_PER - 1) / WT_PER) * (WT_ICON + WT_IGAP) + WT_IGAP : 0.0) + WT_FOOT;
}

/* techtree.js isUpgrade: what a builder makes that never stands on the field as a unit of its own --
   a non-structure that is 0 x 0, or born cloaked, stealthed and hiding its damage */
static int is_upgrade(const WIKITECHUNIT* u)
{
    return !u->structure && ((u->fx == 0 && u->fz == 0) || (u->cloaked && u->stealth && u->hidedamage));
}

/* /upgrad/i over the description's bytes */
static int says_upgradable(const unsigned char* d)
{
    static const char W[] = "upgrad";
    size_t i, k;
    for (i = 0; d[i]; i++) {
        for (k = 0; k < 6 && d[i + k] && (d[i + k] | 0x20) == W[k]; k++) {}
        if (k == 6) return 1;
    }
    return 0;
}

void tagpu_wikitech_free(WIKITECHTREE* tr)
{
    free(tr->kind); free(tr->upgrade); free(tr->upgradable); free(tr->tier); free(tr->first);
    free(tr->builds); free(tr->upgrades); free(tr->parents); free(tr->unlocks); free(tr->icons);
    free(tr->pool); free(tr->spine); free(tr->sp); free(tr->card); free(tr->pos);
    memset(tr, 0, sizeof *tr);
}

int tagpu_wikitech_tree(const WIKITECH* t, int side, WIKITECHTREE* out)
{
    WIKITECHTREE T;
    const WIKITECHSIDE* sd;
    int32_t *oi = NULL, *kids = NULL, *kid_at = NULL, *kid_n = NULL, *col = NULL, *stack = NULL, *next = NULL;
    double* y0 = NULL;
    uint8_t* seen = NULL;
    uint32_t i, q, at = 0, cap;
    int n, ok = 0;
    memset(&T, 0, sizeof T);
    memset(out, 0, sizeof *out);
    if (!t || side < 0 || side >= t->nside) return 0;
    sd = &t->side[side];
    n = (int)sd->norder;
    if (n < 1 || sd->first + sd->norder > t->norder) return 0;
    T.t = t;
    T.side = side;
    T.n = n;
    T.unit = t->order + sd->first;
    /* BOUND: every list is drawn from the side's menus, each entry once a list: builds, upgrades,
       parents and unlocks hold at most the menus' entries each, the icons at most a card's unlocks
       and upgrades */
    cap = 0;
    for (i = 0; i < (uint32_t)n; i++) cap += t->mcount[sd->first + i];
    cap = 5 * cap + 1;
    if (!(T.kind = calloc((size_t)n, 1)) || !(T.upgrade = calloc((size_t)n, 1)) || !(T.upgradable = calloc((size_t)n, 1)) ||
        !(T.tier = calloc((size_t)n, 1)) || !(T.first = malloc((size_t)n * sizeof *T.first)) ||
        !(T.builds = calloc((size_t)n, sizeof *T.builds)) || !(T.upgrades = calloc((size_t)n, sizeof *T.upgrades)) ||
        !(T.parents = calloc((size_t)n, sizeof *T.parents)) || !(T.unlocks = calloc((size_t)n, sizeof *T.unlocks)) ||
        !(T.icons = calloc((size_t)n, sizeof *T.icons)) || !(T.pool = malloc((size_t)cap * sizeof *T.pool)) ||
        !(T.spine = malloc((size_t)n * sizeof *T.spine)) || !(T.sp = malloc((size_t)n * sizeof *T.sp)) ||
        !(T.card = malloc((size_t)n * sizeof *T.card)) || !(oi = malloc((size_t)(t->nunit ? t->nunit : 1) * sizeof *oi)) ||
        !(seen = calloc((size_t)n, 1)))
        goto done;
    for (i = 0; i < (uint32_t)t->nunit; i++) oi[i] = -1;
    for (i = 0; i < (uint32_t)n; i++) {
        if (T.unit[i] >= t->nunit) goto done;
        oi[T.unit[i]] = (int32_t)i;
    }
    /* each menu entry as an order index: a menu holds only walked units, so every one has one */
    for (i = 0; i < (uint32_t)n; i++) {
        const uint32_t a = sd->first + i;
        if (t->mfirst[a] + t->mcount[a] > t->nmenu) goto done;
        for (q = 0; q < t->mcount[a]; q++)
            if (t->menu[t->mfirst[a] + q] >= t->nunit || oi[t->menu[t->mfirst[a] + q]] < 0) goto done;
    }
#define MENU(i, q) oi[t->menu[t->mfirst[sd->first + (i)] + (q)]]
#define MCOUNT(i) t->mcount[sd->first + (i)]
    for (i = 0; i < (uint32_t)n; i++) T.upgrade[i] = (uint8_t)is_upgrade(&t->unit[T.unit[i]]);
    /* parents: every builder whose menu holds it, in walk order, each once */
    for (i = 0; i < (uint32_t)n; i++) {
        uint32_t c = 0, j;
        T.parents[i].at = at;
        for (j = 0; j < (uint32_t)n; j++)
            for (q = 0; q < MCOUNT(j); q++)
                if (MENU(j, q) == (int32_t)i) { T.pool[at++] = (int32_t)j; c++; break; }
        T.parents[i].n = c;
    }
    /* first: the builder the walk reached it from */
    for (i = 0; i < (uint32_t)n; i++) T.first[i] = -1;
    seen[0] = 1;
    for (i = 0; i < (uint32_t)n; i++)
        for (q = 0; q < MCOUNT(i); q++) {
            const int32_t x = MENU(i, q);
            if (!seen[x]) { seen[x] = 1; T.first[x] = (int32_t)i; }
        }
    /* the kind (techtree.js kindOf) and the menu split */
    for (i = 0; i < (uint32_t)n; i++) {
        const WIKITECHUNIT* u = &t->unit[T.unit[i]];
        uint32_t real = 0;
        for (q = 0; q < MCOUNT(i); q++) real += !T.upgrade[MENU(i, q)];
        if (i == 0) T.kind[i] = WT_COMMANDER;
        else if (T.upgrade[i]) T.kind[i] = WT_UPGRADE;
        else if (u->structure && real) T.kind[i] = WT_FACTORY;
        else if (!u->structure && u->builder && real) T.kind[i] = WT_BUILDER;
        else if (u->structure) {
            if (u->extracts > 0 || u->energymake > 0 || u->metalmake > 0) T.kind[i] = WT_ECONOMY;
            else if (u->nweapon) T.kind[i] = WT_DEFENSE;
            else T.kind[i] = u->mstore > 0 || u->estore > 0 ? WT_ECONOMY : WT_SUPPORT;
        } else T.kind[i] = u->canfly ? WT_AIR : u->water ? WT_SEA : WT_LAND;
        T.builds[i].at = at;
        for (q = 0; q < MCOUNT(i); q++) if (!T.upgrade[MENU(i, q)]) T.pool[at++] = MENU(i, q);
        T.builds[i].n = at - T.builds[i].at;
        T.upgrades[i].at = at;
        for (q = 0; q < MCOUNT(i); q++) if (T.upgrade[MENU(i, q)]) T.pool[at++] = MENU(i, q);
        T.upgrades[i].n = at - T.upgrades[i].at;
        T.unlocks[i].at = at;
        for (q = 0; q < T.builds[i].n; q++)
            if (T.first[T.pool[T.builds[i].at + q]] == (int32_t)i) T.pool[at++] = T.pool[T.builds[i].at + q];
        T.unlocks[i].n = at - T.unlocks[i].at;
        T.upgradable[i] = (uint8_t)(!T.upgrade[i] && (T.upgrades[i].n > 0 || says_upgradable(u->desc)));
    }
#undef MENU
#undef MCOUNT
    /* the tier: the category's LEVELn, else one more than the builders crossed from the commander */
    for (i = 0; i < (uint32_t)n; i++) {
        const int lv = t->unit[T.unit[i]].level;
        int tier = 1, p, steps = 0;
        if (lv >= 0) { T.tier[i] = (uint8_t)lv; continue; }
        for (p = T.first[i]; p > 0 && steps < n; p = T.first[p], steps++) if (T.kind[p] == WT_BUILDER) tier++;
        T.tier[i] = (uint8_t)(tier < 4 ? tier : 4);
    }
    /* the spine and each card's parent card */
    for (i = 0; i < (uint32_t)n; i++) {
        T.card[i] = -1;
        T.sp[i] = -1;
        if (T.kind[i] == WT_COMMANDER || T.kind[i] == WT_BUILDER || T.kind[i] == WT_FACTORY) {
            T.card[i] = T.nspine;
            T.spine[T.nspine++] = (int32_t)i;
        }
    }
    for (i = 0; i < (uint32_t)T.nspine; i++) {
        int32_t p = T.first[T.spine[i]];
        int steps = 0;
        while (p >= 0 && T.card[p] < 0 && steps++ < n) p = T.first[p];
        T.sp[T.spine[i]] = p;
    }
    /* a card's icons: its unlocks that are no card, then its upgrades */
    for (i = 0; i < (uint32_t)n; i++) {
        T.icons[i].at = at;
        if (T.card[i] >= 0) {
            for (q = 0; q < T.unlocks[i].n; q++) if (T.card[T.pool[T.unlocks[i].at + q]] < 0) T.pool[at++] = T.pool[T.unlocks[i].at + q];
            for (q = 0; q < T.upgrades[i].n; q++) T.pool[at++] = T.pool[T.upgrades[i].at + q];
        }
        T.icons[i].n = at - T.icons[i].at;
    }

    /* THE LAYOUT (techtree.js layout): a tidy tree, one cursor down the whole of it. Each childless
       card takes the next row; a parent sits centred over its children but never above the cursor;
       every card starts at or below the cursor and moves it past its bottom plus WT_GAP, so two
       cards of one column are WT_GAP apart or more. Walked with a stack of its own, not by
       recursion, as a card's children come in spine order. */
    {
        const int ns = T.nspine;
        double y = 0;
        int sp_top = 0;
        if (!(T.pos = calloc((size_t)ns, sizeof *T.pos)) || !(kids = malloc((size_t)ns * sizeof *kids)) ||
            !(kid_at = calloc((size_t)ns, sizeof *kid_at)) || !(kid_n = calloc((size_t)ns, sizeof *kid_n)) ||
            !(col = calloc((size_t)ns, sizeof *col)) || !(stack = malloc((size_t)ns * sizeof *stack)) ||
            !(next = calloc((size_t)ns, sizeof *next)) || !(y0 = calloc((size_t)ns, sizeof *y0)))
            goto done;
        for (i = 0; i < (uint32_t)ns; i++) {
            const int32_t p = T.sp[T.spine[i]];
            if (p >= 0) kid_n[T.card[p]]++;
        }
        for (i = 1; i < (uint32_t)ns; i++) kid_at[i] = kid_at[i - 1] + kid_n[i - 1];
        memset(kid_n, 0, (size_t)ns * sizeof *kid_n);
        for (i = 0; i < (uint32_t)ns; i++) {
            const int32_t p = T.sp[T.spine[i]];
            if (p >= 0) { const int c = T.card[p]; kids[kid_at[c] + kid_n[c]++] = (int32_t)i; col[i] = col[c] + 1; }
        }
        stack[sp_top++] = 0;                                    /* the commander's card */
        while (sp_top) {
            const int c = stack[sp_top - 1];
            const double hc = tagpu_wikitech_card_h(T.icons[T.spine[c]].n);
            double top;
            if (next[c] == 0 && kid_n[c]) y0[c] = y;
            if (next[c] < kid_n[c]) {
                if (sp_top >= ns) goto done;                    /* a cycle: a card is its own ancestor */
                stack[sp_top++] = kids[kid_at[c] + next[c]++];
                continue;
            }
            if (!kid_n[c]) top = y;
            else {
                const double* a = T.pos[kids[kid_at[c]]];
                const double* b = T.pos[kids[kid_at[c] + kid_n[c] - 1]];
                const double mid = (a[1] + b[1] + b[3]) / 2 - hc / 2;
                top = y0[c] > mid ? y0[c] : mid;
            }
            T.pos[c][0] = col[c] * WT_PITCH;
            T.pos[c][1] = top;
            T.pos[c][2] = WT_W;
            T.pos[c][3] = hc;
            if (top + hc + WT_GAP > y) y = top + hc + WT_GAP;
            sp_top--;
        }
        for (i = 0; i < (uint32_t)ns; i++) if (T.pos[i][0] + WT_W > T.w) T.w = T.pos[i][0] + WT_W;
        T.h = y - WT_GAP > 0 ? y - WT_GAP : 0;
    }
    ok = 1;
done:
    free(oi); free(seen); free(kids); free(kid_at); free(kid_n); free(col); free(stack); free(next); free(y0);
    if (!ok) { tagpu_wikitech_free(&T); return 0; }
    *out = T;
    return 1;
}

/* ---- THE CAMERA AND THE HIT TEST (techtree.js fit, hit) ---- */

void tagpu_wikitech_fit(const WIKITECHTREE* T, double cw, double ch, double s, WTCAM* out)
{
    const double a = T->w > 0 ? (cw - 2 * WT_MARGIN * s) / T->w : 1e-3, b = T->h > 0 ? (ch - 2 * WT_MARGIN * s) / T->h : 1e-3;
    double z = a < b ? a : b;
    if (!(z > 1e-3)) z = 1e-3;
    out->z = z;
    out->x = T->w / 2 - cw / 2 / z;
    out->y = T->h / 2 - ch / 2 / z;
}

int tagpu_wikitech_hit(const WIKITECHTREE* T, double px, double py, int* unit, int* card, int* icon)
{
    int c;
    for (c = 0; c < T->nspine; c++) {
        const double* p = T->pos[c];
        const int k = T->spine[c];
        const WTLIST ic = T->icons[k];
        const double ix = px - p[0] - WT_PAD, iy = py - p[1] - WT_HEAD - WT_IGAP, step = WT_ICON + WT_IGAP;
        if (px < p[0] || px >= p[0] + p[2] || py < p[1] || py >= p[1] + p[3]) continue;
        *card = k;
        if (iy >= 0 && ix >= 0) {
            const double col = floor(ix / step), row = floor(iy / step);
            const double i = row * WT_PER + col;
            if (col < WT_PER && fmod(ix, step) < WT_ICON && fmod(iy, step) < WT_ICON && i < ic.n) {
                *unit = T->pool[ic.at + (uint32_t)i];
                *icon = 1;
                return 1;
            }
        }
        *unit = k;
        *icon = 0;
        return 1;
    }
    return 0;
}

/* code-page bytes as UTF-8, each byte the code point of its value (the mockup's text, which gfx.py
   reads as Latin-1); `cp1252` takes a title's rule instead: Windows-1252, '?' for a byte with no
   character there (gfx.py display_name) */
void tagpu_wikitech_utf8(char* d, size_t n, const unsigned char* s, int cp1252)
{
    static const uint16_t HI[32] = { 0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
        0x2039, 0x0152, 0, 0x017D, 0, 0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161,
        0x203A, 0x0153, 0, 0x017E, 0x0178 };
    size_t o = 0;
    for (; *s && o + 4 < n; s++) {
        unsigned v = *s;
        if (cp1252 && v >= 0x80 && v < 0xA0) v = HI[v - 0x80] ? HI[v - 0x80] : '?';
        if (v < 0x80) d[o++] = (char)v;
        else if (v < 0x800) { d[o++] = (char)(0xC0 | v >> 6); d[o++] = (char)(0x80 | (v & 0x3F)); }
        else { d[o++] = (char)(0xE0 | v >> 12); d[o++] = (char)(0x80 | (v >> 6 & 0x3F)); d[o++] = (char)(0x80 | (v & 0x3F)); }
    }
    d[o] = 0;
}

static const char* const KIND_NAME[WT_KINDS] = { "Commander", "Factory", "Builder", "Economy", "Defense", "Support",
                                                 "Land", "Air", "Sea", "Upgrade" };

const char* tagpu_wikitech_kind_name(int kind)
{
    return kind >= 0 && kind < WT_KINDS ? KIND_NAME[kind] : "";
}

int tagpu_wikitech_home(const WIKITECHTREE* T, int k)
{
    int steps = 0;
    if (k < 0 || k >= T->n) return -1;
    while (k >= 0 && T->card[k] < 0 && steps++ < T->n) k = T->first[k];
    return k >= 0 && T->card[k] >= 0 ? k : -1;
}

int tagpu_wikitech_path(const WIKITECHTREE* T, int k, int32_t* out, int max)
{
    int n = 0, x;
    /* `first` is acyclic (the walk is breadth-first), so the steps are at most the walk's length */
    if (k < 0 || k >= T->n) return 0;
    for (x = k; x >= 0 && x < T->n && n < T->n; x = T->first[x]) n++;
    if (n > max) return -1;
    for (x = k; n > 0 && x >= 0; x = T->first[x]) out[--n] = x;
    for (x = k, n = 0; x >= 0 && x < T->n && n < T->n; x = T->first[x]) n++;
    return n;
}
