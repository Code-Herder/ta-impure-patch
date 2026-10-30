/* tagpu_wikitext.c -- the wiki's shown text, filled in. Contract: tagpu_wikitext.h. */
#include <string.h>
#include "tagpu_wikitext.h"


void tagpu_wikitext_words(WIKIWORDS* w, const WIKIENTRY* e, const WIKICASTING* casting, int iface)
{
    int c, i;
    for (c = 0; c < WIKI_NCAST; c++) {
        const WIKICASTDEF* b = casting && casting->cast[c].bound ? &casting->cast[c] : NULL;
        w->cast[c] = NULL;
        if (b) {
            w->cast[c] = b->title[0] ? b->title : b->type;
            continue;
        }
        for (i = 0; e && i < e->ndef; i++)
            if (e->def[i].cast == c) {
                w->cast[c] = e->def[i].title && e->def[i].title[0] ? e->def[i].title : e->def[i].name;
                break;
            }
    }
    w->iface = iface == 1 ? 1 : 0;
}

/* the word of the token named by the `len` bytes at `s`, or NULL: text.js's wordOf */
static const char* word_of(const char* s, size_t len, const WIKIWORDS* w)
{
    const char* b = tagpu_wikidata_button_word(s, len, w->iface);
    int c;
    if (b) return b;
    for (c = 0; c < WIKI_NCAST; c++) {
        const char* name = tagpu_wikidata_cast_name(c);
        if (strlen(name) == len && !memcmp(name, s, len)) return w->cast[c];
    }
    return NULL;
}

/* The output so far: `len` bytes kept of `n`, and `cut` once a byte did not fit. */
typedef struct {
    char*  out;
    size_t n, len;
    int    cut;
} SINK;

/* `len` bytes of `s` after what is kept. The bytes that do not fit are dropped and the kept ones
   end at a UTF-8 character: while the first byte dropped continues a character, the cut steps back
   over the bytes of that character that were kept, as tagpu_wikistage.c's cut does. */
static void put(SINK* k, const char* s, size_t len)
{
    size_t room;
    unsigned char next;
    if (k->cut || !len) return;
    room = k->n - 1 - k->len;
    if (len <= room) {
        memcpy(k->out + k->len, s, len);
        k->len += len;
        return;
    }
    memcpy(k->out + k->len, s, room);
    k->len += room;
    k->cut = 1;
    next = (unsigned char)s[room];
    while (k->len > 0 && (next & 0xC0) == 0x80) next = (unsigned char)k->out[--k->len];
}

int tagpu_wikitext_fill(const char* raw, const WIKIWORDS* w, char* out, size_t n)
{
    SINK k;
    const char *s = raw ? raw : "", *a, *b;
    if (!n) return 1;
    k.out = out;
    k.n = n;
    k.len = 0;
    k.cut = 0;
    for (;;) {
        const char* word;
        if (!(a = strchr(s, '{')) || !(b = strchr(a + 1, '}'))) {
            put(&k, s, strlen(s));
            break;
        }
        put(&k, s, (size_t)(a - s));
        word = word_of(a + 1, (size_t)(b - a - 1), w);
        if (word) put(&k, word, strlen(word));
        else put(&k, a, (size_t)(b - a + 1));
        s = b + 1;
    }
    out[k.len] = 0;
    return k.cut;
}
