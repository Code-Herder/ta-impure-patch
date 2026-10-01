/* tagpu_recreader.c -- reads a demo recording back (tagpu_recreader.h): the blocks, the header's
   entries, and the world the stream builds, one iteration at a time.

   A block is checked before anything reads it: its header's magic and CRC, its sizes against
   RECR_BLOCK_MAX, its payload's CRC, and the inflated size against the header's. A block that
   fails ends the reading there, as it ends tarec.py's. The inflate itself (lodepng) grows its
   output as it goes and fails cleanly when it cannot allocate; a recording is the local game's
   own file, so a stream crafted to inflate without end is not guarded beyond that. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_recreader.h"
#include "lodepng.h"
#include "crc32.h"

struct RecReader {
    HANDLE             f;
    unsigned long long off;         /* the file offset of the next block                      */
    RecHeader          h;
    RecWorld           w;
    unsigned char*     pk;          /* the block's packed bytes                               */
    unsigned           pkcap;
    unsigned char*     raw;         /* its payload: pk for a stored block, else the inflate's  */
    size_t             rawn;
    size_t             at;          /* the next record in the payload                         */
    int                ended;       /* the end block, or the file's end, has been read         */
    char               end[160];    /* how the recording ended, once `ended`                   */
    unsigned*          touched;     /* the slots whose `changed` is set                        */
    unsigned           ntouched;
    unsigned           evcap, fcap;
    unsigned           fbracket;    /* an RK_EV_FEATURE's root + 1, until its END             */
    unsigned           ndeaths;     /* the deaths whose RK_EV_DEATH_END is still to come      */
    unsigned           death[REC_DEATHS_MAX];
    struct { unsigned slot, list, total, next; } pend;   /* a list sent in several records   */
};

static const char* const END_REASONS[] = {
    "", "complete: the level ended", "INCOMPLETE: the ring was full",
    "INCOMPLETE: the file could not be written",
    "INCOMPLETE: no memory for a declared table's growth",
    "INCOMPLETE: the level changed without a teardown",
    "INCOMPLETE: the game stopped being a skirmish",
    "INCOMPLETE: an order list could not be recorded",
    "INCOMPLETE: the features were not the ones it began with"
};

/* ---- blocks --------------------------------------------------------------------------------- */

static void payload_free(RecReader* r)
{
    if (r->raw && r->raw != r->pk) free(r->raw);
    r->raw = NULL;
    r->rawn = 0;
    r->at = 0;
}

static int read_all(HANDLE f, void* p, DWORD n)
{
    DWORD got = 0;
    return ReadFile(f, p, n, &got, NULL) && got == n;
}

/* 1 a block, its payload in raw; 0 the file's end; -1 a block that cannot be read (why) */
static int block_read(RecReader* r, RecBlock* b, char* why, unsigned cap)
{
    DWORD got = 0;
    unsigned lz;
    payload_free(r);
    if (!ReadFile(r->f, b, sizeof *b, &got, NULL)) {
        snprintf(why, cap, "cannot read at byte %llu (%lu)", r->off, GetLastError());
        return -1;
    }
    if (!got) return 0;
    if (got < sizeof *b) {
        snprintf(why, cap, "a block header cut short at byte %llu", r->off);
        return -1;
    }
    if (b->magic != REC_BLOCK_MAGIC || (uint32_t)Crc32_ComputeBuf(0, b, 28) != b->crc_head) {
        snprintf(why, cap, "a torn block header at byte %llu", r->off);
        return -1;
    }
    if (b->packed > RECR_BLOCK_MAX || b->raw > RECR_BLOCK_MAX ||
        ((b->flags & RBF_STORED) && b->packed != b->raw)) {
        snprintf(why, cap, "a block at byte %llu of %u bytes, %u inflated: past what it may hold",
                 r->off, b->packed, b->raw);
        return -1;
    }
    if (b->packed > r->pkcap) {
        unsigned char* n = (unsigned char*)realloc(r->pk, b->packed);
        if (!n) {
            snprintf(why, cap, "no memory for a block of %u bytes", b->packed);
            return -1;
        }
        r->pk = n;
        r->pkcap = b->packed;
    }
    if (b->packed && !read_all(r->f, r->pk, b->packed)) {
        snprintf(why, cap, "a block cut short at byte %llu", r->off);
        return -1;
    }
    if ((uint32_t)Crc32_ComputeBuf(0, r->pk, b->packed) != b->crc_body) {
        snprintf(why, cap, "a block whose payload fails its CRC at byte %llu", r->off);
        return -1;
    }
    if (b->flags & RBF_STORED) {
        r->raw = r->pk;
        r->rawn = b->packed;
    } else {
        unsigned char* out = NULL;
        size_t outn = 0;
        lz = b->packed ? lodepng_zlib_decompress(&out, &outn, r->pk, b->packed,
                                                 &lodepng_default_decompress_settings)
                       : 1;
        if (lz || outn != b->raw) {
            free(out);
            snprintf(why, cap, "a block at byte %llu that does not inflate to its %u bytes",
                     r->off, b->raw);
            return -1;
        }
        r->raw = out;
        r->rawn = outn;
    }
    r->off += sizeof *b + b->packed;
    return 1;
}

