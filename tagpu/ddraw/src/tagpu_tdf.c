/* tagpu_tdf.c -- TA's text definitions. Contract and rules: tagpu_tdf.h. */
#include <stdlib.h>
#include <string.h>
#include "tagpu_hpi.h"
#include "tagpu_tdf.h"

#define STACKMAX 64             /* sections open at once; one opened deeper is not followed      */

typedef struct {
    int    parent;
    size_t name;
    int    dead;                /* a key of the same name replaced it                            */
    int    keys, kids;          /* live ones                                                     */
    int    first_kid, next;     /* its sections, and its next sibling, in the order opened       */
    int    last_key;            /* its newest key, which wins                                    */
} NODE;
typedef struct { size_t key, val; int prev; } KEY;

struct TAGPU_TDF {
    char*  buf;                 /* the text, comments blanked, every name, key and value ended   */
    NODE*  node;
    int    nnode;
    KEY*   key;
    int    nkey;
};

/* Python's str.strip() on Latin-1: these and nothing else */
static int blank(unsigned char c)
{
    return c == ' ' || (c >= 0x09 && c <= 0x0D) || (c >= 0x1C && c <= 0x1F) || c == 0x85 || c == 0xA0;
}

/* parse_tdf's token classes: these end a run of text, and NUL does not */
static int delim(char c)
{
    return c == ';' || c == '{' || c == '}' || c == '[' || c == ']';
}

static void trim(const char* b, size_t* lo, size_t* hi)
{
    while (*lo < *hi && blank((unsigned char)b[*lo])) (*lo)++;
    while (*hi > *lo && blank((unsigned char)b[*hi - 1])) (*hi)--;
}

/* the comments, as parse_tdf's two substitutions take them: every block comment first (an
   unclosed one is text), then every line comment, each replaced by one space and the rest moved
   down */
static size_t uncomment(char* b, size_t n)
{
    size_t r = 0, w = 0;
    while (r < n) {
        if (r + 1 < n && b[r] == '/' && b[r + 1] == '*') {
            size_t e = r + 2;
            while (e + 1 < n && !(b[e] == '*' && b[e + 1] == '/')) e++;
            if (e + 1 < n) {
                b[w++] = ' ';
                r = e + 2;
                continue;
            }
            memmove(b + w, b + r, n - r);
            w += n - r;
            break;
        }
        b[w++] = b[r++];
    }
    n = w;
    for (r = w = 0; r < n;) {
        if (r + 1 < n && b[r] == '/' && b[r + 1] == '/') {
            while (r < n && b[r] != '\n') r++;
            b[w++] = ' ';
            continue;
        }
        b[w++] = b[r++];
    }
    return w;
}

static int child(const TAGPU_TDF* t, int parent, const char* name)
{
    int i;
    for (i = t->node[parent].first_kid; i > 0; i = t->node[i].next)
        if (!t->node[i].dead && !tagpu_hpi_stricmp(name, t->buf + t->node[i].name, (size_t)-1)) return i;
    return -1;
}

static int find_key(const TAGPU_TDF* t, int node, const char* name)
{
    int k;
    for (k = t->node[node].last_key; k >= 0; k = t->key[k].prev)
        if (!tagpu_hpi_stricmp(name, t->buf + t->key[k].key, (size_t)-1)) return k;
    return -1;
}

