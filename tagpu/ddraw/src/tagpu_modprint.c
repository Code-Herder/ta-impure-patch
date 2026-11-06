/* tagpu_modprint -- see tagpu_modprint.h */
#include "tagpu_modprint.h"
#include <string.h>
#include <ctype.h>

#define TAIL_VERSION 1

static int same_nocase(const char* a, const char* b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return 0;
    return 1;
}

/* The engine builds a unit's menu pages as "<unit><n>.gui", and a unit's own name can end in digits, so each cut of the digits is asked. */
static int names_a_unit(const char* stem, size_t len, int (*is_unit)(void*, const char*, size_t), void* ctx)
{
    size_t cut = len;
    while (cut > 0 && stem[cut - 1] >= '0' && stem[cut - 1] <= '9') {
        cut--;
        if (cut > 0 && is_unit(ctx, stem, cut)) return 1;
    }
    return 0;
}

int tagpu_modprint_counts(const char* path, int (*is_unit)(void* ctx, const char* name, size_t len), void* ctx)
{
    static const char* const COSMETIC[] = { ".gaf", ".pcx", ".bmp", ".wav", ".fnt" };
    const char* base = strrchr(path, '\\');
    const char* dot;
    size_t i;
    base = base ? base + 1 : path;
    dot = strrchr(base, '.');
    if (same_nocase(path, "maps\\", 5) || same_nocase(path, "camps\\", 6)) return 0;
    if (!dot) return 1;
    for (i = 0; i < sizeof COSMETIC / sizeof *COSMETIC; i++)
        if (strlen(dot) == 4 && same_nocase(dot, COSMETIC[i], 4)) return 0;
    if (strlen(dot) == 4 && same_nocase(dot, ".gui", 4)) return names_a_unit(base, (size_t)(dot - base), is_unit, ctx);
    return 1;
}

void tagpu_modprint_tail_put(const MODPRINT* p, unsigned char out[MODPRINT_TAIL])
{
    memset(out, 0, MODPRINT_TAIL);
    memcpy(out, "IMFP", 4);
    out[4] = TAIL_VERSION;
    memcpy(out + 8, p->exe, MODPRINT_HASH);
    memcpy(out + 8 + MODPRINT_HASH, p->content, MODPRINT_HASH);
    memcpy(out + 8 + 2 * MODPRINT_HASH, p->label, MODPRINT_LABEL);
}

int tagpu_modprint_tail_read(const unsigned char* in, size_t n, MODPRINT* out)
{
    size_t i;
    if (n < MODPRINT_TAIL || memcmp(in, "IMFP", 4) || in[4] != TAIL_VERSION) return 0;
    memcpy(out->exe, in + 8, MODPRINT_HASH);
    memcpy(out->content, in + 8 + MODPRINT_HASH, MODPRINT_HASH);
    memcpy(out->label, in + 8 + 2 * MODPRINT_HASH, MODPRINT_LABEL);
    out->label[MODPRINT_LABEL - 1] = 0;
    for (i = 0; out->label[i]; i++)
        if ((unsigned char)out->label[i] < 0x20 || (unsigned char)out->label[i] > 0x7E) out->label[i] = '?';
    return 1;
}

#ifndef MODPRINT_TEST
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include "tagpu_log.h"
#include "tagpu_modcat.h"
#include "tagpu_modinst.h"
#include "tagpu_modload.h"
#include "tagpu_modpak.h"
#include "tagpu_replays.h"
#include "tagpu_vfs.h"

#define READ_RANGE (16u << 20)   /* inside TAGPU_HPI_READMAX */

typedef struct {
    TAGPU_VFILE* f;
    char (*key)[TAGPU_VFS_PATHMAX];     /* each file's path, lower-cased: what the hash takes and what sorts it */
    int  n, cap, failed;
} WALKED;

static MODPRINT s_print;
static int      s_printOk;
static HANDLE   s_thread;
static int      s_started;
static char     (*s_arcs)[TAGPU_VFS_PATHMAX];
static int      s_narcs, s_ntotal;

