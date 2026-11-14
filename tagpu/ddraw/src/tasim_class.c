/* tasim_class.c -- the engine's pathing maps: the per-cell rule, the fill, the refresh with its ring, the re-mark,
   the claim, the search's watermark and its cleanup (tasim_class.h). Every function is a port of the engine function
   it names, in its order of operations, DISASSEMBLED from pristine/TotalA.exe.pristine (objdump -d -M intel). */
#include <string.h>
#include "tasim_class.h"

#define AL4(n) (((n) + 3u) & ~(size_t)3u)

static size_t map_words(int32_t w, int32_t h) { return (size_t)((h + 15) >> 4) * (size_t)w; }

/* the fill's line: max(W + fx, H + fz) + 3 bytes, two before the line and fx + 1 (fz + 1) zeros after it
   (0x440509..0x440531) */
static size_t line_bytes(int32_t w, int32_t h, int32_t fx, int32_t fz)
{
    const int64_t a = (int64_t)w + fx, b = (int64_t)h + fz;
    return (size_t)((a > b ? a : b) + 3);
}

size_t ts_class_bytes(const TASIM_MOVECLASS cls[TS_CLASSES], int32_t w, int32_t h)
{
    size_t n = 0, line = 0;
    int i;
    if (w <= 0 || h <= 0) return 0;
    for (i = 0; i < TS_CLASSES; i++) {
        size_t l;
        if (!cls[i].present) continue;
        if (cls[i].footx < 0 || cls[i].footz < 0) return 0;
        n += 4 * map_words(w, h);
        l = line_bytes(w, h, cls[i].footx, cls[i].footz);
        if (l > line) line = l;
    }
    return n + AL4(line);
}

int ts_class_init(TS_CLASSMAPS* cm, void* mem, size_t bytes, const TASIM_MOVECLASS cls[TS_CLASSES], const TS_MAP* m,
                  TS_CUNITS* units, int* bad)
{
    unsigned char* p = (unsigned char*)mem;
    int i;
    *bad = -1;
    for (i = 0; i < TS_CLASSES; i++)
        if (cls[i].present && (cls[i].footx < 0 || cls[i].footz < 0)) {
            *bad = i;
            return TS_CLASS_FOOT;
        }
    if (!mem || bytes < ts_class_bytes(cls, m->w, m->h)) return TS_CLASS_ROOM;
    memset(cm, 0, sizeof *cm);
    memset(mem, 0, ts_class_bytes(cls, m->w, m->h));
    cm->map = m;
    cm->units = units;
    cm->w = m->w;
    cm->h = m->h;
    cm->rows16 = (m->h + 15) >> 4;
    for (i = 0; i < TS_CLASSES; i++) {
        TS_CLASSMAP* c = &cm->c[i];
        if (!cls[i].present) continue;
        c->present = 1;
        c->fx = cls[i].footx;
        c->fz = cls[i].footz;
        c->maxwaterdepth = cls[i].maxwaterdepth;
        c->minwaterdepth = cls[i].minwaterdepth;
        c->maxslope = cls[i].maxslope;
        c->badslope = cls[i].badslope;
        c->maxwaterslope = cls[i].maxwaterslope;
        c->badwaterslope = cls[i].badwaterslope;
        c->map = (uint32_t*)p;
        p += 4 * map_words(m->w, m->h);
    }
    cm->line = p;
    return TS_CLASS_OK;
}

/* THE FEATURE RULE the five readers share (map.md, The cell): an empty word is free; a definition blocks by its
   +0xFE bit 6, and one at or past the count blocks (0x47DE8D); 0xFFFB..0xFFFD block; a footprint cell is its
   anchor's, an anchor word from 0xFFFB up being free. THE BOUNDS: the anchor's definition is read without the count
   test in the engine (0x47DED0 goes straight to 0x47DED5), and its cell is wherever the back-reference points; the
   core takes a definition at or past the count, or an anchor before the grid, as blocking, as the direct test does. */