TAGPU_TDF* tagpu_tdf_parse(const char* text, size_t len)
{
    TAGPU_TDF* t = (TAGPU_TDF*)calloc(1, sizeof *t);
    int stack_cur[STACKMAX], stack_pend[STACKMAX], sp = 0, cur = 0, last_kid[TAGPU_TDF_SECTIONS + 1];
    size_t* ends = NULL;
    size_t nends = 0, n, i;
    if (!t || !(t->buf = (char*)malloc(len + 1))) goto refuse;
    memcpy(t->buf, text, len);
    n = uncomment(t->buf, len);
    t->buf[n] = 0;
    if (!(t->node = (NODE*)calloc(TAGPU_TDF_SECTIONS + 1, sizeof *t->node)) ||
        !(t->key = (KEY*)calloc(TAGPU_TDF_KEYS, sizeof *t->key)) ||
        !(ends = (size_t*)malloc(TAGPU_TDF_KEYS * sizeof *ends)))
        goto refuse;
    t->node[0].parent = -1;
    t->node[0].last_key = -1;
    last_kid[0] = 0;
    t->nnode = 1;
    for (i = 0; i < n;) {
        const char c = t->buf[i];
        if (c == '[') {
            const char* close = (const char*)memchr(t->buf + i + 1, ']', n - i - 1);
            size_t lo = i + 1, hi, at_close;
            int at;
            if (!close) { i++; continue; }
            at_close = (size_t)(close - t->buf);
            hi = at_close;
            trim(t->buf, &lo, &hi);
            t->buf[hi] = 0;             /* a blank or the ']' itself, which the scan steps past */
            at = cur >= 0 ? child(t, cur, t->buf + lo) : -1;
            /* a key of that name already in this scope: parse_tdf's setdefault keeps the string,
               and what the section holds goes nowhere */
            if (at < 0 && cur >= 0 && find_key(t, cur, t->buf + lo) < 0) {
                NODE* s;
                if (t->nnode > TAGPU_TDF_SECTIONS) goto refuse;
                at = t->nnode++;
                s = &t->node[at];
                s->parent = cur;
                s->name = lo;
                s->last_key = -1;
                if (last_kid[cur]) t->node[last_kid[cur]].next = at;
                else t->node[cur].first_kid = at;
                last_kid[cur] = at;
                last_kid[at] = 0;
                t->node[cur].kids++;
            }
            if (sp < STACKMAX) {
                stack_cur[sp] = cur;
                stack_pend[sp] = at;
                sp++;
            }
            i = at_close + 1;
            continue;
        }
        if (c == '{') { if (sp) cur = stack_pend[sp - 1]; i++; continue; }
        if (c == '}') { if (sp) cur = stack_cur[--sp]; i++; continue; }
        if (c == ';' || c == ']') { i++; continue; }
        {
            size_t e = i, eq, klo, khi, vlo, vhi;
            while (e < n && !delim(t->buf[e])) e++;
            for (eq = i; eq < e && t->buf[eq] != '='; eq++) {}
            if (eq < e && cur >= 0) {
                klo = i; khi = eq; vlo = eq + 1; vhi = e;
                trim(t->buf, &klo, &khi);
                trim(t->buf, &vlo, &vhi);
                if (khi > klo) {
                    KEY* k;
                    int sec;
                    if (t->nkey >= TAGPU_TDF_KEYS) goto refuse;
                    k = &t->key[t->nkey];
                    /* the key ends on a blank or this token's '=', which nothing reads again; the
                       value may end on the delimiter the scan reads next, so it is ended after */
                    t->buf[khi] = 0;
                    ends[nends++] = vhi;
                    k->key = klo;
                    k->val = vlo;
                    k->prev = t->node[cur].last_key;
                    t->node[cur].last_key = t->nkey++;
                    t->node[cur].keys++;
                    /* a key naming a section of this scope replaces it, as cur[k] = v does */
                    if (t->node[cur].kids && (sec = child(t, cur, t->buf + klo)) >= 0) {
                        t->node[sec].dead = 1;
                        t->node[cur].kids--;
                    }
                }
            }
            i = e;
        }
    }
    /* each value's end is a blank, the delimiter after its token, or the buffer's end */
    for (i = 0; i < nends; i++) t->buf[ends[i]] = 0;
    free(ends);
    return t;
refuse:
    free(ends);
    tagpu_tdf_free(t);
    return NULL;
}

void tagpu_tdf_free(TAGPU_TDF* t)
{
    if (!t) return;
    free(t->buf);
    free(t->node);
    free(t->key);
    free(t);
}

int tagpu_tdf_section(const TAGPU_TDF* t, int parent, const char* name)
{
    int at;
    if (!t || parent < 0 || parent >= t->nnode) return -1;
    at = child(t, parent, name);
    return at >= 0 && (t->node[at].keys > 0 || t->node[at].kids > 0) ? at : -1;
}

const char* tagpu_tdf_get(const TAGPU_TDF* t, int node, const char* key)
{
    int k;
    if (!t || node < 0 || node >= t->nnode || (k = find_key(t, node, key)) < 0) return NULL;
    return t->buf + t->key[k].val;
}

long tagpu_tdf_int(const TAGPU_TDF* t, int node, const char* key, long dflt)
{
    const char* v = tagpu_tdf_get(t, node, key);
    long r = 0;
    int neg = 0;
    if (!v) return dflt;
    while (*v == ' ' || (*v >= 0x09 && *v <= 0x0D)) v++;
    if (*v == '-' || *v == '+') neg = *v++ == '-';
    while (*v >= '0' && *v <= '9' && r < 100000000L) r = r * 10 + (*v++ - '0');
    return neg ? -r : r;
}
