/* tagpu_modtar.c -- Contract: tagpu_modtar.h. */
#include <stdio.h>
#include <string.h>
#include "tagpu_modtar.h"

#define BLOCK 512u

static int refuse(MODTAR* t, const char* what, const char* name)
{
    if (!t->why[0]) snprintf(t->why, sizeof t->why, "%s%s%s", what, name ? ": " : "", name ? name : "");
    return 0;
}

static int same_fold(const char* a, size_t n, const char* name)
{
    size_t i;
    for (i = 0; i < n; i++) {
        char c = a[i] >= 'a' && a[i] <= 'z' ? (char)(a[i] - 32) : a[i];
        if (c != name[i] || !name[i]) return 0;
    }
    return !name[n];
}

/* Windows opens these as devices whatever the folder before them and whatever follows the first dot. */
static int device_name(const char* part, size_t n)
{
    static const char* const names[] = { "CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$" };
    size_t stem = 0, i;
    while (stem < n && part[stem] != '.') stem++;
    while (stem && part[stem - 1] == ' ') stem--;       /* the name's parser drops these too */
    for (i = 0; i < sizeof names / sizeof names[0]; i++)
        if (same_fold(part, stem, names[i])) return 1;
    return stem == 4 && part[3] >= '0' && part[3] <= '9' && (same_fold(part, 3, "COM") || same_fold(part, 3, "LPT"));
}

int modtar_path_safe(const char* path)
{
    const char* part = path;
    const char* p;
    if (!path || !*path) return 0;
    for (p = path;; p++) {
        unsigned char c = (unsigned char)*p;
        if (!c || c == '/') {
            size_t n = (size_t)(p - part);
            if (!n || (n == 1 && part[0] == '.') || (n == 2 && part[0] == '.' && part[1] == '.')) return 0;
            if (part[n - 1] == '.' || part[n - 1] == ' ' || device_name(part, n)) return 0;
            if (!c) return 1;
            part = p + 1;
        } else if (c < 0x20 || c >= 0x7F || c == '\\' || c == ':') {
            return 0;
        }
    }
}

void modtar_init(MODTAR* t, const MODTAR_SINK* sink, const MODTAR_FILE* files, int nfiles, unsigned char* seen)
{
    memset(t, 0, sizeof *t);
    t->sink = *sink;
    t->files = files;
    t->nfiles = nfiles;
    t->seen = seen;
    t->current = -1;
    memset(seen, 0, (size_t)nfiles);
}

/* a NUL- or space-ended octal field; 0 when it holds anything else or passes 32 bits */
static int octal(const unsigned char* f, size_t n, uint32_t* out)
{
    uint64_t v = 0;
    size_t i = 0;
    int digits = 0;
    while (i < n && f[i] == ' ') i++;
    for (; i < n && f[i] >= '0' && f[i] <= '7'; i++, digits++) {
        v = v * 8 + (uint64_t)(f[i] - '0');
        if (v > 0xFFFFFFFFu) return 0;
    }
    if (!digits) return 0;
    for (; i < n; i++) if (f[i] != 0 && f[i] != ' ') return 0;
    *out = (uint32_t)v;
    return 1;
}

static int all_zero(const unsigned char* b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) if (b[i]) return 0;
    return 1;
}

static int on_header(MODTAR* t)
{
    const unsigned char* h = t->header;
    char name[101];
    uint32_t sum, size, got = 0;
    int i;

    if (all_zero(h, BLOCK)) {
        t->zero_blocks++;
        return 1;
    }
    if (t->zero_blocks) return refuse(t, "data after the archive's end", NULL);
    if (!octal(h + 148, 8, &sum)) return refuse(t, "a member header with no checksum", NULL);
    for (i = 0; i < (int)BLOCK; i++) got += (i >= 148 && i < 156) ? ' ' : h[i];
    if (got != sum) return refuse(t, "a member header whose checksum does not add up", NULL);
    if (memcmp(h + 257, "ustar", 5)) return refuse(t, "a member header that is not ustar", NULL);
    if (h[156] != '0' && h[156] != 0) return refuse(t, "a member that is not a regular file", NULL);
    if (!memchr(h, 0, 100) || !all_zero(h + 345, 155)) return refuse(t, "a member name longer than the name field", NULL);
    memcpy(name, h, 100);
    name[100] = 0;
    if (!modtar_path_safe(name)) return refuse(t, "a member path that is not safe", name);
    if (!octal(h + 124, 12, &size)) return refuse(t, "a member whose size is not octal", name);

    for (i = 0; i < t->nfiles; i++) if (!strcmp(t->files[i].path, name)) break;
    if (i == t->nfiles) return refuse(t, "a member the catalog does not declare", name);
    if (t->seen[i]) return refuse(t, "a member given twice", name);
    if (t->files[i].size != size) return refuse(t, "a member of another size than the catalog declares", name);

    t->seen[i] = 1;
    t->current = i;
    t->left = size;
    t->pad = (BLOCK - size % BLOCK) % BLOCK;
    t->in_member = 1;
    modsha_init(&t->sha);
    return t->sink.begin(t->sink.ctx, name, size) ? 1 : refuse(t, "the member could not be written", name);
}

static int member_done(MODTAR* t)
{
    uint8_t got[32];
    modsha_end(&t->sha, got);
    t->in_member = 0;
    if (!t->sink.end(t->sink.ctx)) return refuse(t, "the member could not be written", t->files[t->current].path);
    if (memcmp(got, t->files[t->current].sha256, 32)) return refuse(t, "a member whose hash is not the declared one", t->files[t->current].path);
    return 1;
}

int modtar_feed(MODTAR* t, const void* data, size_t n)
{
    const unsigned char* p = (const unsigned char*)data;
    if (t->why[0]) return 0;
    while (n) {
        if (t->in_member) {
            size_t take = n < t->left ? n : t->left;
            if (take) {
                modsha_add(&t->sha, p, take);
                if (!t->sink.data(t->sink.ctx, p, take)) return refuse(t, "the member could not be written", t->files[t->current].path);
                t->left -= (uint32_t)take;
                p += take;
                n -= take;
            }
            if (!t->left && !member_done(t)) return 0;
        } else if (t->pad) {
            size_t take = n < t->pad ? n : t->pad;
            if (!all_zero(p, take)) return refuse(t, "a member whose padding is not zero", t->files[t->current].path);
            t->pad -= (uint32_t)take;
            p += take;
            n -= take;
        } else {
            size_t take = BLOCK - t->have;
            if (take > n) take = n;
            memcpy(t->header + t->have, p, take);
            t->have += (uint32_t)take;
            p += take;
            n -= take;
            if (t->have == BLOCK) {
                t->have = 0;
                if (!on_header(t)) return 0;
                /* a member of no bytes ends where it begins */
                if (t->in_member && !t->left && !member_done(t)) return 0;
            }
        }
    }
    return 1;
}

int modtar_finish(MODTAR* t)
{
    int i;
    if (t->why[0]) return 0;
    if (t->in_member || t->pad || t->have) return refuse(t, "the archive is truncated", NULL);
    if (t->zero_blocks < 2) return refuse(t, "the archive has no end", NULL);
    for (i = 0; i < t->nfiles; i++) if (!t->seen[i]) return refuse(t, "the archive lacks a declared file", t->files[i].path);
    return 1;
}