static int feature_blocks(const TS_MAP* m, int32_t idx)
{
    uint16_t w = m->fdef[idx];
    int32_t a;
    if (w == TS_F_NONE) return 0;
    if (w < TS_F_MARK) return w >= m->nfd ? 1 : (m->fd[w].flags >> 6) & 1;
    if (w != TS_F_FOOT) return 1;
    a = idx - ((int32_t)(m->back[idx] & 0xFF) * m->w + (int32_t)(m->back[idx] >> 8));
    if (a < 0) return 1;
    w = m->fdef[a];
    if (w >= TS_F_MARK) return 0;
    return w >= m->nfd ? 1 : (m->fd[w].flags >> 6) & 1;
}

/* THE UNIT RULE (0x47DEF8..0x47DF32, 0x47E10C..0x47E13F): a unit in slot A blocks when it has no mover -- a building
   -- or when its claim stamp is below the class's watermark (unsigned): it has stood still since before the search
   that set it. Slot B, the air layer's, is not read. THE BOUND: the slot against the units' count; the engine
   indexes its unit array with the word unchecked. A slot past the view, or any slot with no view, blocks. */
/* THE BOUND on a class index: the engine's comes from a unit's definition, a pointer into the 32 records (DEF
   +0x1B6); the core's from the definition's class index, which a later caller hands over as data. */
static int class_ok(int i) { return i >= 0 && i < TS_CLASSES; }

static int unit_blocks(const TS_CLASSMAPS* cm, const TS_CLASSMAP* c, int32_t idx)
{
    const uint16_t s = cm->map->slot_a[idx];
    const TS_CUNIT* u;
    if (!s) return 0;
    if (!cm->units || s >= cm->units->n) return 1;
    u = &cm->units->u[s];
    return !u->mover || u->claim < c->wm;
}

/* 0x47DE60(class, cell), stdcall ret 8, DISASSEMBLED 0x47DE60..0x47DFB3: the feature rule, the unit rule, then the
   ground (ts_class_ground). */
int ts_class_cell(const TS_CLASSMAPS* cm, int i, int32_t idx)
{
    const TS_MAP* m = cm->map;
    if (!class_ok(i) || idx < 0 || idx >= m->cells) return 0;
    if (feature_blocks(m, idx) || unit_blocks(cm, &cm->c[i], idx)) return 0;
    return ts_class_ground(&cm->c[i], m->sea, m->hmin[idx], m->hmax[idx]);
}

/* 0x47DE60's rule past its unit rule: the depths against the sea level (a byte, main+0x1427F): the 2x2 minimum below
   sea - maxwaterdepth or the maximum above sea - minwaterdepth blocks (signed); then the slope, max - min as a byte,
   against the water pair when the minimum is below the sea level (unsigned) and the land pair otherwise: up to bad
   is 3, up to max 1, past it 0. */
int ts_class_ground(const TS_CLASSMAP* c, int32_t sea, int32_t mn, int32_t mx)
{
    uint8_t d;
    if (mn < sea - c->maxwaterdepth) return 0;
    if (mx > sea - c->minwaterdepth) return 0;
    d = (uint8_t)(mx - mn);
    if (mn < sea) return d <= c->badwaterslope ? 3 : d <= c->maxwaterslope ? 1 : 0;
    return d <= c->badslope ? 3 : d <= c->maxslope ? 1 : 0;
}

int ts_class_content(const TS_CLASSMAPS* cm, int i, int32_t idx)
{
    const TS_MAP* m = cm->map;
    if (!class_ok(i) || idx < 0 || idx >= m->cells || feature_blocks(m, idx)) return 0;
    return ts_class_ground(&cm->c[i], m->sea, m->hmin[idx], m->hmax[idx]);
}

/* THE FOLD, in place, in three passes over the window, each a scan that reads a byte before it writes it and looks
   only ahead of it. The row pass leaves in each column i two bits: a 0 among columns [i + 1, i + fx] (the
   footprint's) and a code below 3 among [i, i + fx + 1] (with the ring's), from the nearest such column at or past
   it, scanning from the right. The column pass does the same down each column over those bits with fz, giving the
   entry. The last pass packs entry (x, z), at window row z column x, to x + z w: an index at most its source's, so
   copying in ascending order overwrites only what has been copied. */