static int note_file(void* ctx, const TAGPU_VFILE* f)
{
    WALKED* w = (WALKED*)ctx;
    int i;
    if (w->n == w->cap) {
        const int cap = w->cap ? w->cap * 2 : 4096;
        TAGPU_VFILE* nf = (TAGPU_VFILE*)realloc(w->f, (size_t)cap * sizeof *nf);
        char (*nk)[TAGPU_VFS_PATHMAX];
        if (nf) w->f = nf;
        nk = (char (*)[TAGPU_VFS_PATHMAX])realloc(w->key, (size_t)cap * TAGPU_VFS_PATHMAX);
        if (nk) w->key = nk;
        if (!nf || !nk) { w->failed = 1; return 0; }
        w->cap = cap;
    }
    w->f[w->n] = *f;
    for (i = 0; f->path[i] && i < TAGPU_VFS_PATHMAX - 1; i++) w->key[w->n][i] = (char)tolower((unsigned char)f->path[i]);
    w->key[w->n][i] = 0;
    w->n++;
    return 1;
}

static int by_key(void* ctx, const void* a, const void* b)
{
    const WALKED* w = (const WALKED*)ctx;
    return strcmp(w->key[*(const int*)a], w->key[*(const int*)b]);
}

static int unit_order(const void* a, const void* b)
{
    return strcmp((const char*)a, (const char*)b);
}

typedef struct { char (*name)[64]; int n; } UNITS;

static int is_unit_of(void* ctx, const char* name, size_t len)
{
    const UNITS* u = (const UNITS*)ctx;
    char key[64] = { 0 };
    size_t i;
    if (len >= sizeof key) return 0;
    for (i = 0; i < len; i++) key[i] = (char)tolower((unsigned char)name[i]);
    return bsearch(key, u->name, (size_t)u->n, sizeof *u->name, unit_order) != NULL;
}

/* Every FBI's name, for the .gui rule. A name past the array is no unit's the engine's own 32-byte buffers could hold. */
static void collect_units(const WALKED* w, UNITS* u)
{
    int i;
    u->n = 0;
    u->name = (char (*)[64])malloc((size_t)(w->n + 1) * sizeof *u->name);
    if (!u->name) return;
    for (i = 0; i < w->n; i++) {
        const char* base = strrchr(w->key[i], '\\');
        const char* dot;
        base = base ? base + 1 : w->key[i];
        dot = strrchr(base, '.');
        if (dot && !strcmp(dot, ".fbi") && (size_t)(dot - base) < sizeof *u->name) {
            memset(u->name[u->n], 0, sizeof *u->name);
            memcpy(u->name[u->n], base, (size_t)(dot - base));
            u->n++;
        }
    }
    qsort(u->name, (size_t)u->n, sizeof *u->name, unit_order);
}

/* mingw has no qsort_r on every runtime, so the sort's context is one the walk alone uses */
static const WALKED* s_sorting;
static int by_key_only(const void* a, const void* b)
{
    return by_key((void*)s_sorting, a, b);
}

/* A file's bytes into `sha`, in ranges the reader's own bound allows. A file that will not read adds its size alone: this player then differs from one
   whose copy reads, and two copies that both fail match only as far as their paths and sizes do. */
static void hash_file(TAGPU_VFS* v, const TAGPU_VFILE* f, unsigned char* buf, MODSHA* sha, unsigned long long* bytes)
{
    uint32_t off;
    char why[96] = "";
    for (off = 0; off < f->size;) {
        const uint32_t n = f->size - off < READ_RANGE ? f->size - off : READ_RANGE;
        if (!tagpu_vfs_read(v, f, off, n, buf, why, sizeof why)) {
            tagpu_logf("modprint: %s not read: %s", f->path, why);
            modsha_init(sha);
            modsha_add(sha, &f->size, sizeof f->size);
            return;
        }
        modsha_add(sha, buf, n);
        *bytes += n;
        off += n;
    }
}

