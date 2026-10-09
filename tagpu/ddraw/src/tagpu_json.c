/* tagpu_json.c -- the JSON reader. Contract: tagpu_json.h.

   RFC 8259, less what our files never use and the parser therefore refuses rather than
   half-supports: a value nested deeper than JSON_DEPTH, and a \u escape naming a lone
   surrogate. Numbers go through strtod after the grammar has been checked, so a number the
   grammar refuses (a leading zero, a bare dot) never reaches it. strtod reads in the C
   locale, the only one this DLL runs under. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_json.h"

#define JSON_DEPTH 64
#define CHUNK      (64 * 1024)

typedef struct CHUNKHDR { struct CHUNKHDR* next; size_t used, size; } CHUNKHDR;

struct TJDOC {
    CHUNKHDR* chunks;
    TJV*      root;
};

typedef struct {
    TJDOC*      d;
    const char* p;
    const char* end;
    const char* base;
    char*       err;
    size_t      errn;
    int         failed;
} P;

static void* arena(P* ps, size_t n)
{
    CHUNKHDR* c = ps->d->chunks;
    n = (n + 7) & ~(size_t)7;
    if (!c || c->used + n > c->size) {
        size_t size = n > CHUNK ? n : CHUNK;
        c = (CHUNKHDR*)malloc(sizeof(CHUNKHDR) + size);
        if (!c) return NULL;
        c->next = ps->d->chunks;
        c->used = 0;
        c->size = size;
        ps->d->chunks = c;
    }
    c->used += n;
    return (char*)(c + 1) + c->used - n;
}

static void fail(P* ps, const char* what)
{
    if (ps->failed) return;
    ps->failed = 1;
    if (ps->err && ps->errn)
        snprintf(ps->err, ps->errn, "json: %s at byte %ld", what, (long)(ps->p - ps->base));
}

static void ws(P* ps)
{
    while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r'))
        ps->p++;
}

static int hex4(P* ps, unsigned* out)
{
    unsigned v = 0;
    int i;
    if (ps->end - ps->p < 4) return 0;
    for (i = 0; i < 4; i++) {
        char c = ps->p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return 0;
    }
    ps->p += 4;
    *out = v;
    return 1;
}

static char* put_utf8(char* o, unsigned cp)
{
    if (cp < 0x80) *o++ = (char)cp;
    else if (cp < 0x800) { *o++ = (char)(0xC0 | (cp >> 6)); *o++ = (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
        *o++ = (char)(0xE0 | (cp >> 12)); *o++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *o++ = (char)(0x80 | (cp & 0x3F));
    } else {
        *o++ = (char)(0xF0 | (cp >> 18)); *o++ = (char)(0x80 | ((cp >> 12) & 0x3F));
        *o++ = (char)(0x80 | ((cp >> 6) & 0x3F)); *o++ = (char)(0x80 | (cp & 0x3F));
    }
    return o;
}

/* A string's decoded bytes are never longer than its source, so the source length is the
   whole allocation. */
static const char* string(P* ps)
{
    const char* s = ++ps->p;
    char *out, *o;
    while (ps->p < ps->end && *ps->p != '"') {
        if (*ps->p == '\\') ps->p++;
        else if ((unsigned char)*ps->p < 0x20) { fail(ps, "a control character in a string"); return NULL; }
        ps->p++;
    }
    if (ps->p >= ps->end) { fail(ps, "an unterminated string"); return NULL; }
    out = o = (char*)arena(ps, (size_t)(ps->p - s) + 1);
    if (!out) { fail(ps, "out of memory"); return NULL; }
    ps->p = s;
    while (*ps->p != '"') {
        char c = *ps->p++;
        unsigned cp, lo;
        if (c != '\\') { *o++ = c; continue; }
        switch (c = *ps->p++) {
        case '"': case '\\': case '/': *o++ = c; break;
        case 'b': *o++ = '\b'; break;
        case 'f': *o++ = '\f'; break;
        case 'n': *o++ = '\n'; break;
        case 'r': *o++ = '\r'; break;
        case 't': *o++ = '\t'; break;
        case 'u':
            if (!hex4(ps, &cp)) { fail(ps, "a bad \\u escape"); return NULL; }
            if (cp >= 0xD800 && cp < 0xDC00) {
                if (ps->end - ps->p < 6 || ps->p[0] != '\\' || ps->p[1] != 'u') { fail(ps, "a lone surrogate"); return NULL; }
                ps->p += 2;
                if (!hex4(ps, &lo) || lo < 0xDC00 || lo >= 0xE000) { fail(ps, "a lone surrogate"); return NULL; }
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            } else if (cp >= 0xDC00 && cp < 0xE000) { fail(ps, "a lone surrogate"); return NULL; }
            o = put_utf8(o, cp);
            break;
        default: fail(ps, "a bad escape"); return NULL;
        }
    }
    *o = 0;
    ps->p++;
    return out;
}

static int digits(P* ps)
{
    const char* s = ps->p;
    while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9') ps->p++;
    return ps->p > s;
}