void ts_class_fold(uint8_t* grid, int32_t w, int32_t h, int32_t fx, int32_t fz)
{
    const int32_t W = w + fx + 2, H = h + fz + 2;
    int32_t i, j;
    if (w < 1 || h < 1 || fx < 1 || fz < 1) return;
    for (j = 0; j < H; j++) {
        uint8_t* row = grid + (size_t)j * (size_t)W;
        int32_t zero = W + fx + 2, low = W + fx + 2;    /* the nearest such column, none: past every window */
        for (i = W - 1; i >= 0; i--) {
            const uint8_t c = row[i];
            const int z = zero <= i + fx;
            if (c < 3) low = i;
            if (c == 0) zero = i;
            row[i] = (uint8_t)(z | (low <= i + fx + 1) << 1);
        }
    }
    for (i = 0; i < w; i++) {
        int32_t zero = H + fz + 2, low = H + fz + 2;
        for (j = H - 1; j >= 0; j--) {
            uint8_t* p = grid + (size_t)j * (size_t)W + (size_t)i;
            const uint8_t b = *p;
            const int z = zero <= j + fz;
            if (b & 2) low = j;
            if (b & 1) zero = j;
            *p = (uint8_t)(z ? 0 : low <= j + fz + 1 ? 1 : 3);
        }
    }
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++) grid[(size_t)j * (size_t)w + (size_t)i] = grid[(size_t)j * (size_t)W + (size_t)i];
}

/* 0x47DFC0(class, x, z, w, h), stdcall ret 0x14, DISASSEMBLED 0x47DFC0..0x47E1EC: 0 when x or z is negative or the
   rectangle reaches the last column or row (x + w >= W, z + h >= H, signed, W and H the grid's); else row by row the
   rule of ts_class_cell, any cell's 0 answering 0 at once and a bad slope lowering the answer from 3 to 1. An empty
   rectangle (w or h not positive) answers 3. */
int ts_class_rect(const TS_CLASSMAPS* cm, int i, int32_t x, int32_t z, int32_t w, int32_t h)
{
    const TS_CLASSMAP* c = &cm->c[class_ok(i) ? i : 0];
    const TS_MAP* m = cm->map;
    const int32_t sea = m->sea, lo = sea - c->maxwaterdepth, hi = sea - c->minwaterdepth;
    int32_t r = 3, row, col;
    if (!class_ok(i) || x < 0 || z < 0 || x + w >= m->w || z + h >= m->h) return 0;
    for (row = 0; row < h; row++)
        for (col = 0; col < w; col++) {
            const int32_t idx = (z + row) * m->w + x + col;
            const int32_t mn = m->hmin[idx], mx = m->hmax[idx];
            uint8_t d;
            if (feature_blocks(m, idx) || unit_blocks(cm, c, idx)) return 0;
            if (mn < lo || mx > hi) return 0;
            d = (uint8_t)(mx - mn);
            if (mn < sea) {
                if (d <= c->badwaterslope) continue;
                if (d > c->maxwaterslope) return 0;
            } else {
                if (d <= c->badslope) continue;
                if (d > c->maxslope) return 0;
            }
            if (r > 1) r = 1;
        }
    return r;
}

/* 0x47E1F0(class, x, z), stdcall ret 0xC, DISASSEMBLED 0x47E1F0..0x47E2CD: the footprint's answer when it is 0 or 1;
   else the ring in pinwheel order -- the top row x-1..x+fx-1, the right column z-1..z+fz-1, the bottom row x..x+fx,
   the left column z..z+fz, each corner once -- any part not 3 answering 1. A ring part off the grid, or on the last
   column or row, answers 0 from 0x47DFC0 and so 1 here, never 0. */