/* ---- the header ----------------------------------------------------------------------------- */

static void text(char* dst, unsigned cap, const unsigned char* p, unsigned n)
{
    unsigned k = 0;
    while (k < n && k + 1 < cap && p[k]) {
        dst[k] = (char)p[k];
        k++;
    }
    dst[k] = 0;
}

static unsigned u32(const unsigned char* p)
{
    unsigned v;
    memcpy(&v, p, 4);
    return v;
}

/* 1 parsed; 0 malformed or of another format (why) */
static int header_parse(RecHeader* h, const unsigned char* b, size_t len, char* why, unsigned cap)
{
    size_t at = 0;
    unsigned seen = 0, i;
    memset(h, 0, sizeof *h);
    while (at + 4 <= len) {
        unsigned short tag, n;
        const unsigned char* p = b + at + 4;
        memcpy(&tag, b + at, 2);
        memcpy(&n, b + at + 2, 2);
        if (at + 4 + n > len) {
            snprintf(why, cap, "a header entry past its block");
            return 0;
        }
        switch (tag) {
        case RT_FORMAT:        if (n == 4) { h->format = u32(p); seen |= 1; } break;
        case RT_BUILD:         text(h->build, sizeof h->build, p, n); break;
        case RT_MODE:          if (n == 4) { h->mode = u32(p); seen |= 2; } break;
        case RT_MAP:           text(h->map, sizeof h->map, p, n); break;
        case RT_START:         if (n == 4) h->start = u32(p); break;
        case RT_SLOTS:         if (n == 4) { h->slots = u32(p); seen |= 4; } break;
        case RT_UNITS_EACH:    if (n == 4) h->units_each = u32(p); break;
        case RT_UNITDEFS:
            if (n == 8) {
                h->has_unitdefs = 1;
                h->ndefs = u32(p);
                h->defs_hash = u32(p + 4);
            }
            break;
        case RT_WEAPONS:
            if (n == 8) {
                h->has_weapons = 1;
                h->nweapons = u32(p);
                h->weapons_hash = u32(p + 4);
            }
            break;
        case RT_FEATURES:
            if (n == 20) {
                h->grid_w = u32(p);
                h->grid_h = u32(p + 4);
                h->wrecks = u32(p + 8);
                h->nfdefs = u32(p + 12);
                h->fdefs_hash = u32(p + 16);
                seen |= 8;
            }
            break;
        case RT_LAYOUT:        text(h->layout, sizeof h->layout, p, n); break;
        case RT_SETTINGS_MAP:
            text(h->settings_map, sizeof h->settings_map, p, n);
            h->has_settings_map = 1;
            break;
        case RT_OPTIONS:
            if (n == 20) {
                unsigned short np;
                h->has_options = 1;
                h->lostype = u32(p);
                h->cmdrdeath = u32(p + 4);
                h->difficulty = u32(p + 8);
                h->fixed_starts = u32(p + 12);
                memcpy(&np, p + 16, 2);
                h->nplayers = np;
                h->controlled = p[18];
                h->viewed = p[19];
            }
            break;
        case RT_PLAYER: {
            RecSeat* s;
            if (n < 16 + REC_INFO_BYTES || p[0] >= REC_PLAYERS || h->nseats >= REC_PLAYERS) {
                snprintf(why, cap, "a malformed seat in the header");
                return 0;
            }
            for (i = 0; i < h->nseats; i++)
                if (h->seats[i].index == p[0]) {
                    snprintf(why, cap, "seat %u twice in the header", p[0]);
                    return 0;
                }
            s = &h->seats[h->nseats++];
            s->index = p[0];
            s->controller = p[1];
            s->side = p[2];
            s->logo = p[3];
            memcpy(s->ally, p + 4, 11);
            memcpy(s->info, p + 16, REC_INFO_BYTES);
            s->has_info = 0;
            for (i = 0; i < REC_INFO_BYTES; i++)
                if (s->info[i] != 0xFF) s->has_info = 1;
            text(s->name, sizeof s->name, p + 16 + REC_INFO_BYTES, n - 16 - REC_INFO_BYTES);
            break;
        }
        case RT_TABLE: {
            RecTableInfo* t;
            if (n < 12 || h->ntab >= REC_TABLE_MAX) {
                snprintf(why, cap, "a malformed table in the header");
                return 0;
            }
            t = &h->tab[h->ntab];
            memcpy(&t->index, p, 2);
            t->cls = p[2];
            t->key = p[3];
            t->stride = u32(p + 4);
            memcpy(&t->flags, p + 8, 2);
            text(t->name, sizeof t->name, p + 12, n - 12);
            for (i = 0; i < h->ntab; i++)
                if (h->tab[i].index == t->index) {
                    snprintf(why, cap, "table %u twice in the header", t->index);
                    return 0;
                }
            if (!t->stride || t->stride > RECR_BLOCK_MAX) {
                snprintf(why, cap, "table %u with rows of %u bytes", t->index, t->stride);
                return 0;
            }
            h->ntab++;
            break;
        }
        default:
            break;      /* RT_WALLCLOCK, RT_LEVER: nothing the replay needs */
        }
        at += (4 + (size_t)n + 3) & ~(size_t)3;
    }
    if ((seen & 1) && h->format != REC_FORMAT) {
        snprintf(why, cap, "format %u, this build reads %u", h->format, REC_FORMAT);
        return 0;
    }
    if (seen != 15) {
        snprintf(why, cap, "a header without its format, mode, slots or features");
        return 0;
    }
    if (h->slots < 2 || h->slots > 0xFFFF) {
        snprintf(why, cap, "a unit array of %u slots", h->slots);
        return 0;
    }
    if (!h->grid_w || h->grid_w > 0xFFFF || !h->grid_h || h->grid_h > 0xFFFF || !h->wrecks ||
        h->wrecks > 0x8000) {
        snprintf(why, cap, "a feature grid of %u x %u cells, or a wreck pool of %u records",
                 h->grid_w, h->grid_h, h->wrecks);
        return 0;
    }
    return 1;
}