static int number(P* ps, double* out)
{
    const char* s = ps->p;
    char buf[64];
    if (ps->p < ps->end && *ps->p == '-') ps->p++;
    if (ps->p < ps->end && *ps->p == '0') ps->p++;
    else if (!digits(ps)) return 0;
    if (ps->p < ps->end && *ps->p == '.') { ps->p++; if (!digits(ps)) return 0; }
    if (ps->p < ps->end && (*ps->p == 'e' || *ps->p == 'E')) {
        ps->p++;
        if (ps->p < ps->end && (*ps->p == '+' || *ps->p == '-')) ps->p++;
        if (!digits(ps)) return 0;
    }
    if ((size_t)(ps->p - s) >= sizeof buf) return 0;
    memcpy(buf, s, (size_t)(ps->p - s));
    buf[ps->p - s] = 0;
    *out = strtod(buf, NULL);
    return 1;
}

static int word(P* ps, const char* w)
{
    size_t n = strlen(w);
    if ((size_t)(ps->end - ps->p) < n || memcmp(ps->p, w, n)) return 0;
    ps->p += n;
    return 1;
}

static TJV* value(P* ps, int depth)
{
    TJV* v;
    ws(ps);
    if (ps->p >= ps->end) { fail(ps, "a value expected"); return NULL; }
    if (depth > JSON_DEPTH) { fail(ps, "nesting too deep"); return NULL; }
    v = (TJV*)arena(ps, sizeof(TJV));
    if (!v) { fail(ps, "out of memory"); return NULL; }
    memset(v, 0, sizeof *v);
    switch (*ps->p) {
    case '{': case '[': {
        char close = *ps->p == '{' ? '}' : ']';
        TJV** tail = &v->first;
        v->type = close == '}' ? TJ_OBJ : TJ_ARR;
        ps->p++;
        ws(ps);
        if (ps->p < ps->end && *ps->p == close) { ps->p++; return v; }
        for (;;) {
            const char* key = NULL;
            TJV* kid;
            if (v->type == TJ_OBJ) {
                ws(ps);
                if (ps->p >= ps->end || *ps->p != '"') { fail(ps, "a member name expected"); return NULL; }
                if (!(key = string(ps))) return NULL;
                ws(ps);
                if (ps->p >= ps->end || *ps->p != ':') { fail(ps, "':' expected"); return NULL; }
                ps->p++;
            }
            if (!(kid = value(ps, depth + 1))) return NULL;
            kid->key = key;
            *tail = kid;
            tail = &kid->next;
            v->n++;
            ws(ps);
            if (ps->p < ps->end && *ps->p == ',') { ps->p++; continue; }
            if (ps->p < ps->end && *ps->p == close) { ps->p++; return v; }
            fail(ps, close == '}' ? "',' or '}' expected" : "',' or ']' expected");
            return NULL;
        }
    }
    case '"':
        v->type = TJ_STR;
        return (v->str = string(ps)) ? v : NULL;
    case 't': if (word(ps, "true"))  { v->type = TJ_TRUE;  return v; } break;
    case 'f': if (word(ps, "false")) { v->type = TJ_FALSE; return v; } break;
    case 'n': if (word(ps, "null"))  { v->type = TJ_NULL;  return v; } break;
    default:
        v->type = TJ_NUM;
        if (number(ps, &v->num)) return v;
        break;
    }
    fail(ps, "a bad value");
    return NULL;
}

TJDOC* tagpu_json_parse(const char* text, size_t len, char* err, size_t errn)
{
    P ps;
    TJDOC* d = (TJDOC*)calloc(1, sizeof(TJDOC));
    if (!d) return NULL;
    memset(&ps, 0, sizeof ps);
    ps.d = d;
    ps.p = ps.base = text;
    ps.end = text + len;
    ps.err = err;
    ps.errn = errn;
    if (err && errn) err[0] = 0;
    if (ps.end - ps.p >= 3 && !memcmp(ps.p, "\xEF\xBB\xBF", 3)) ps.p += 3;
    d->root = value(&ps, 0);
    if (d->root) {
        ws(&ps);
        if (ps.p != ps.end) fail(&ps, "text after the value");
    }
    if (ps.failed || !d->root) {
        tagpu_json_free(d);
        return NULL;
    }
    return d;
}

const TJV* tagpu_json_root(const TJDOC* d) { return d ? d->root : NULL; }

void tagpu_json_free(TJDOC* d)
{
    CHUNKHDR* c;
    if (!d) return;
    while ((c = d->chunks)) { d->chunks = c->next; free(c); }
    free(d);
}

const TJV* tj_get(const TJV* obj, const char* key)
{
    const TJV* k;
    if (!obj || obj->type != TJ_OBJ || !key) return NULL;
    for (k = obj->first; k; k = k->next)
        if (!strcmp(k->key, key)) return k;
    return NULL;
}

const TJV* tj_at(const TJV* arr, int i)
{
    const TJV* k;
    if (!arr || (arr->type != TJ_ARR && arr->type != TJ_OBJ) || i < 0) return NULL;
    for (k = arr->first; k && i; k = k->next) i--;
    return k;
}

double tj_num(const TJV* v, double dflt) { return v && v->type == TJ_NUM ? v->num : dflt; }
const char* tj_str(const TJV* v, const char* dflt) { return v && v->type == TJ_STR ? v->str : dflt; }
int tj_is(const TJV* v, int type) { return v && v->type == type; }