int ts_class_refresh(const TS_CLASSMAPS* cm, int i, int32_t x, int32_t z)
{
    int32_t fw, fh;
    int r;
    if (!class_ok(i)) return 0;
    fw = cm->c[i].fx;
    fh = cm->c[i].fz;
    r = ts_class_rect(cm, i, x, z, fw, fh);
    if (r <= 1) return r;
    if (ts_class_rect(cm, i, x - 1, z - 1, fw + 1, 1) != 3) return 1;
    if (ts_class_rect(cm, i, x + fw, z - 1, 1, fh + 1) != 3) return 1;
    if (ts_class_rect(cm, i, x, z + fh, fw + 1, 1) != 3) return 1;
    return ts_class_rect(cm, i, x - 1, z, 1, fh + 1) == 3 ? 3 : 1;
}

static void set_code(TS_CLASSMAPS* cm, TS_CLASSMAP* c, int32_t x, int32_t z, int v)
{
    uint32_t* p = &c->map[(z >> 4) * cm->w + x];
    const int sh = 2 * (z & 15);
    *p = (*p & ~(3u << sh)) | ((uint32_t)v << sh);
}

int ts_class_code(const TS_CLASSMAPS* cm, int i, int32_t x, int32_t z)
{
    const TS_CLASSMAP* c = &cm->c[class_ok(i) ? i : 0];
    if (!class_ok(i) || !c->map || x < 0 || x >= cm->w || z < 0 || z >= cm->h) return 0;
    return (int)(c->map[(z >> 4) * cm->w + x] >> (2 * (z & 15))) & 3;
}

/* One pass of the fill over a line of n codes in buf[0..n): the window's minimum, slid along it as 0x440500 slides
   it, then a 3 lowered to 1 when the cell before the window or the one after it is not 3. The minimum starts at 0;
   it is kept while the cell entering the window is above it and the one leaving was above it, and rescanned over
   the window when the one leaving held it (0x4405C8..0x440606, the column pass 0x44072D..0x440770, the same code).
   That is the exact sliding minimum for a window of one or more; for a footprint of 0 the engine's instructions run
   as they are and the port runs them the same way. buf[-2], buf[-1] and buf[n..n+span] are 0: the cells off the
   map. `put` stores entry k. */
static void fill_line(uint8_t* buf, int32_t n, int32_t span, TS_CLASSMAPS* cm, TS_CLASSMAP* c, int32_t fixed,
                      int rows)
{
    uint8_t run = 0;
    int32_t k, j;
    buf[-2] = buf[-1] = 0;
    for (k = 0; k <= span; k++) buf[n + k] = 0;
    for (k = 0; k < n; k++) {
        const int32_t e = k + span - 1;
        const uint8_t in = buf[e];
        int v;
        if (in <= run) run = in;
        else if (buf[k - 1] <= run) {
            run = buf[k];
            for (j = k + 1; j <= e; j++)
                if (buf[j] <= run) run = buf[j];
        }
        v = run;
        if (run == 3 && (buf[k - 1] < 3 || buf[e + 1] < 3)) v = 1;
        if (rows) set_code(cm, c, k, fixed, v);
        else set_code(cm, c, fixed, k, v);
    }
}

/* The fill 0x440500, thiscall on the class, DISASSEMBLED 0x440500..0x44082F. The row pass: per row, the rule
   0x47DE60 of every cell into the line, then fill_line over it with the footprint's width, each result stored at
   (x, z). The column pass: per column, the stored codes into the line, then fill_line with the footprint's depth.
   Entry (x, z) is then 0 when a cell of [x, x+fx) x [z, z+fz) is 0, 3 when every cell of [x-1, x+fx] x [z-1, z+fz]
   is 3, 1 otherwise, a cell off the map counting 0.
   THE ENGINE'S, KEPT: the fill and the refresh disagree at the far edges, because the fill counts only cells past
   the map as off it while 0x47DFC0 refuses every rectangle that reaches the last column or row. An entry whose
   footprint ends on the last column (x + fx == W) or row (z + fz == H) is 1 (or 0) after the fill and 0 after any
   refresh; one whose ring reaches the last column (x + fx == W - 1) or row (z + fz == H - 1) can be 3 after the fill
   and is 1 after any refresh. A map straight after the fill holds the fill's values there; the core holds the same. */
