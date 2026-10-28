/* tagpu_guifile.c -- a TDF-grammar file by position. Contract and the grammar: tagpu_guifile.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_guifile.h"

typedef struct {
    int name;                   /* offset of its trimmed, NUL-terminated name in `text`         */
    int child, last, next;      /* its first and last child, its next sibling; -1 for none      */
    int nchild;
    int key, lastkey;           /* its keys in file order; -1 for none                          */
} SEC;

typedef struct { int k, v, next; } KEY;   /* offsets in `text`, the next key of its section */

struct TAGPU_GUIFILE {
    char* text;                 /* the comment-blanked copy; names and values cut in place      */
    int   n;
    SEC*  sec;  int nsec, csec;
    KEY*  key;  int nkey, ckey;
};

static int blank(unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* _stricmp in the C locale, which folds A..Z alone */
static int same(const char* a, const char* b)
{
    for (; *a && *b; a++, b++)
        if (lower((unsigned char)*a) != lower((unsigned char)*b)) return 0;
    return *a == *b;
}

/* 0x4C33A0, once over the text: a line comment blanks up to, not including, the next LF; a block
   comment blanks up to and including the first star-slash, testing the byte before the cursor,
   which the opener's own star never is since it is blanked first; unterminated, the text's last
   byte survives */
static void strip_comments(char* t, int n)
{
    int i = 0;
    while (i < n) {
        if (t[i] == '/' && i + 1 < n && t[i + 1] == '/') {
            while (i < n && t[i] != '\n') t[i++] = ' ';
            continue;
        }
        if (t[i] == '/' && i + 1 < n && t[i + 1] == '*') {
            t[i] = t[i + 1] = ' ';
            i += 2;
            while (i < n) {
                if (t[i - 1] == '*' && t[i] == '/') {
                    t[i - 1] = t[i] = ' ';
                    i++;
                    break;
                }
                t[i - 1] = ' ';
                i++;
            }
            continue;
        }
        i++;
    }
}

/* [a, b) of the text trimmed of blanks and cut there with a NUL; its offset. `b` lies at or before
   a byte the parse has already passed, so the cut never reaches text still to be parsed. */
static int cut(char* t, int a, int b)
{
    while (a < b && blank((unsigned char)t[a])) a++;
    while (b > a && blank((unsigned char)t[b - 1])) b--;
    t[b] = 0;
    return a;
}

/* the first `c` in [from, n), or -1 */
static int find(const char* t, int from, int n, char c)
{
    const char* p = from < n ? (const char*)memchr(t + from, c, (size_t)(n - from)) : NULL;
    return p ? (int)(p - t) : -1;
}

static int grow(void** arr, int* cap, int need, size_t each)
{
    void* p;
    int c;
    if (need <= *cap) return 1;
    c = *cap ? *cap : 64;
    while (c < need) {
        if (c > (1 << 28)) return 0;
        c *= 2;
    }
    if (!(p = realloc(*arr, (size_t)c * each))) return 0;
    *arr = p;
    *cap = c;
    return 1;
}

static int add_sec(TAGPU_GUIFILE* f, int parent, int name)
{
    SEC* s;
    const int i = f->nsec;
    if (!grow((void**)&f->sec, &f->csec, i + 1, sizeof *f->sec)) return -1;
    s = &f->sec[i];
    s->name = name;
    s->child = s->last = s->next = s->key = s->lastkey = -1;
    s->nchild = 0;
    f->nsec++;
    if (parent >= 0) {
        SEC* p = &f->sec[parent];
        if (p->last >= 0) f->sec[p->last].next = i; else p->child = i;
        p->last = i;
        p->nchild++;
    }
    return i;
}

static int add_key(TAGPU_GUIFILE* f, int sec, int k, int v)
{
    SEC* s = &f->sec[sec];
    const int i = f->nkey;
    if (!grow((void**)&f->key, &f->ckey, i + 1, sizeof *f->key)) return 0;
    f->key[i].k = k;
    f->key[i].v = v;
    f->key[i].next = -1;
    if (s->lastkey >= 0) f->key[s->lastkey].next = i; else s->key = i;
    s->lastkey = i;
    f->nkey++;
    return 1;
}

TAGPU_GUIFILE* tagpu_guifile_parse(const unsigned char* blob, size_t n, char* why, size_t nwhy)
{
    TAGPU_GUIFILE* f;
    int* stack = NULL;
    int depth = 0, cap = 0, pos = 0, len;
    const void* nul;
    char* t;
    if (!blob || !n) { snprintf(why, nwhy, "an empty file, which the engine does not load"); return NULL; }
    if (n > (size_t)(1 << 28)) { snprintf(why, nwhy, "a file of %lu bytes, past the reader's bound", (unsigned long)n); return NULL; }
    nul = memchr(blob, 0, n);
    len = nul ? (int)((const unsigned char*)nul - blob) : (int)n;
    if (!(f = (TAGPU_GUIFILE*)calloc(1, sizeof *f)) || !(f->text = (char*)malloc((size_t)len + 1))) {
        free(f);
        snprintf(why, nwhy, "out of memory");
        return NULL;
    }
    t = f->text;
    memcpy(t, blob, (size_t)len);
    t[len] = 0;
    f->n = len;
    strip_comments(t, len);
    /* the root's name is the text's terminator: "" */
    if (add_sec(f, -1, len) < 0) goto oom;
    if (!grow((void**)&stack, &cap, 1, sizeof *stack)) goto oom;
    stack[depth++] = 0;
    for (;;) {
        const int cur = stack[depth - 1];
        int j, k;
        while (pos < len && blank((unsigned char)t[pos])) pos++;
        if (pos >= len) {
            if (depth > 1) {
                snprintf(why, nwhy, "the text ends inside [%.40s]", t + f->sec[cur].name);
                goto bad;
            }
            break;
        }
        if (t[pos] == '[') {
            int sec;
            if ((j = find(t, pos, len, ']')) < 0) { snprintf(why, nwhy, "a section's closing ']' is not found"); goto bad; }
            k = j + 1;
            while (k < len && blank((unsigned char)t[k])) k++;
            if (k >= len || t[k] != '{') { snprintf(why, nwhy, "no '{' after a section's name"); goto bad; }
            if ((sec = add_sec(f, cur, cut(t, pos + 1, j))) < 0) goto oom;
            if (!grow((void**)&stack, &cap, depth + 1, sizeof *stack)) goto oom;
            stack[depth++] = sec;
            pos = k + 1;
            continue;
        }
        if (t[pos] == '}') {
            if (depth == 1) break;      /* a stray '}' at the top level ends the parse */
            depth--;
            pos++;
            continue;
        }
        if ((j = find(t, pos, len, '=')) < 0) { snprintf(why, nwhy, "a key in [%.40s] has no '='", t + f->sec[cur].name); goto bad; }
        if ((k = find(t, j + 1, len, ';')) < 0) { snprintf(why, nwhy, "a value in [%.40s] has no ';'", t + f->sec[cur].name); goto bad; }
        if (!add_key(f, cur, cut(t, pos, j), cut(t, j + 1, k))) goto oom;
        pos = k + 1;
    }
    free(stack);
    return f;
oom:
    snprintf(why, nwhy, "out of memory");
bad:
    free(stack);
    tagpu_guifile_free(f);
    return NULL;
}

void tagpu_guifile_free(TAGPU_GUIFILE* f)
{
    if (!f) return;
    free(f->text);
    free(f->sec);
    free(f->key);
    free(f);
}

int tagpu_guifile_count(const TAGPU_GUIFILE* f, int sec)
{
    return f && sec >= 0 && sec < f->nsec ? f->sec[sec].nchild : 0;
}

int tagpu_guifile_child(const TAGPU_GUIFILE* f, int sec, int i)
{
    int c;
    if (!f || sec < 0 || sec >= f->nsec || i < 0 || i >= f->sec[sec].nchild) return -1;
    for (c = f->sec[sec].child; c >= 0 && i > 0; i--) c = f->sec[c].next;
    return c;
}

const char* tagpu_guifile_name(const TAGPU_GUIFILE* f, int sec)
{
    return f && sec >= 0 && sec < f->nsec ? f->text + f->sec[sec].name : "";
}

int tagpu_guifile_find(const TAGPU_GUIFILE* f, int sec, const char* name)
{
    int c;
    if (!f || sec < 0 || sec >= f->nsec) return -1;
    for (c = f->sec[sec].child; c >= 0; c = f->sec[c].next)
        if (same(f->text + f->sec[c].name, name)) return c;
    return -1;
}

const char* tagpu_guifile_get(const TAGPU_GUIFILE* f, int sec, const char* key)
{
    const char* hit = NULL;
    int k;
    if (!f || sec < 0 || sec >= f->nsec) return NULL;
    for (k = f->sec[sec].key; k >= 0; k = f->key[k].next)
        if (same(f->text + f->key[k].k, key)) hit = f->text + f->key[k].v;
    return hit;
}

int tagpu_guifile_num(const char* v, long long* num, uint32_t* bits)
{
    long long m = 0;
    uint32_t low = 0;
    int neg = 0, digits = 0;
    const char* p = v;
    if (p) {
        while (blank((unsigned char)*p)) p++;
        if (*p == '+' || *p == '-') neg = *p++ == '-';
        for (; *p >= '0' && *p <= '9'; p++, digits++) {
            low = low * 10u + (uint32_t)(*p - '0');
            if (m < TAGPU_GUIFILE_NUMSAT) m = m * 10 + (*p - '0');
        }
    }
    if (!digits) neg = 0;
    if (m > TAGPU_GUIFILE_NUMSAT) m = TAGPU_GUIFILE_NUMSAT;
    if (num) *num = neg ? -m : m;
    if (bits) *bits = neg ? 0u - low : low;
    if (!p || !digits) return 0;
    while (blank((unsigned char)*p)) p++;
    return *p == 0;
}
