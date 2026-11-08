/* tagpu_modcat.c -- Contract: tagpu_modcat.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_modcat.h"
#include "tagpu_json.h"

static int flag(const TJV* v, int* out)
{
    if (tj_is(v, TJ_TRUE)) *out = 1;
    else if (tj_is(v, TJ_FALSE)) *out = 0;
    else return 0;
    return 1;
}

/* an option's allowed and, when it is off, its reason; 0 when either is missing */
static int option(const TJV* o, int* allowed, char* reason)
{
    const char* r = tj_str(tj_get(o, "reason"), "");
    reason[0] = 0;
    if (!flag(tj_get(o, "allowed"), allowed)) return 0;
    if (!*allowed && !*r) return 0;
    snprintf(reason, MODCAT_REASON, "%s", r);
    return 1;
}

static int bad(char* err, size_t errn, const char* id, const char* what)
{
    if (err && errn) snprintf(err, errn, "catalog: %s: %s", id, what);
    return -1;
}

static int whole(const TJV* v, int lo, int hi, int* out)
{
    if (!tj_is(v, TJ_NUM) || v->num != (double)(int)v->num || v->num < lo || v->num > hi) return 0;
    *out = (int)v->num;
    return 1;
}

int tagpu_modcat_rules(const char* catalog, size_t n, const char* id, MODRULES* out, char* err, size_t errn)
{
    char e[160];
    TJDOC* d;
    const TJV* m;
    int ret = 0;
    if (err && errn) err[0] = 0;
    if (!(d = tagpu_json_parse(catalog, n, e, sizeof e))) {
        if (err && errn) snprintf(err, errn, "catalog: %s", e);
        return -1;
    }
    m = tj_get(tagpu_json_root(d), "mods");
    for (m = tj_is(m, TJ_ARR) ? m->first : NULL; m; m = m->next) {
        const TJV* opts, *snap, *quick;
        if (strcmp(tj_str(tj_get(m, "id"), ""), id)) continue;
        memset(out, 0, sizeof *out);
        opts = tj_get(m, "options");
        snap = tj_get(opts, "resource_snapping");
        quick = tj_get(opts, "quick_build");
        if (!option(snap, &out->manual, out->manual_reason) ||
            !whole(tj_get(snap, "default"), 0, MODRULES_RADIUS_MAX, &out->radius) ||
            !whole(tj_get(snap, "max"), 0, MODRULES_RADIUS_MAX, &out->maximum) || out->radius > out->maximum)
            ret = bad(err, errn, id, "resource_snapping is missing or out of range");
        else if (!option(quick, &out->quick, out->quick_reason) || !flag(tj_get(quick, "default"), &out->quick_default))
            ret = bad(err, errn, id, "quick_build is missing or out of range");
        else
            ret = 1;
        break;
    }
    tagpu_json_free(d);
    return ret;
}

static int same_name(const char* a, const char* b)
{
    for (; *a && *b; a++, b++) {
        int x = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a, y = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
        if (x != y) return 0;
    }
    return *a == *b;
}

int tagpu_modcat_hidden(const char* catalog, size_t n, const char* id, const char* name)
{
    char e[160];
    TJDOC* d = tagpu_json_parse(catalog, n, e, sizeof e);
    const TJV* m;
    int hit = 0;
    if (!d) return 0;
    m = tj_get(tagpu_json_root(d), "mods");
    for (m = tj_is(m, TJ_ARR) ? m->first : NULL; m; m = m->next) {
        const TJV* h;
        if (strcmp(tj_str(tj_get(m, "id"), ""), id)) continue;
        h = tj_get(m, "hide");
        for (h = tj_is(h, TJ_ARR) ? h->first : NULL; h; h = h->next)
            if (same_name(tj_str(h, ""), name)) hit = 1;
        break;
    }
    tagpu_json_free(d);
    return hit;
}