void ts_class_fill(TS_CLASSMAPS* cm, int i)
{
    TS_CLASSMAP* c = &cm->c[class_ok(i) ? i : 0];
    uint8_t* const buf = cm->line + 2;
    int32_t x, z;
    if (!class_ok(i) || !c->present || !c->map) return;
    for (z = 0; z < cm->h; z++) {
        for (x = 0; x < cm->w; x++) buf[x] = (uint8_t)ts_class_cell(cm, i, z * cm->w + x);
        fill_line(buf, cm->w, c->fx, cm, c, z, 1);
    }
    for (x = 0; x < cm->w; x++) {
        for (z = 0; z < cm->h; z++) buf[z] = (uint8_t)ts_class_code(cm, i, x, z);
        fill_line(buf, cm->h, c->fz, cm, c, x, 0);
    }
}

/* 0x440940, DISASSEMBLED 0x440940..0x4409FC: every present class of the 32 takes the grid's W and H and is filled,
   in index order; the loading bar's byte main+0x38D73 it advances is presentation. */
void ts_class_fill_all(TS_CLASSMAPS* cm)
{
    int i;
    for (i = 0; i < TS_CLASSES; i++)
        if (cm->c[i].present) ts_class_fill(cm, i);
}

/* 0x440830(xy, wh), thiscall on the class, ret 8, DISASSEMBLED 0x440830..0x44091F: every entry whose footprint and
   ring meet the rectangle -- z in [z - fz, z + h + 1), x in [x - fx, x + w + 1) -- recomputed through 0x47E1F0. Each
   argument is a word (movsx); the near ends clamp to 0 (signed), the far ends to the class's W and H by an UNSIGNED
   compare (0x440884 `jbe`, 0x440891). THE ENGINE'S, KEPT: a far end below 0 is a huge unsigned value, so it clamps to
   W (H) and the whole width (height) from the near end is recomputed. */
void ts_class_remark(TS_CLASSMAPS* cm, int i, int32_t x, int32_t z, int32_t w, int32_t h)
{
    TS_CLASSMAP* c = &cm->c[class_ok(i) ? i : 0];
    int32_t x0 = (int16_t)x - c->fx, z0 = (int16_t)z - c->fz;
    int32_t x1 = (int16_t)w + (int16_t)x + 1, z1 = (int16_t)h + (int16_t)z + 1, xx, zz;
    if (!class_ok(i) || !c->present || !c->map) return;
    if (x0 < 0) x0 = 0;
    if (z0 < 0) z0 = 0;
    if ((uint32_t)x1 > (uint32_t)cm->w) x1 = cm->w;
    if ((uint32_t)z1 > (uint32_t)cm->h) z1 = cm->h;
    if (x0 >= x1 || z0 >= z1) return;
    for (zz = z0; zz < z1; zz++)
        for (xx = x0; xx < x1; xx++) set_code(cm, c, xx, zz, ts_class_refresh(cm, i, xx, zz));
}

/* 0x440A40(xy, wh), stdcall ret 8, DISASSEMBLED 0x440A40..0x440A6C: 0x440830 in every present class, in index order.
   Its callers: a feature placed (0x424031) or destroyed (0x424822), a structure stamped (0x47CEAD), a yard opened
   (0x47DB0A), the saved game's re-claim walk (0x47DB58). */
void ts_class_remark_all(TS_CLASSMAPS* cm, int32_t x, int32_t z, int32_t w, int32_t h)
{
    int i;
    for (i = 0; i < TS_CLASSES; i++)
        if (cm->c[i].present) ts_class_remark(cm, i, x, z, w, h);
}

static TS_CUNIT* slot_unit(TS_CLASSMAPS* cm, int32_t slot)
{
    return cm->units && slot > 0 && slot < cm->units->n ? &cm->units->u[slot] : NULL;
}

