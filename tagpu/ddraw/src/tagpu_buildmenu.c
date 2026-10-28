/* tagpu_buildmenu.c -- the build menu's offered types by the page loader's rule; the rule, its
   addresses and its bounds are in tagpu_buildmenu.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "tagpu_buildmenu.h"
#include "tagpu_guifile.h"

#define REC_NAME 16          /* the record's name field, +2: 15 characters from a file (0x4AD3A7) */
#define PRESS_NAME 16        /* the handler's copy of the name (0x49FEE0..0x49FEFF) */

/* The fields of a gadget record this rule reads. `name` holds a filled entry's whole name. */
typedef struct {
    unsigned char id, active, common, grey;
    char name[TAGPU_BM_ENTRY_NAME];
} Rec;

/* strnlen, which C99 does not have: the length of `s`, or `cap` when no NUL is in `cap` bytes */
static size_t bounded_len(const char* s, size_t cap)
{
    const char* nul = (const char*)memchr(s, 0, cap);
    return nul ? (size_t)(nul - s) : cap;
}

static void why_set(char* why, size_t nwhy, const char* text)
{
    if (why && nwhy) snprintf(why, nwhy, "%s", text);
}

/* A NUMBER key as the engine stores it in a byte: atoi's low bits (0x4C46C0, then a byte store). */
static unsigned char byte_of(const char* v)
{
    uint32_t bits = 0;
    tagpu_guifile_num(v, NULL, &bits);
    return (unsigned char)bits;
}

unsigned tagpu_buildmenu_pages(int orders_page, unsigned count, unsigned char out[TAGPU_BM_PAGES])
{
    unsigned n = 0, last, p;
    if (!count) return 0;
    if (orders_page) out[n++] = 0;
    last = count - 1 < TAGPU_BM_PAGES - 1 ? count - 1 : TAGPU_BM_PAGES - 1;
    for (p = 1; p <= last; p++) out[n++] = (unsigned char)p;
    return n;
}

int tagpu_buildmenu_page_name(char* out, size_t cap, const char* def_name, unsigned page)
{
    int n;
    if (!out || !cap || page >= TAGPU_BM_PAGES) return 0;
    n = snprintf(out, cap, "%.31s%u.GUI", def_name, page);
    return n > 0 && (size_t)n < cap;
}

int tagpu_buildmenu_gui_path(char* out, size_t cap, const char* prefix, const char* name)
{
    size_t plen = bounded_len(prefix, 0x100), nlen, len, i;
    if (plen >= 0x100 || !out) return 0;
    nlen = strlen(name);
    if (plen + nlen + 4 >= cap) return 0;      /* + ".GUI" + NUL; a cut only shortens it */
    memcpy(out, prefix, plen);
    memcpy(out + plen, name, nlen);
    len = plen + nlen;
    out[len] = 0;
    for (i = len; i-- > 0;) {
        if (out[i] == '\\') break;
        if (out[i] == '.') { out[i] = 0; len = i; break; }
    }
    memcpy(out + len, ".GUI", 5);
    return 1;
}

/* Step 1: records 0..count-1 of the parse, one a top-level section. */
static void read_records(const TAGPU_GUIFILE* f, Rec* rec, int count)
{
    int i;
    for (i = 0; i < count; i++) {
        const int sec = tagpu_guifile_child(f, 0, i);
        const int com = tagpu_guifile_find(f, sec, "COMMON");
        const char* name;
        memset(&rec[i], 0, sizeof rec[i]);
        rec[i].grey = byte_of(tagpu_guifile_get(f, sec, "grayedout")) & 1u;
        if (com < 0) continue;                 /* 0x4AD361: no COMMON leaves the record zero, id 0 */
        rec[i].id = byte_of(tagpu_guifile_get(f, com, "id"));
        rec[i].active = byte_of(tagpu_guifile_get(f, com, "active"));
        rec[i].common = byte_of(tagpu_guifile_get(f, com, "commonattribs"));
        name = tagpu_guifile_get(f, com, "name");
        if (name) snprintf(rec[i].name, REC_NAME, "%s", name);
    }
}

int tagpu_buildmenu_page(const unsigned char* blob, size_t n, unsigned page,
                         const TAGPU_BM_ENTRY* entries, unsigned nentries,
                         const TAGPU_BM_ENGINE* eng, unsigned char* offered, char* why, size_t nwhy)
{
    static const char* const subs[] = { "PREV", "NEXT", "ORDERS", "BUILD" };
    TAGPU_GUIFILE* f;
    Rec* rec;
    int count, last, i, marked = 0;
    unsigned e;
    if (!blob || !n) { why_set(why, nwhy, "an empty file, which the engine's load refuses"); return -1; }
    if (n > TAGPU_BM_FILE_CAP) { why_set(why, nwhy, "a file past 1 MiB"); return -1; }
    if (!(f = tagpu_guifile_parse(blob, n, why, nwhy))) return -1;
    count = tagpu_guifile_count(f, 0);
    if (count <= 0) {
        tagpu_guifile_free(f);
        why_set(why, nwhy, "no gadget");
        return -1;
    }
    if (!(rec = (Rec*)calloc((size_t)count, sizeof *rec))) {
        tagpu_guifile_free(f);
        why_set(why, nwhy, "out of memory");
        return -1;
    }
    read_records(f, rec, count);
    tagpu_guifile_free(f);
    last = count - 1;                          /* N, HEADER's count (0x4AED48) */

    /* Step 2: the fill. */
    for (e = 0; e < nentries; e++) {
        const TAGPU_BM_ENTRY* en = &entries[e];
        const unsigned slot = (unsigned)en->button + 4u;
        const size_t len = bounded_len(en->name, TAGPU_BM_ENTRY_NAME);
        if (!en->menu || (unsigned)en->menu - 1u != page) continue;
        if (slot > (unsigned)last || len >= TAGPU_BM_ENTRY_NAME) continue;
        rec[slot].grey = 0;
        rec[slot].common = 4;
        memcpy(rec[slot].name, en->name, len);
        rec[slot].name[len] = 0;
    }

    /* Step 3: the greyed pass, records 0..N-1. */
    if (page)
        for (i = 0; i < last; i++)
            if (rec[i].common & 4u) rec[i].grey = eng->type_of(eng->ctx, rec[i].name) == 0;

    /* Step 4: what a press arms, gadgets 1..N. */
    for (i = 1; i <= last; i++) {
        char press[PRESS_NAME + 1];
        unsigned t, k;
        if (rec[i].id != 1 || !rec[i].active || rec[i].grey) continue;
        snprintf(press, sizeof press, "%.16s", rec[i].name);
        for (k = 0; k < sizeof subs / sizeof subs[0] && !strstr(press, subs[k]); k++) {}
        if (k < sizeof subs / sizeof subs[0]) continue;
        t = eng->type_of(eng->ctx, press);
        if (!t || t >= eng->ndefs || !eng->armable(eng->ctx, t) || offered[t]) continue;
        offered[t] = 1;
        marked++;
    }
    free(rec);
    return marked;
}
