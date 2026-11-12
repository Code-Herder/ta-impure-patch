#ifndef WIKICOBFILE_H
#define WIKICOBFILE_H
/* wikicobfile.h -- the units' scripts and sounds the wiki's test drivers read from files where the
   DLL reads the game's archives: a table of types, COBFILES, and the WIKICOBSRC and WIKISNDSRC over
   it, for tests/wikihost.c, tests/wikitest.c and tests/wikistagetest.c, each of which includes this
   once. Two readers fill a table: cob_packs reads the gfx packs tools/wiki/gfx.py writes, and
   cob_load the COB file, the same facts gathered in one JSON object. Each script is read once and
   lives with its table; a sim copies what it takes during tagpu_wikisim_init (WIKISIMCTX). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_json.h"
#include "tagpu_wikisim.h"

/* a whole file, NUL-terminated, its length in `*n`; NULL when it cannot be read */
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

/* One type: its script and its posed model's facts (visible flags, and the model's shape, gfx.py
   `model_shape`: offsets, parents and top), or none; its sound category and variant counts, or
   none. Offsets or parents not one a primitive are none, as tools/wiki/serve.py `shape` takes
   them. A model past WIKICOB_PIECES is one the DLL's 3D load refuses, so its type has no script. */
typedef struct {
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
} COBFILE;

typedef struct COBFILES {
    COBFILE*   t;
    int        n, cap;
    WIKICOBSRC cob;                 /* over this table: what a WIKISIMCTX's cob points at */
    WIKISNDSRC snd;                 /* and its snd */
} COBFILES;

static int cob_type(void* ctx, const char* name, WIKICOBTYPE* out)
{
    const COBFILES* f = (const COBFILES*)ctx;
    int i;
    for (i = 0; i < f->n; i++)
        if (!strcmp(f->t[i].name, name) && f->t[i].cob) {
            const COBFILE* c = &f->t[i];
            out->cob = c->cob;
            out->ncob = c->ncob;
            out->pieces = c->pieces;
            out->visible = c->visible;
            out->offset = c->noffset == c->pieces ? c->offset : NULL;
            out->parent = c->nparent == c->pieces ? c->parent : NULL;
            out->top = c->top;
            return 1;
        }
    return 0;
}

static int snd_type(void* ctx, const char* name, WIKISNDTYPE* out)
{
    const COBFILES* f = (const COBFILES*)ctx;
    int i;
    for (i = 0; i < f->n; i++)
        if (!strcmp(f->t[i].name, name) && f->t[i].has_snd) {
            *out = f->t[i].snd;
            return 1;
        }
    return 0;
}

static void cob_init(COBFILES* f)
{
    memset(f, 0, sizeof *f);
    f->cob.ctx = f;
    f->cob.type = cob_type;
    f->snd.ctx = f;
    f->snd.type = snd_type;
}

static void cob_free(COBFILES* f)
{
    int i;
    for (i = 0; i < f->n; i++) free(f->t[i].cob);
    free(f->t);
    f->t = NULL;
    f->n = f->cap = 0;
}

/* the type `name`'s record, added empty when the table has none; NULL when memory runs out or the
   name is too long */
static COBFILE* cob_slot(COBFILES* f, const char* name)
{
    int i;
    if (strlen(name) >= WIKI_ID_MAX) return NULL;
    for (i = 0; i < f->n; i++)
        if (!strcmp(f->t[i].name, name)) return &f->t[i];
    if (f->n == f->cap) {
        const int c = f->cap ? 2 * f->cap : 32;
        COBFILE* t = (COBFILE*)realloc(f->t, (size_t)c * sizeof *t);
        if (!t) return NULL;
        f->t = t;
        f->cap = c;
    }
    memset(&f->t[f->n], 0, sizeof f->t[f->n]);
    snprintf(f->t[f->n].name, WIKI_ID_MAX, "%s", name);
    return &f->t[f->n++];
}

/* a sound {cat, count}: the simulation reads no clip, so `death` is none */
static void cob_sound(COBFILE* c, const TJV* snd)
{
    const TJV* e;
    int k;
    c->has_snd = 1;
    c->snd.cat = (int)tj_num(tj_get(snd, "cat"), -1);
    c->snd.death = -1;
    for (k = 0, e = tj_is(tj_get(snd, "count"), TJ_ARR) ? tj_get(snd, "count")->first : NULL; e && k < WIKI_SND_KINDS; e = e->next, k++)
        c->snd.count[k] = (int32_t)tj_num(e, 0);
}

/* the script at `path` and the model facts `s` carries ({visible, offset, parent, top}); 1, or 0
   with why in `err` when the script cannot be read */
