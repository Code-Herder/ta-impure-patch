/* tagpu_modcat.c -- Contract: tagpu_modcat.h. */
#include <stdio.h>
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
