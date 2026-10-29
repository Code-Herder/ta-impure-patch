#ifndef WIKICOBFILE_H
#define WIKICOBFILE_H
/* wikicobfile.h -- the units' scripts and sounds the wiki's test drivers read from files where the
   DLL reads the game's archives: the COB file and the WIKICOBSRC and WIKISNDSRC over it, for
   tests/wikitest.c and
   tests/wikistagetest.c, each of which defines slurp() (read a whole file, NUL-terminated, NULL on
   failure) and includes this once. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_json.h"
#include "tagpu_wikisim.h"

static char* slurp(const char* path, size_t* n);

/* COB, the file wikitest's `demos` and `trace` and wikistagetest's `ops` take: a JSON object of type name -> {"cob": the path of its
   script, "visible": its posed model's visible flags, "offset", "parent" and "top": its model's
   shape (tools/wiki/gfx.py `model_shape`)}, which simparity.mjs writes from the gfx packs sim.js's
   host reads the same scripts from. Offsets or parents not one a primitive are none, as
   tools/wiki/serve.py `shape` takes them. A type it does not name has no script. Each script is
   read once and lives until the run ends. A type may carry "sound": {"cat", "count": the
   WIKI_SND_KINDS variant counts} (tools/wiki/gfx.py `write_sounds`, the pack's sounds.json), and a
   type with a sound and no "cob" has no script; a type with no sound is silent. */
#define COB_TYPES 64
static struct {
    char           name[WIKI_ID_MAX];
    unsigned char* cob;
    size_t         ncob;
    unsigned char  visible[WIKICOB_PIECES];
    int            pieces;
    int32_t        offset[WIKICOB_PIECES * 3];
    int16_t        parent[WIKICOB_PIECES];
    int            noffset, nparent;
    int32_t        top;
    int            has_snd;
    WIKISNDTYPE    snd;
} s_cob[COB_TYPES];
static int s_ncob;

static int cob_load(const char* path)
{
    char err[256];
    size_t n;
    char* text = slurp(path, &n);
    TJDOC* doc = text ? tagpu_json_parse(text, n, err, sizeof err) : NULL;
    const TJV* c;
    int ok = doc != NULL;
    if (!doc) fprintf(stderr, "wiki tests: %s: %s\n", path, text ? err : "cannot read it");
    for (c = doc ? tagpu_json_root(doc)->first : NULL; ok && c; c = c->next) {
        const TJV *f = tj_get(c, "cob"), *vis = tj_get(c, "visible"), *off = tj_get(c, "offset"), *e, *w;
        const TJV* snd = tj_get(c, "sound");
        int k = 0;
        if (s_ncob == COB_TYPES || strlen(c->key) >= WIKI_ID_MAX || (f && !tj_is(f, TJ_STR)) || (!f && !snd)) {
            fprintf(stderr, "wiki tests: %s: %s is not a script or a sound\n", path, c->key);
            ok = 0;
            break;
        }
        memset(&s_cob[s_ncob], 0, sizeof s_cob[s_ncob]);
        snprintf(s_cob[s_ncob].name, WIKI_ID_MAX, "%s", c->key);
        if (tj_is(snd, TJ_OBJ)) {
            s_cob[s_ncob].has_snd = 1;
            s_cob[s_ncob].snd.cat = (int)tj_num(tj_get(snd, "cat"), -1);
            for (k = 0, e = tj_is(tj_get(snd, "count"), TJ_ARR) ? tj_get(snd, "count")->first : NULL; e && k < WIKI_SND_KINDS;
                 e = e->next, k++)
                s_cob[s_ncob].snd.count[k] = (int32_t)tj_num(e, 0);
        }
        /* a model past WIKICOB_PIECES is one the DLL's 3D load refuses, so its type has no script */
        for (k = 0, e = tj_is(vis, TJ_ARR) ? vis->first : NULL; e; e = e->next) k++;
        if (!f || k > WIKICOB_PIECES) {
            s_ncob++;
            continue;
        }
        if (!(s_cob[s_ncob].cob = (unsigned char*)slurp(f->str, &s_cob[s_ncob].ncob))) {
            fprintf(stderr, "wiki tests: cannot read %s\n", f->str);
            ok = 0;
            break;
        }
        for (e = tj_is(vis, TJ_ARR) ? vis->first : NULL; e; e = e->next)
            s_cob[s_ncob].visible[s_cob[s_ncob].pieces++] = (unsigned char)tj_num(e, 0);
        for (e = tj_is(off, TJ_ARR) ? off->first : NULL; e && s_cob[s_ncob].noffset < WIKICOB_PIECES; e = e->next) {
            for (k = 0, w = tj_is(e, TJ_ARR) ? e->first : NULL; w && k < 3; w = w->next, k++)
                s_cob[s_ncob].offset[3 * s_cob[s_ncob].noffset + k] = (int32_t)tj_num(w, 0);
            s_cob[s_ncob].noffset++;
        }
        for (e = tj_is(tj_get(c, "parent"), TJ_ARR) ? tj_get(c, "parent")->first : NULL;
             e && s_cob[s_ncob].nparent < WIKICOB_PIECES; e = e->next)
            s_cob[s_ncob].parent[s_cob[s_ncob].nparent++] = (int16_t)tj_num(e, -1);
        s_cob[s_ncob].top = (int32_t)tj_num(tj_get(c, "top"), 0);
        s_ncob++;
    }
    tagpu_json_free(doc);
    free(text);
    return ok;
}

static int cob_type(void* ctx, const char* name, WIKICOBTYPE* out)
{
    int i;
    (void)ctx;
    for (i = 0; i < s_ncob; i++)
        if (!strcmp(s_cob[i].name, name) && s_cob[i].cob) {
            out->cob = s_cob[i].cob;
            out->ncob = s_cob[i].ncob;
            out->pieces = s_cob[i].pieces;
            out->visible = s_cob[i].visible;
            out->offset = s_cob[i].noffset == s_cob[i].pieces ? s_cob[i].offset : NULL;
            out->parent = s_cob[i].nparent == s_cob[i].pieces ? s_cob[i].parent : NULL;
            out->top = s_cob[i].top;
            return 1;
        }
    return 0;
}

static const WIKICOBSRC COB_SRC = { NULL, cob_type };

static int snd_type(void* ctx, const char* name, WIKISNDTYPE* out)
{
    int i;
    (void)ctx;
    for (i = 0; i < s_ncob; i++)
        if (!strcmp(s_cob[i].name, name) && s_cob[i].has_snd) {
            *out = s_cob[i].snd;
            return 1;
        }
    return 0;
}

static const WIKISNDSRC SND_SRC = { NULL, snd_type };

static void cob_free(void)
{
    while (s_ncob) free(s_cob[--s_ncob].cob);
}

#endif