int tagpu_modcat_about(const char* catalog, size_t n, const char* id, MODABOUT* out)
{
    char e[160];
    TJDOC* d = tagpu_json_parse(catalog, n, e, sizeof e);
    const TJV* m;
    int hit = 0;
    if (!d) return 0;
    m = tj_get(tagpu_json_root(d), "mods");
    for (m = tj_is(m, TJ_ARR) ? m->first : NULL; m && !hit; m = m->next) {
        if (strcmp(tj_str(tj_get(m, "id"), ""), id)) continue;
        snprintf(out->name, sizeof out->name, "%s", tj_str(tj_get(m, "name"), id));
        snprintf(out->author, sizeof out->author, "%s", tj_str(tj_get(m, "author"), ""));
        snprintf(out->version, sizeof out->version, "%s", tj_str(tj_get(m, "version"), ""));
        snprintf(out->summary, sizeof out->summary, "%s", tj_str(tj_get(m, "summary"), ""));
        out->is_mutator = !strcmp(tj_str(tj_get(m, "kind"), ""), "mutator");
        snprintf(out->base, sizeof out->base, "%s", tj_str(tj_get(m, "base"), ""));
        out->hosted = tj_str(tj_get(m, "base_url"), "")[0] != 0;
        out->memory = !strcmp(tj_str(tj_get(m, "memory"), ""), "4gb") ? MODCAT_MEMORY_4GB :
                      !strcmp(tj_str(tj_get(m, "memory"), ""), "bit-off") ? MODCAT_MEMORY_BIT_OFF : MODCAT_MEMORY_LEAVE;
        hit = 1;
    }
    tagpu_json_free(d);
    return hit;
}

static char* copy_text(const char* t)
{
    size_t n = strlen(t) + 1;
    char* c = (char*)malloc(n);
    if (c) memcpy(c, t, n);
    return c;
}

int tagpu_modcat_hash(const char* t, uint8_t out[32])
{
    int i;
    if (strlen(t) != 64) return 0;
    for (i = 0; i < 64; i++) {
        char c = t[i];
        int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (v < 0) return 0;
        if (i & 1) out[i / 2] |= (uint8_t)v;
        else out[i / 2] = (uint8_t)(v << 4);
    }
    return 1;
}

static int plain_id(const char* id)
{
    if (!*id) return 0;
    for (; *id; id++)
        if (!((*id >= 'a' && *id <= 'z') || (*id >= '0' && *id <= '9') || *id == '-' || *id == '_')) return 0;
    return 1;
}

/* a size that fits every 32-bit count the install keeps: 0 when it is not a whole number in range */
static int size_of(const TJV* v, uint32_t* out)
{
    double d = tj_num(v, -1);
    if (d < 0 || d > 2147483647.0 || d != (double)(uint32_t)d) return 0;
    *out = (uint32_t)d;
    return 1;
}

static int count_of(const TJV* arr)
{
    int n = 0;
    const TJV* v;
    for (v = tj_is(arr, TJ_ARR) ? arr->first : NULL; v; v = v->next) n++;
    return n;
}

void tagpu_modcat_install_free(MODINSTALL* m)
{
    int i, k;
    for (i = 0; i < m->npackages; i++) {
        for (k = 0; k < m->packages[i].nfiles; k++) free((void*)m->packages[i].files[k].path);
        free(m->packages[i].files);
        free(m->packages[i].url);
    }
    free(m->packages);
    free(m->id);
    free(m->version);
    free(m->base_url);
    memset(m, 0, sizeof *m);
}