/* The claim 0x440A70(unit), stdcall ret 4, DISASSEMBLED 0x440A70..0x440AE0, the footprint clear's last act
   (0x47D2CA). With a mover: the stamp becomes GameTime, and every class whose watermark is above the old stamp
   re-marks the unit's rectangle -- a unit that stood still was an obstacle there, and is erased as one once it moves.
   Without one (a building): every present class re-marks it. THE BOUND: the mover branch (0x440A94..0x440AB1) walks
   the 32 records without the present test the other loops make, and an absent class's re-mark writes nothing: its
   W and H (+0x10, +0x14) are 0 -- the static constructor 0x440230 and the teardown 0x440A00 zero them, and 0x440940
   sets them only for a class with a name (0x440974) -- so 0x440830's far ends clamp to 0 (0x440881..0x440897) and
   it leaves at 0x44089D (DISASSEMBLED). The core tests presence, which is the same. */
void ts_class_claim(TS_CLASSMAPS* cm, int32_t slot, uint32_t now)
{
    TS_CUNIT* u = slot_unit(cm, slot);
    uint32_t old;
    int i;
    if (!u) return;
    if (!u->mover) {
        ts_class_remark_all(cm, u->cx, u->cz, u->sx, u->sz);
        return;
    }
    old = u->claim;
    u->claim = now;
    for (i = 0; i < TS_CLASSES; i++)
        if (cm->c[i].present && old < cm->c[i].wm) ts_class_remark(cm, i, u->cx, u->cz, u->sx, u->sz);
}

/* The search's watermark 0x440AF0(class, unit), thiscall ret 4, DISASSEMBLED 0x440AF0..0x440BCE:
   the watermark becomes max(GameTime, 30) - 30 (unsigned); the searcher's stamp is set to GameTime for the walk, so
   it is not its own obstacle, and if its old stamp was below the old watermark its rectangle is re-marked; if the
   watermark moved, every slot from 1 to the last whose unit is alive (+0x110 bit 28) with a mover and a stamp in
   [old watermark, new watermark) is re-marked -- it has become an obstacle; then the searcher's stamp is put back.
   THE BOUND: the engine reads the searcher's mover with no test (only a mover searches); the core does nothing for a
   unit without one. */
void ts_class_watermark(TS_CLASSMAPS* cm, int i, int32_t slot, uint32_t now)
{
    TS_CLASSMAP* c = &cm->c[class_ok(i) ? i : 0];
    TS_CUNIT* u = slot_unit(cm, slot);
    const uint32_t oldw = c->wm, neww = (now > 30 ? now : 30) - 30;
    uint32_t old;
    int32_t s;
    if (!class_ok(i) || !u || !u->mover || !c->present) return;
    c->wm = neww;
    old = u->claim;
    u->claim = now;
    if (old < oldw) ts_class_remark(cm, i, u->cx, u->cz, u->sx, u->sz);
    if (neww != oldw)
        for (s = 1; s < cm->units->n; s++) {
            const TS_CUNIT* v = &cm->units->u[s];
            if (v->alive && v->mover && v->claim >= oldw && v->claim < neww)
                ts_class_remark(cm, i, v->cx, v->cz, v->sx, v->sz);
        }
    u->claim = old;
}

/* The search's cleanup 0x440BE0(class, unit), thiscall ret 4, DISASSEMBLED 0x440BE0..0x440BFF: a searcher whose stamp
   is below the watermark is an obstacle again, and its rectangle is re-marked. THE BOUND as the watermark's. */
void ts_class_cleanup(TS_CLASSMAPS* cm, int i, int32_t slot)
{
    TS_CUNIT* u = slot_unit(cm, slot);
    if (!class_ok(i) || !u || !u->mover || !cm->c[i].present) return;
    if (u->claim < cm->c[i].wm) ts_class_remark(cm, i, u->cx, u->cz, u->sx, u->sz);
}