/* An archive the engine mounted and this reader refuses: its name and size stand for it, and the log says so. */
static void hash_refused(const TAGPU_VFS* v, MODSHA* all)
{
    int i;
    for (i = 0; i < tagpu_vfs_narc(v); i++) {
        const char* why = tagpu_vfs_arc_refused(v, i);
        WIN32_FILE_ATTRIBUTE_DATA info;
        if (!why) continue;
        tagpu_logf("modprint: %s is counted by its size alone: %s", tagpu_vfs_arc_name(v, i), why);
        memset(&info, 0, sizeof info);
        GetFileAttributesExA(s_arcs[i], GetFileExInfoStandard, &info);
        modsha_add(all, tagpu_vfs_arc_name(v, i), strlen(tagpu_vfs_arc_name(v, i)) + 1);
        modsha_add(all, &info.nFileSizeLow, sizeof info.nFileSizeLow);
    }
}

/* Each top folder's own digest goes to the log, so two players told they differ can compare logs and find the folder. */
static void top_done(const char* top, int files, MODSHA* sha)
{
    uint8_t d[32];
    modsha_end(sha, d);
    if (files) tagpu_logf("modprint: %s: %d files, %02x%02x%02x%02x", top, files, d[0], d[1], d[2], d[3]);
    modsha_init(sha);
}

static DWORD WINAPI walk_thread(void* arg)
{
    char cwd[TAGPU_VFS_PATHMAX] = "", top[TAGPU_VFS_PATHMAX] = "";
    WALKED w;
    UNITS units = { 0 };
    TAGPU_VFS* v;
    MODSHA all, topsha;
    uint8_t digest[32];
    int* order = NULL;
    int i, k, counted = 0, nsame = 0, ntop = 0, whole;
    unsigned long long bytes = 0;
    size_t ncat = 0;
    const char* cat = tagpu_modinst_catalog(&ncat);
    const char* id = tagpu_modload_id();
    MODSAMEAS* same = id && cat ? tagpu_modcat_same_as(cat, ncat, id, &nsame) : NULL;
    unsigned char* buf = (unsigned char*)malloc(READ_RANGE);
    const DWORD t0 = GetTickCount();
    (void)arg;
    memset(&w, 0, sizeof w);
    GetCurrentDirectoryA(sizeof cwd, cwd);
    v = buf ? tagpu_vfs_open(cwd, (const char (*)[TAGPU_VFS_PATHMAX])s_arcs, s_narcs, "") : NULL;
    if (!v) { tagpu_log("modprint: out of memory, this player sends no fingerprint"); free(buf); free(same); return 0; }
    tagpu_vfs_close_loose(v, tagpu_modload_loose_closed);
    tagpu_modload_each_gone(tagpu_vfs_forget, v);
    whole = tagpu_vfs_enum_game(v, note_file, &w);
    if (whole && !w.failed && (order = (int*)malloc((size_t)(w.n + 1) * sizeof *order))) {
        collect_units(&w, &units);
        for (i = 0; i < w.n; i++) order[i] = i;
        s_sorting = &w;
        qsort(order, (size_t)w.n, sizeof *order, by_key_only);
        modsha_init(&all);
        modsha_init(&topsha);
        /* past the list's bound, what was cut still changes the answer, and the log says the answer is partial */
        if (s_ntotal != s_narcs) {
            tagpu_logf("modprint: %d archives mounted, %d read", s_ntotal, s_narcs);
            modsha_add(&all, &s_ntotal, sizeof s_ntotal);
        }
        hash_refused(v, &all);
        for (i = 0; i < w.n; i++) {
            const TAGPU_VFILE* f = &w.f[order[i]];
            const char* key = w.key[order[i]];
            const char* slash = strchr(key, '\\');
            const size_t ntopname = slash ? (size_t)(slash - key) : 0;
            uint8_t one[32];
            MODSHA sha;
            if (i && !strcmp(key, w.key[order[i - 1]])) continue;       /* a loose root file named by two archives */
            if (!units.name || !tagpu_modprint_counts(f->path, is_unit_of, &units)) continue;
            if (strlen(top) != ntopname || strncmp(top, key, ntopname)) {
                top_done(top, ntop, &topsha);
                ntop = 0;
                memcpy(top, key, ntopname);
                top[ntopname] = 0;
            }
            modsha_init(&sha);
            hash_file(v, f, buf, &sha, &bytes);
            modsha_end(&sha, one);
            for (k = 0; k < nsame; k++)
                if (!memcmp(one, same[k].here, sizeof one) && tagpu_modcat_same_path(f->path, same[k].path)) memcpy(one, same[k].dropin, sizeof one);
            modsha_add(&all, key, strlen(key) + 1);
            modsha_add(&all, one, sizeof one);
            modsha_add(&topsha, key, strlen(key) + 1);
            modsha_add(&topsha, one, sizeof one);
            counted++;
            ntop++;
        }
        top_done(top, ntop, &topsha);
        modsha_end(&all, digest);
        memcpy(s_print.content, digest, MODPRINT_HASH);
        s_printOk = units.name != NULL;
        tagpu_logf("modprint: %d files of %d, %u MB, %lu ms: exe %02x%02x%02x%02x content %02x%02x%02x%02x, %s", counted, w.n, (unsigned)(bytes >> 20),
                   (unsigned long)(GetTickCount() - t0), s_print.exe[0], s_print.exe[1], s_print.exe[2], s_print.exe[3],
                   s_print.content[0], s_print.content[1], s_print.content[2], s_print.content[3], s_print.label);
    }
    if (!s_printOk) tagpu_log("modprint: out of memory, this player sends no fingerprint");
    free(buf);
    free(same);
    free(order);
    free(units.name);
    free(w.f);
    free(w.key);
    tagpu_vfs_close(v);
    return 0;
}