int tagpu_modcat_install(const char* catalog, size_t n, const char* id, MODINSTALL* out, char* err, size_t errn)
{
    char e[160];
    TJDOC* d = tagpu_json_parse(catalog, n, e, sizeof e);
    const TJV* m, *pk;
    int i, ok = 0;

    memset(out, 0, sizeof *out);
    if (!d) { if (err && errn) snprintf(err, errn, "catalog: %s", e); return 0; }
    if (!plain_id(id)) { bad(err, errn, id, "not a plain name"); goto done; }
    m = tj_get(tagpu_json_root(d), "mods");
    for (m = tj_is(m, TJ_ARR) ? m->first : NULL; m; m = m->next)
        if (!strcmp(tj_str(tj_get(m, "id"), ""), id)) break;
    if (!m) { bad(err, errn, id, "no entry names it"); goto done; }

    pk = tj_get(m, "packages");
    out->id = copy_text(id);
    out->version = copy_text(tj_str(tj_get(m, "version"), ""));
    out->is_mutator = !strcmp(tj_str(tj_get(m, "kind"), ""), "mutator");
    out->base_url = copy_text(tj_str(tj_get(m, "base_url"), ""));
    /* each count set only with its array: the free walks them */
    out->packages = (MODPKG*)calloc((size_t)count_of(pk) + 1, sizeof(MODPKG));
    if (!out->id || !out->version || !out->base_url || !out->packages) { bad(err, errn, id, "out of memory"); goto done; }
    out->npackages = count_of(pk);
    if (!out->npackages) { bad(err, errn, id, "it has no package"); goto done; }
    if (!out->version[0]) { bad(err, errn, id, "it has no version"); goto done; }

    for (i = 0, pk = pk->first; pk; pk = pk->next, i++) {
        MODPKG* p = &out->packages[i];
        const TJV* f = tj_get(pk, "files");
        int k;
        p->url = copy_text(tj_str(tj_get(pk, "url"), ""));
        p->files = (MODTAR_FILE*)calloc((size_t)count_of(f) + 1, sizeof(MODTAR_FILE));
        if (!p->url || !p->files) { bad(err, errn, id, "out of memory"); goto done; }
        p->nfiles = count_of(f);
        if (!p->url[0] || !tagpu_modcat_hash(tj_str(tj_get(pk, "sha256"), ""), p->sha256) || !size_of(tj_get(pk, "size"), &p->size) || !p->nfiles) {
            bad(err, errn, id, "a package without its url, hash, size or files");
            goto done;
        }
        for (k = 0, f = f->first; f; f = f->next, k++) {
            MODTAR_FILE* t = &p->files[k];
            const char* path = tj_str(tj_get(f, "path"), "");
            if (!modtar_path_safe(path) || !tagpu_modcat_hash(tj_str(tj_get(f, "sha256"), ""), t->sha256) || !size_of(tj_get(f, "size"), &t->size)) {
                bad(err, errn, id, "a package file without a safe path, its hash or its size");
                goto done;
            }
            if (!(t->path = copy_text(path))) { bad(err, errn, id, "out of memory"); goto done; }
        }
    }
    ok = 1;
done:
    tagpu_json_free(d);
    if (!ok) tagpu_modcat_install_free(out);
    return ok;
}

int tagpu_modcat_each_id(const char* catalog, size_t n, void (*fn)(void* ctx, const char* id), void* ctx)
{
    char e[160];
    TJDOC* d = tagpu_json_parse(catalog, n, e, sizeof e);
    const TJV* m;
    if (!d) return 0;
    m = tj_get(tagpu_json_root(d), "mods");
    for (m = tj_is(m, TJ_ARR) ? m->first : NULL; m; m = m->next) {
        const char* id = tj_str(tj_get(m, "id"), "");
        if (plain_id(id)) fn(ctx, id);
    }
    tagpu_json_free(d);
    return 1;
}

/* ---- a mutator ---------------------------------------------------------------------------------- */

/* The catalog's spelling into the engine's. 0 for one a mount cannot act on or a profile's `file.<path>=` key cannot hold. */
static int engine_path(const char* in, char out[MODCAT_PATH])
{
    size_t i, n = strlen(in);
    if (!n || n >= MODCAT_PATH || !modtar_path_safe(in)) return 0;
    for (i = 0; i <= n; i++) {
        const char c = in[i];
        if (c == '=' || (c && (unsigned char)c < 0x20)) return 0;
        out[i] = c == '/' ? '\\' : c;
    }
    return 1;
}

void tagpu_modcat_mutator_free(MODMUTATOR* m)
{
    free(m->files);
    free(m->hides);
    memset(m, 0, sizeof *m);
}

int tagpu_modcat_mutator(const char* catalog, size_t n, const char* id, MODMUTATOR* out)
{
    char e[160];
    TJDOC* d = tagpu_json_parse(catalog, n, e, sizeof e);
    const TJV* m, *f, *h, *bh;
    int ok = 0, i;
    memset(out, 0, sizeof *out);
    if (!d) return 0;
    m = tj_get(tagpu_json_root(d), "mods");
    for (m = tj_is(m, TJ_ARR) ? m->first : NULL; m; m = m->next)
        if (!strcmp(tj_str(tj_get(m, "id"), ""), id)) break;
    if (!m || strcmp(tj_str(tj_get(m, "kind"), ""), "mutator") || !plain_id(id) || strlen(id) >= sizeof out->id) goto done;
    snprintf(out->id, sizeof out->id, "%s", id);
    snprintf(out->base, sizeof out->base, "%s", tj_str(tj_get(m, "base"), ""));
    if (!out->base[0]) goto done;
    f = tj_get(m, "files");
    h = tj_get(m, "hides");
    bh = tj_get(m, "base_hashes");
    out->files = (MODMUTFILE*)calloc((size_t)count_of(f) + 1, sizeof *out->files);
    out->hides = (char (*)[MODCAT_PATH])calloc((size_t)count_of(h) + 1, MODCAT_PATH);
    if (!out->files || !out->hides) goto done;
    for (i = 0, f = tj_is(f, TJ_ARR) ? f->first : NULL; f; f = f->next, i++) {
        const char* path = tj_str(f, "");
        const TJV* hash = tj_get(bh, path);
        if (!engine_path(path, out->files[i].path)) goto done;
        if (hash) {
            if (!tagpu_modcat_hash(tj_str(hash, ""), out->files[i].sha)) goto done;
            out->files[i].replaces = 1;
        }
        out->nfiles = i + 1;
    }
    for (i = 0, h = tj_is(h, TJ_ARR) ? h->first : NULL; h; h = h->next, i++) {
        if (!engine_path(tj_str(h, ""), out->hides[i])) goto done;
        out->nhides = i + 1;
    }
    ok = 1;
done:
    tagpu_json_free(d);
    if (!ok) tagpu_modcat_mutator_free(out);
    return ok;
}

