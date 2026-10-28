/* wikitexttest.c -- the C half of the wiki text's parity check, for tools/wiki/textparity.mjs: the
   real tagpu_wikitext.c over tagpu_wikidata.c's reading of the archive the DLL embeds, compiled
   natively (or for Windows, run under Wine), writes every fill it makes as a JSON line, and
   textparity.mjs makes the same fills with text.js and compares the bytes.

   `wikitexttest PAK CASTINGS EDGES` loads PAK, an archive of art/wiki/ (tools/hudpak.py --set wiki),
   as the DLL loads WIKIPAK; CASTINGS is a JSON array whose items are a side's casting record or
   null (no casting); EDGES a JSON array of strings. For every entry, every casting and the
   interface values 0, 1, 2 and -1 it fills each text the entry shows -- its title, subtitle and
   paragraphs, each demo's title and every caption its inputs carry -- and each edge string, into a
   buffer far longer than any of them. Then, for every entry and casting at interface 1, it fills
   each of those texts again into buffers of the sizes that test the cut: 0 to 3 bytes, the fill's
   length - 3 to its length + 1, and every size that makes the cut fall inside a character. One
   line per fill: {"e": the entry's id, "w": where the text stands, "c": the casting's index, "i":
   the interface value, "n": the buffer's size, "cut": 0|1, "t": the bytes as hex}. A casting the
   reader refuses prints {"refused": index, "why"} and is not filled with.

   Exit 0 when everything loaded; the comparison is textparity.mjs's. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_json.h"
#include "tagpu_wikidata.h"
#include "tagpu_wikitext.h"

#ifdef _WIN32
#include <windows.h>
/* tagpu_pak.c's resource lookup links against the DLL's module handle; this test reads its archive
   from a file instead and never calls it */
HMODULE g_ddraw_module;
#endif

#define WHOLE 65536     /* the buffer that holds any fill whole: the longest text and its words */

static char s_out[WHOLE];

static char* slurp(const char* path, size_t* n)
{
    FILE* f = fopen(path, "rb");
    char* p;
    long len;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0 || !(p = (char*)malloc((size_t)len + 1)) || fread(p, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        return NULL;
    }
    fclose(f);
    p[len] = 0;
    *n = (size_t)len;
    return p;
}

static void jstr(const char* s)
{
    putchar('"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') printf("\\%c", c);
        else if (c < 0x20 || c >= 0x7F) printf("\\u%04x", c);
        else putchar(c);
    }
    putchar('"');
}

/* one fill of `raw` into the first `n` bytes of s_out, and its line */
static void fill(const char* id, const char* where, int ci, int iface, const WIKIWORDS* w, const char* raw, size_t n)
{
    const int cut = tagpu_wikitext_fill(raw, w, s_out, n);
    const char* p;
    printf("{\"e\":");
    jstr(id);
    printf(",\"w\":");
    jstr(where);
    printf(",\"c\":%d,\"i\":%d,\"n\":%lu,\"cut\":%d,\"t\":\"", ci, iface, (unsigned long)n, cut);
    for (p = n ? s_out : ""; *p; p++) printf("%02x", (unsigned char)*p);
    printf("\"}\n");
}

/* `raw` at every size of the cut's cases: the whole fill's length decides them */
static void fill_cuts(const char* id, const char* where, int ci, const WIKIWORDS* w, const char* raw)
{
    static char whole[WHOLE];
    size_t len, n;
    tagpu_wikitext_fill(raw, w, whole, WHOLE);
    len = strlen(whole);
    for (n = 0; n <= len + 1; n++) {
        const int in_char = n >= 1 && n - 1 < len && ((unsigned char)whole[n - 1] & 0xC0) == 0x80;
        if (n <= 3 || n + 3 >= len || in_char) fill(id, where, ci, 1, w, raw, n);
    }
}

typedef void (*EACH)(const char* id, const char* where, int ci, int iface, const WIKIWORDS* w, const char* raw);

static void each_whole(const char* id, const char* where, int ci, int iface, const WIKIWORDS* w, const char* raw)
{
    fill(id, where, ci, iface, w, raw, WHOLE);
}

static void each_cut(const char* id, const char* where, int ci, int iface, const WIKIWORDS* w, const char* raw)
{
    (void)iface;
    fill_cuts(id, where, ci, w, raw);
}