static void make_label(const char* id)
{
    size_t ncat = 0;
    const char* cat = tagpu_modinst_catalog(&ncat);
    const char* mutators = tagpu_modload_mutators();
    MODABOUT about;
    char* l = s_print.label;
    size_t i;
    l[0] = 0;
    if (id[0] && cat && tagpu_modcat_about(cat, ncat, id, &about))
        snprintf(l, MODPRINT_LABEL, "%s %s%s%s", about.name, about.version, mutators[0] ? " + " : "", mutators);
    else
        snprintf(l, MODPRINT_LABEL, "%s", mutators[0] ? mutators : "the folder's own game");
    for (i = 0; l[i]; i++)
        if ((unsigned char)l[i] < 0x20 || (unsigned char)l[i] > 0x7E) l[i] = '?';
}

void tagpu_modprint_start(void)
{
    uint8_t exe[32];
    char id[33];
    if (s_started) return;
    s_started = 1;
    tagpu_replays_mount_all();
    tagpu_modload_exe_identity(exe, id, sizeof id);
    memcpy(s_print.exe, exe, MODPRINT_HASH);
    make_label(id);
    s_ntotal = tagpu_replays_archive_count();
    s_narcs = s_ntotal < TAGPU_VFS_ARCSANE ? s_ntotal : TAGPU_VFS_ARCSANE;
    if (!(s_arcs = (char (*)[TAGPU_VFS_PATHMAX])malloc(((size_t)s_narcs + 1) * TAGPU_VFS_PATHMAX))) return;
    s_narcs = tagpu_replays_archive_paths(s_arcs, s_narcs);
    s_thread = CreateThread(NULL, 0, walk_thread, NULL, 0, NULL);
    if (!s_thread) walk_thread(NULL);
}

int tagpu_modprint_started(void)
{
    return s_started;
}

const MODPRINT* tagpu_modprint_wait(void)
{
    tagpu_modprint_start();
    if (s_thread) {
        WaitForSingleObject(s_thread, INFINITE);
        CloseHandle(s_thread);
        s_thread = NULL;
    }
    return s_printOk ? &s_print : NULL;
}
#endif