MODSAMEAS* tagpu_modcat_same_as(const char* catalog, size_t n, const char* id, int* count)
{
    char e[160];
    TJDOC* d = tagpu_json_parse(catalog, n, e, sizeof e);
    const TJV* m, *s;
    MODSAMEAS* out = NULL;
    int i;
    *count = 0;
    if (!d) return NULL;
    m = tj_get(tagpu_json_root(d), "mods");
    for (m = tj_is(m, TJ_ARR) ? m->first : NULL; m; m = m->next)
        if (!strcmp(tj_str(tj_get(m, "id"), ""), id)) break;
    s = m ? tj_get(m, "same_as") : NULL;
    if (s && count_of(s) && (out = (MODSAMEAS*)calloc((size_t)count_of(s), sizeof *out))) {
        for (i = 0, s = s->first; s; s = s->next, i++)
            if (!engine_path(tj_str(tj_get(s, "path"), ""), out[i].path) || !tagpu_modcat_hash(tj_str(tj_get(s, "here"), ""), out[i].here) ||
                !tagpu_modcat_hash(tj_str(tj_get(s, "dropin"), ""), out[i].dropin)) {
                free(out);
                out = NULL;
                break;
            }
        if (out) *count = i;
    }
    tagpu_json_free(d);
    return out;
}

int tagpu_modcat_same_path(const char* a, const char* b)
{
    for (;; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x == '/') x = '\\';
        if (y == '/') y = '\\';
        if (x != y) return 0;
        if (!x) return 1;
    }
}

int tagpu_modcat_stale(const MODMUTATOR* m, MODCAT_HASH hash, void* ctx, char (*out)[MODCAT_PATH], int max)
{
    int i, total = 0;
    for (i = 0; i < m->nfiles; i++) {
        uint8_t have[32];
        if (!m->files[i].replaces) continue;
        if (hash(ctx, m->files[i].path, have) && !memcmp(have, m->files[i].sha, 32)) continue;
        if (total < max) memcpy(out[total], m->files[i].path, MODCAT_PATH);
        total++;
    }
    return total;
}

const char* tagpu_modcat_stale_live(char (*list)[MODCAT_PATH], int total, int max, MODCAT_OFF off, void* ctx)
{
    int i;
    for (i = 0; i < total && i < max; i++)
        if (!off || !off(ctx, list[i])) return list[i];
    return total > max ? list[0] : NULL;
}

const char* tagpu_modcat_clash(const MODMUTATOR* a, const MODMUTATOR* b, MODCAT_OFF off, void* ctx)
{
    int i, k;
    for (i = 0; i < a->nfiles; i++) {
        const char* p = a->files[i].path;
        if (off && off(ctx, p)) continue;
        for (k = 0; k < b->nfiles; k++)
            if (tagpu_modcat_same_path(p, b->files[k].path)) return p;
        for (k = 0; k < b->nhides; k++)
            if (tagpu_modcat_same_path(p, b->hides[k]) && !(off && off(ctx, b->hides[k]))) return p;
    }
    for (i = 0; i < a->nhides; i++) {
        const char* p = a->hides[i];
        if (off && off(ctx, p)) continue;
        for (k = 0; k < b->nfiles; k++)
            if (tagpu_modcat_same_path(p, b->files[k].path)) return p;
    }
    return NULL;
}