/* every text entry `e` shows, and every edge string, through `f` */
static void texts(const WIKIENTRY* e, const TJV* edges, int ci, int iface, const WIKIWORDS* w, EACH f)
{
    char where[96];
    const TJV* s;
    int i, j;
    f(e->id, "title", ci, iface, w, e->title);
    f(e->id, "subtitle", ci, iface, w, e->subtitle);
    for (i = 0; i < e->ntext; i++) {
        snprintf(where, sizeof where, "text[%d]", i);
        f(e->id, where, ci, iface, w, e->text[i]);
    }
    for (i = 0; i < e->ndemo; i++) {
        const WIKIDEMO* d = &e->demo[i];
        snprintf(where, sizeof where, "demos[%d].title", i);
        f(e->id, where, ci, iface, w, d->title);
        for (j = 0; j < d->ninput; j++)
            if (d->input[j].has & WIKI_IN_CAPTION) {
                snprintf(where, sizeof where, "demos[%d].inputs[%d].caption", i, j);
                f(e->id, where, ci, iface, w, d->input[j].caption);
            }
    }
    for (s = edges->first, i = 0; s; s = s->next, i++) {
        snprintf(where, sizeof where, "edge[%d]", i);
        f(e->id, where, ci, iface, w, s->str);
    }
}

static TJDOC* parse_file(const char* path, int type, const TJV** root)
{
    char err[160] = "unreadable";
    size_t n;
    char* text = slurp(path, &n);
    TJDOC* doc = text ? tagpu_json_parse(text, n, err, sizeof err) : NULL;
    free(text);
    if (doc && tj_is(*root = tagpu_json_root(doc), type)) return doc;
    fprintf(stderr, "wikitexttest: %s is not a JSON array: %s\n", path, doc ? "another value" : err);
    if (doc) tagpu_json_free(doc);
    return NULL;
}

int main(int argc, char** argv)
{
    static const int IFACE[] = { 0, 1, 2, -1 };
    char err[256];
    const TJV *castings, *edges, *c;
    TJDOC *cdoc, *edoc;
    WIKICASTING* cast;
    int* ok;
    WIKIDATA* d;
    size_t n;
    unsigned char* pak;
    int nc, ci, i, k;
    if (argc != 4) {
        fprintf(stderr, "usage: wikitexttest PAK CASTINGS EDGES\n");
        return 2;
    }
    if (!(pak = (unsigned char*)slurp(argv[1], &n))) {
        fprintf(stderr, "wikitexttest: cannot read %s\n", argv[1]);
        return 2;
    }
    if (!(d = tagpu_wikidata_load(pak, n, err, sizeof err))) {
        fprintf(stderr, "wikitexttest: the archive is refused: %s\n", err);
        return 2;
    }
    if (!(cdoc = parse_file(argv[2], TJ_ARR, &castings)) || !(edoc = parse_file(argv[3], TJ_ARR, &edges))) return 2;
    for (c = edges->first; c; c = c->next)
        if (!tj_is(c, TJ_STR)) {
            fprintf(stderr, "wikitexttest: an edge string is not a string\n");
            return 2;
        }
    nc = castings->n;
    cast = (WIKICASTING*)calloc((size_t)(nc ? nc : 1), sizeof *cast);
    ok = (int*)calloc((size_t)(nc ? nc : 1), sizeof *ok);
    if (!cast || !ok) return 2;
    for (c = castings->first, ci = 0; c; c = c->next, ci++) {
        if (tj_is(c, TJ_NULL)) continue;
        if (tagpu_wikidata_casting(c, &cast[ci], err, sizeof err)) ok[ci] = 1;
        else {
            printf("{\"refused\":%d,\"why\":", ci);
            jstr(err);
            printf("}\n");
            ok[ci] = -1;
        }
    }
    for (i = 0; i < d->nentry; i++)
        for (ci = 0; ci < nc; ci++) {
            WIKIWORDS w;
            if (ok[ci] < 0) continue;
            for (k = 0; k < (int)(sizeof IFACE / sizeof *IFACE); k++) {
                tagpu_wikitext_words(&w, &d->entry[i], ok[ci] ? &cast[ci] : NULL, IFACE[k]);
                texts(&d->entry[i], edges, ci, IFACE[k], &w, each_whole);
            }
            tagpu_wikitext_words(&w, &d->entry[i], ok[ci] ? &cast[ci] : NULL, 1);
            texts(&d->entry[i], edges, ci, 1, &w, each_cut);
        }
    free(cast);
    free(ok);
    tagpu_json_free(cdoc);
    tagpu_json_free(edoc);
    tagpu_wikidata_free(d);
    free(pak);
    return 0;
}