static int cob_script(COBFILE* c, const char* path, const TJV* s, char* err, size_t errn)
{
    const TJV *vis = tj_get(s, "visible"), *e, *w;
    int k = 0;
    for (e = tj_is(vis, TJ_ARR) ? vis->first : NULL; e; e = e->next) k++;
    if (k > WIKICOB_PIECES) return 1;
    if (!(c->cob = (unsigned char*)slurp(path, &c->ncob))) {
        snprintf(err, errn, "cannot read %.200s", path);
        return 0;
    }
    for (e = tj_is(vis, TJ_ARR) ? vis->first : NULL; e; e = e->next) c->visible[c->pieces++] = (unsigned char)tj_num(e, 0);
    for (e = tj_is(tj_get(s, "offset"), TJ_ARR) ? tj_get(s, "offset")->first : NULL; e && c->noffset < WIKICOB_PIECES; e = e->next) {
        for (k = 0, w = tj_is(e, TJ_ARR) ? e->first : NULL; w && k < 3; w = w->next, k++)
            c->offset[3 * c->noffset + k] = (int32_t)tj_num(w, 0);
        c->noffset++;
    }
    for (e = tj_is(tj_get(s, "parent"), TJ_ARR) ? tj_get(s, "parent")->first : NULL; e && c->nparent < WIKICOB_PIECES; e = e->next)
        c->parent[c->nparent++] = (int16_t)tj_num(e, -1);
    c->top = (int32_t)tj_num(tj_get(s, "top"), 0);
    return 1;
}

/* THE PACKS: each folder's scene.json, units.meshes[name], its `script` ({file, visible, offset,
   parent, top}, the file relative to the folder) and its `sound` ({cat, count}); a type's script
   from the first folder whose mesh has one, and its sound likewise, each on its own. A folder
   without a scene.json holds nothing. 1, or 0 with why in `err`. */
static inline int cob_packs(COBFILES* f, const char* const* dirs, int n, char* err, size_t errn)
{
    int i, ok = 1;
    for (i = 0; ok && i < n; i++) {
        char path[1024];
        size_t len;
        char* text;
        TJDOC* doc;
        const TJV* m;
        snprintf(path, sizeof path, "%s/scene.json", dirs[i]);
        if (!(text = slurp(path, &len))) continue;
        if (!(doc = tagpu_json_parse(text, len, err, errn))) { free(text); return 0; }
        m = tj_get(tj_get(tagpu_json_root(doc), "units"), "meshes");
        for (m = tj_is(m, TJ_OBJ) ? m->first : NULL; ok && m; m = m->next) {
            const TJV *s = tj_get(m, "script"), *snd = tj_get(m, "sound");
            COBFILE* c;
            if (!tj_is(s, TJ_OBJ) && !tj_is(snd, TJ_OBJ)) continue;
            if (!(c = cob_slot(f, m->key))) { snprintf(err, errn, "%.200s: a mesh name too long, or out of memory", path); ok = 0; break; }
            if (tj_is(snd, TJ_OBJ) && !c->has_snd) cob_sound(c, snd);
            if (tj_is(s, TJ_OBJ) && !c->cob && tj_is(tj_get(s, "file"), TJ_STR)) {
                char cob[1024];
                snprintf(cob, sizeof cob, "%s/%s", dirs[i], tj_get(s, "file")->str);
                ok = cob_script(c, cob, s, err, errn);
            }
        }
        tagpu_json_free(doc);
        free(text);
    }
    return ok;
}

/* THE COB FILE (tests/wikistagetest.c's `ops` takes it): a JSON object of type name -> {"cob": the
   path of its script, "visible", "offset", "parent", "top"} and maybe "sound": {"cat", "count"};
   a type with a sound and no "cob" has no script, a type it does not name neither. 1, or 0 with a
   message on stderr. */
static inline int cob_load(COBFILES* f, const char* path)
{
    char err[256];
    size_t n;
    char* text = slurp(path, &n);
    TJDOC* doc = text ? tagpu_json_parse(text, n, err, sizeof err) : NULL;
    const TJV* c;
    int ok = doc != NULL;
    if (!doc) fprintf(stderr, "wiki tests: %s: %s\n", path, text ? err : "cannot read it");
    for (c = doc ? tagpu_json_root(doc)->first : NULL; ok && c; c = c->next) {
        const TJV *cob = tj_get(c, "cob"), *snd = tj_get(c, "sound");
        COBFILE* t;
        if ((cob && !tj_is(cob, TJ_STR)) || (!cob && !snd) || !(t = cob_slot(f, c->key))) {
            fprintf(stderr, "wiki tests: %s: %s is not a script or a sound\n", path, c->key);
            ok = 0;
            break;
        }
        if (tj_is(snd, TJ_OBJ)) cob_sound(t, snd);
        if (cob && !cob_script(t, cob->str, c, err, sizeof err)) {
            fprintf(stderr, "wiki tests: %s\n", err);
            ok = 0;
        }
    }
    tagpu_json_free(doc);
    free(text);
    return ok;
}

#endif