static void end_parse(RecReader* r, const unsigned char* b, size_t len)
{
    size_t at = 0;
    unsigned reason = 0, last = 0, iters = 0;
    while (at + 4 <= len) {
        unsigned short tag, n;
        memcpy(&tag, b + at, 2);
        memcpy(&n, b + at + 2, 2);
        if (at + 4 + n > len) break;
        if (n == 4 && tag == RT_END_REASON) reason = u32(b + at + 4);
        if (n == 4 && tag == RT_LAST) last = u32(b + at + 4);
        if (n == 4 && tag == RT_ITERATIONS) iters = u32(b + at + 4);
        at += (4 + (size_t)n + 3) & ~(size_t)3;
    }
    snprintf(r->end, sizeof r->end, "%s (%u iterations to GameTime %u)",
             reason && reason < sizeof END_REASONS / sizeof END_REASONS[0]
                 ? END_REASONS[reason] : "an end of an unknown reason",
             iters, last);
}

/* ---- the world ------------------------------------------------------------------------------ */

static int world_init(RecReader* r, char* why, unsigned cap)
{
    RecWorld* w = &r->w;
    unsigned i;
    w->slots = r->h.slots;
    w->live = (unsigned char*)calloc(w->slots, 1);
    w->changed = (unsigned char*)calloc(w->slots, 1);
    w->list = (RecList*)calloc((size_t)w->slots * 2, sizeof *w->list);
    w->unit = (uint32_t*)VirtualAlloc(NULL, (size_t)w->slots * REC_UNIT_IMAGE,
                                      MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    r->touched = (unsigned*)malloc((size_t)w->slots * sizeof *r->touched);
    w->ntab = r->h.ntab;
    w->tab = (RecTable*)calloc(w->ntab ? w->ntab : 1, sizeof *w->tab);
    w->grid = (unsigned char*)VirtualAlloc(NULL, (size_t)r->h.grid_w * r->h.grid_h * REC_CELL_BYTES,
                                           MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    w->pool = (unsigned char*)calloc(r->h.wrecks, REC_WRECK_BYTES);
    if (!w->live || !w->changed || !w->list || !w->unit || !r->touched || !w->tab || !w->grid ||
        !w->pool) {
        snprintf(why, cap, "no memory for the world of %u slots and %u x %u cells", w->slots,
                 r->h.grid_w, r->h.grid_h);
        return 0;
    }
    for (i = 0; i < w->ntab; i++) {
        w->tab[i].index = r->h.tab[i].index;
        w->tab[i].stride = r->h.tab[i].stride;
    }
    for (i = 0; i < r->h.nseats; i++) w->ctl[r->h.seats[i].index] = r->h.seats[i].controller;
    if (r->h.has_options) {
        w->view[0] = (unsigned char)r->h.controlled;
        w->view[1] = (unsigned char)r->h.viewed;
    }
    return 1;
}

static RecTable* table_of(RecWorld* w, unsigned index)
{
    unsigned i;
    for (i = 0; i < w->ntab; i++)
        if (w->tab[i].index == index) return &w->tab[i];
    return NULL;
}

static int grow(unsigned char** p, unsigned* cap, size_t bytes)
{
    unsigned char* n;
    if (bytes <= *cap) return 1;
    n = (unsigned char*)realloc(*p, bytes);
    if (!n) return 0;
    *p = n;
    *cap = (unsigned)bytes;
    return 1;
}

static void touch(RecReader* r, unsigned slot, unsigned what)
{
    if (!r->w.changed[slot]) r->touched[r->ntouched++] = slot;
    r->w.changed[slot] |= (unsigned char)what;
}

/* the dwords a delta names: its bitmap's set bits, none past `dwords` */
static int delta_count(const unsigned char* bits, unsigned nbytes, unsigned dwords, unsigned* n)
{
    unsigned i, c = 0;
    for (i = 0; i < nbytes * 8; i++)
        if (bits[i >> 3] >> (i & 7) & 1) {
            if (i >= dwords) return 0;
            c++;
        }
    *n = c;
    return 1;
}

static void delta_apply(uint32_t* img, const unsigned char* bits, unsigned dwords,
                        const unsigned char* vals)
{
    unsigned i, k = 0;
    for (i = 0; i < dwords; i++)
        if (bits[i >> 3] >> (i & 7) & 1) memcpy(&img[i], vals + 4 * k++, 4);
}

static int event_len(unsigned kind, unsigned len)
{
    switch (kind) {
    case RK_EV_CREATE:      return len == 24;
    case RK_EV_DEATH:       return len == 12 + REC_UNIT_IMAGE;
    case RK_EV_SHOT:        return len == 36;
    case RK_EV_INTERCEPT:   return len == 16;
    case RK_EV_START:       return len == 28;
    case RK_EV_START_NAMED: return len >= 24 && len <= 24 + REC_NAME_MAX;
    case RK_EV_BUILT:       return len == 8;
    case RK_EV_STATE:       return len == 8;
    case RK_EV_REPLY:       return len >= 12 && len <= 12 + REC_TEXT_MAX;
    case RK_EV_SPRAY:       return len == 48;
    case RK_EV_DRAW:        return len == 12;
    default:                return 0;
    }
}

/* an RK_EV_FEATURE's arguments after its site, by its root (tagpu_recfmt.h, RecRoot) */
static int root_len(unsigned root, unsigned len)
{
    static const unsigned char args[FR_ROOTS] = { 0, 28, 8, 12, 12, 16, 12 };
    return root < FR_ROOTS && len == 8 + args[root];
}

/* An RK_CELLS, bounded by the grid: 1 well-formed. */
static int cells_ok(const RecReader* r, const unsigned char* b, unsigned len)
{
    unsigned short h[4];
    if (len < 8) return 0;
    memcpy(h, b, 8);
    return h[2] && h[2] <= REC_CELLS_MAX && !h[3] && len == 8u + h[2] * REC_CELL_BYTES &&
           (unsigned)h[0] + h[2] <= r->h.grid_w && h[1] < r->h.grid_h;
}

static int list_add(RecEvent** l, unsigned* n, unsigned* cap, unsigned kind, unsigned len,
                    const unsigned char* b)
{
    if (*n == *cap) {
        unsigned c = *cap ? *cap * 2 : 256;
        RecEvent* e = (RecEvent*)realloc(*l, c * sizeof *e);
        if (!e) return 0;
        *l = e;
        *cap = c;
    }
    (*l)[*n].kind = kind;
    (*l)[*n].len = len;
    (*l)[*n].b = b;
    (*n)++;
    return 1;
}

/* An event of the step, by the brackets' grammar (tagpu_recwriter.c, feature_record): a feature
   call's holds no other and cells only when it is the tick's, which is inside none; a death's
   holds its explosion's feature calls and may hold deaths; a draw is inside one. 1 taken, 0
   malformed. */
static int event_take(RecReader* r, unsigned kind, const unsigned char* b, unsigned len)
{
    RecWorld* w = &r->w;
    unsigned id;
    switch (kind) {
    case RK_EV_FEATURE:
        if (r->fbracket || (b[0] == FR_TICK && r->ndeaths) || b[1] || b[2] || b[3] ||
            !root_len(b[0], len))
            return 0;
        r->fbracket = b[0] + 1u;
        break;
    case RK_EV_FEATURE_END:
        if (len != 8 || r->fbracket != b[0] + 1u || b[1] || b[2] || b[3]) return 0;
        r->fbracket = 0;
        break;
    case RK_EV_DEATH:
        if (r->fbracket || r->ndeaths == REC_DEATHS_MAX || !event_len(kind, len)) return 0;
        memcpy(&r->death[r->ndeaths++], b, 4);
        break;
    case RK_EV_DEATH_END:
        if (len != 4 || !r->ndeaths) return 0;
        memcpy(&id, b, 4);
        if (id != r->death[r->ndeaths - 1]) return 0;
        r->ndeaths--;
        break;
    case RK_EV_DRAW:
        if ((!r->fbracket && !r->ndeaths) || !event_len(kind, len) || b[0] >= FS_SITES || b[1] ||
            b[2] || b[3])
            return 0;
        break;
    case RK_CELLS:
        if (r->fbracket != FR_TICK + 1 || !cells_ok(r, b, len)) return 0;
        break;
    default:
        if (kind < RK_EV_CREATE || kind > RK_EV_SPRAY || !event_len(kind, len)) return 0;
        break;
    }
    return list_add(&w->ev, &w->nev, &r->evcap, kind, len, b);
}

/* An RK_ORDERS: the first record of a list starts it, and the rest must be its own, in order,
   until every node has come (tagpu_recwriter.c, list_record). 1 applied, 0 malformed. */
static int list_record(RecReader* r, const unsigned char* b, unsigned len)
{
    RecWorld* w = &r->w;
    unsigned short slot, total, first;
    unsigned list, k;
    RecList* l;
    if (len < 8 || (len - 8) % REC_ORDER_IMAGE) return 0;
    memcpy(&slot, b, 2);
    memcpy(&total, b + 4, 2);
    memcpy(&first, b + 6, 2);
    list = b[2];
    k = (len - 8) / REC_ORDER_IMAGE;
    if (list > 1 || b[3] || slot < 1 || slot >= w->slots || !w->live[slot] ||
        total > REC_ORDERS_MAX || k > REC_ORDERS_CHUNK || first + k > total || (total && !k))
        return 0;
    l = &w->list[(size_t)slot * 2 + list];
    if (!first) {
        if (r->pend.next < r->pend.total) return 0;
        r->pend.slot = slot;
        r->pend.list = list;
        r->pend.total = total;
        r->pend.next = 0;
        if (!grow(&l->img, &l->cap, (size_t)total * REC_ORDER_IMAGE)) return 0;
        l->n = total;
    } else if (r->pend.slot != slot || r->pend.list != list || r->pend.total != total ||
               r->pend.next != first) {
        return 0;
    }
    r->pend.next = first + k;
    memcpy(l->img + (size_t)first * REC_ORDER_IMAGE, b + 8, (size_t)k * REC_ORDER_IMAGE);
    touch(r, slot, list ? RECW_SUB : RECW_MAIN);
    return 1;
}

/* One record of an iteration after its tick. 1 applied, 0 malformed. */
static int apply(RecReader* r, unsigned kind, const unsigned char* b, unsigned len, int* events)
{
    RecWorld* w = &r->w;
    unsigned short id = 0;
    unsigned n = 0, off = 0;
    RecTable* t;
    if (r->pend.next < r->pend.total && kind != RK_ORDERS) return 0;
    if (len >= 2) memcpy(&id, b, 2);
    if ((kind >= RK_EV_CREATE && kind <= RK_EV_DEATH_END) || (kind == RK_CELLS && *events)) {
        if (!event_take(r, kind, b, len)) return 0;
        *events = 1;
        return 1;
    }
    /* the images come before the events of the step after them (tagpu_recfmt.h, RK_TICK) */
    if (*events) return 0;
    switch (kind) {
    case RK_CELLS: {
        unsigned short h[4];
        if (!cells_ok(r, b, len)) return 0;
        memcpy(h, b, 8);
        memcpy(w->grid + ((size_t)h[1] * r->h.grid_w + h[0]) * REC_CELL_BYTES, b + 8,
               (size_t)h[2] * REC_CELL_BYTES);
        return list_add(&w->feat, &w->nfeat, &r->fcap, kind, len, b);
    }
    case RK_WRECK:
        if (len != 4 + REC_WRECK_BYTES || id >= r->h.wrecks || b[2] || b[3]) return 0;
        memcpy(w->pool + (size_t)id * REC_WRECK_BYTES, b + 4, REC_WRECK_BYTES);
        return list_add(&w->feat, &w->nfeat, &r->fcap, kind, len, b);
    case RK_FEATURES:
        if (len != 16) return 0;
        memcpy(w->heads, b, 16);
        return list_add(&w->feat, &w->nfeat, &r->fcap, kind, len, b);
    case RK_UNIT:
        if (len != 4 + REC_UNIT_IMAGE || id < 1 || id >= w->slots) return 0;
        memcpy(w->unit + (size_t)id * REC_UNIT_DWORDS, b + 4, REC_UNIT_IMAGE);
        w->live[id] = 1;
        touch(r, id, RECW_IMAGE | RECW_BORN);
        return 1;
    case RK_UNIT_DELTA:
        if (len < REC_UNIT_DELTA_HEAD || id < 1 || id >= w->slots || !w->live[id] ||
            !delta_count(b + 2, REC_UNIT_BITS, REC_UNIT_DWORDS, &n) ||
            len != REC_UNIT_DELTA_HEAD + 4 * n)
            return 0;
        delta_apply(w->unit + (size_t)id * REC_UNIT_DWORDS, b + 2, REC_UNIT_DWORDS,
                    b + REC_UNIT_DELTA_HEAD);
        touch(r, id, RECW_IMAGE);
        return 1;
    case RK_UNIT_GONE:
        if (len != 4 || id < 1 || id >= w->slots) return 0;
        w->live[id] = 0;
        w->list[(size_t)id * 2].n = 0;
        w->list[(size_t)id * 2 + 1].n = 0;
        touch(r, id, RECW_GONE);
        return 1;
    case RK_PLAYER:
        if (len != 4 + REC_PLAYER_IMAGE || id >= REC_PLAYERS) return 0;
        memcpy(w->player[id], b + 4, REC_PLAYER_IMAGE);
        w->pchanged[id] = 1;
        return 1;
    case RK_PLAYER_DELTA:
        if (len < REC_PLAYER_DELTA_HEAD || id >= REC_PLAYERS ||
            !delta_count(b + 2, REC_PLAYER_BITS, REC_PLAYER_DWORDS, &n) ||
            len != REC_PLAYER_DELTA_HEAD + 4 * n)
            return 0;
        delta_apply(w->player[id], b + 2, REC_PLAYER_DWORDS, b + REC_PLAYER_DELTA_HEAD);
        w->pchanged[id] = 1;
        return 1;
    case RK_TABLE_SIZE:
        if (len != 8 || !(t = table_of(w, id))) return 0;
        memcpy(&n, b + 4, 4);
        if (n % t->stride || n > RECR_BLOCK_MAX || !grow(&t->img, &t->cap, n)) return 0;
        if (n > t->size) memset(t->img + t->size, 0, n - t->size);
        t->size = n;
        return 1;
    case RK_TABLE_SPAN:
        if (len < 8 || !(t = table_of(w, id))) return 0;
        memcpy(&off, b + 4, 4);
        n = len - 8;
        if (off > t->size || n > t->size - off) return 0;
        memcpy(t->img + off, b + 8, n);
        return 1;
    case RK_ORDERS:
        return list_record(r, b, len);
    case RK_CONTROLLER:
        if (len != 4 || b[0] >= REC_PLAYERS || b[2] || b[3]) return 0;
        w->ctl[b[0]] = b[1];
        return 1;
    case RK_VIEWER:
        if (len != 4 || b[2] || b[3]) return 0;
        w->view[0] = b[0];
        w->view[1] = b[1];
        return 1;
    case RK_CORRECT:
        if (len != 4) return 0;
        w->corrections++;
        return 1;
    default:
        return 0;   /* the ring's own kinds are never in a block */
    }
}

/* An iteration is whole only when no order list it began is still owed chunks -- list_record
   sets the list's count from its first chunk, so a list cut short would leave the rest of its
   nodes unwritten -- and no feature bracket is still open. 1, or -1 with `why`. */
static int iteration_done(const RecReader* r, char* why, unsigned cap)
{
    if (r->pend.next < r->pend.total) {
        snprintf(why, cap, "an order list cut short in the block before byte %llu", r->off);
        return -1;
    }
    if (r->fbracket || r->ndeaths) {
        snprintf(why, cap, "a feature call's or a death's events cut short in the block before "
                 "byte %llu", r->off);
        return -1;
    }
    return 1;
}

int recr_tick(RecReader* r, char* why, unsigned cap)
{
    RecWorld* w = &r->w;
    unsigned i;
    int began = 0, events = 0;
    for (i = 0; i < r->ntouched; i++) w->changed[r->touched[i]] = 0;
    r->ntouched = 0;
    memset(w->pchanged, 0, sizeof w->pchanged);
    w->nev = 0;
    w->nfeat = 0;
    for (;;) {
        RecHdr hd;
        const unsigned char* b;
        if (r->at >= r->rawn) {
            RecBlock blk;
            int k;
            /* a stream block ends where an iteration does (tagpu_recwriter.c, iteration_done) */
            if (began) return iteration_done(r, why, cap);
            if (r->ended) {
                snprintf(why, cap, "%s", r->end);
                return 0;
            }
            k = block_read(r, &blk, why, cap);
            if (k < 0) return -1;
            if (!k) {
                r->ended = 1;
                snprintf(r->end, sizeof r->end, "INCOMPLETE: the file ends without its end block");
                snprintf(why, cap, "%s", r->end);
                return 0;
            }
            if (blk.kind == RB_END) {
                r->ended = 1;
                end_parse(r, r->raw, r->rawn);
                payload_free(r);
                snprintf(why, cap, "%s", r->end);
                return 0;
            }
            if (blk.kind != RB_STREAM) payload_free(r);   /* a snapshot: S3's seek reads them */
            continue;
        }
        if (r->at + 4 > r->rawn) {
            snprintf(why, cap, "a record header cut short in the block before byte %llu", r->off);
            return -1;
        }
        memcpy(&hd, r->raw + r->at, 4);
        if (r->at + 4 + hd.len > r->rawn) {
            snprintf(why, cap, "a record cut short in the block before byte %llu", r->off);
            return -1;
        }
        b = r->raw + r->at + 4;
        if (hd.kind == RK_TICK) {
            /* the next iteration's: left for the next call */
            if (began) return iteration_done(r, why, cap);
            if (hd.len != 4 || r->pend.next < r->pend.total) {
                snprintf(why, cap, "a malformed tick in the block before byte %llu", r->off);
                return -1;
            }
            memcpy(&w->tick, b, 4);
            began = 1;
        } else if (!began || !apply(r, hd.kind, b, hd.len, &events)) {
            snprintf(why, cap, "a malformed record of kind %u at GameTime %u, in the block before "
                     "byte %llu", hd.kind, w->tick, r->off);
            return -1;
        }
        r->at += (4 + (size_t)hd.len + 3) & ~(size_t)3;
    }
}

/* ---- open and close ------------------------------------------------------------------------- */

RecReader* recr_open(const char* path, char* why, unsigned cap)
{
    RecReader* r = (RecReader*)calloc(1, sizeof *r);
    char magic[8];
    RecBlock b;
    int k;
    if (!r) {
        snprintf(why, cap, "no memory");
        return NULL;
    }
    r->f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                       FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (r->f == INVALID_HANDLE_VALUE) {
        snprintf(why, cap, "cannot open %s (%lu)", path, GetLastError());
        free(r);
        return NULL;
    }
    if (!read_all(r->f, magic, 8) || memcmp(magic, REC_MAGIC, 8)) {
        snprintf(why, cap, "%s is not a recording", path);
        goto fail;
    }
    r->off = 8;
    k = block_read(r, &b, why, cap);
    if (k <= 0 || b.kind != RB_HEADER) {
        if (k >= 0) snprintf(why, cap, "%s has no header block", path);
        goto fail;
    }
    if (!header_parse(&r->h, r->raw, r->rawn, why, cap) || !world_init(r, why, cap)) goto fail;
    payload_free(r);
    return r;
fail:
    recr_close(r);
    return NULL;
}

const RecHeader* recr_header(const RecReader* r) { return &r->h; }
RecWorld* recr_world(RecReader* r) { return &r->w; }

void recr_close(RecReader* r)
{
    unsigned i;
    if (!r) return;
    payload_free(r);
    if (r->f && r->f != INVALID_HANDLE_VALUE) CloseHandle(r->f);
    free(r->pk);
    if (r->w.list)
        for (i = 0; i < r->w.slots * 2; i++) free(r->w.list[i].img);
    for (i = 0; i < r->w.ntab && r->w.tab; i++) free(r->w.tab[i].img);
    free(r->w.list);
    free(r->w.tab);
    free(r->w.live);
    free(r->w.changed);
    free(r->w.ev);
    free(r->w.feat);
    free(r->w.pool);
    free(r->touched);
    if (r->w.unit) VirtualFree(r->w.unit, 0, MEM_RELEASE);
    if (r->w.grid) VirtualFree(r->w.grid, 0, MEM_RELEASE);
    free(r);
}
