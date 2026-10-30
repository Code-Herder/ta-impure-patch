/* tagpu_vfs.c -- the engine's file precedence over the copied mount list. Contract and rules:
   tagpu_vfs.h. The file system is Win32's in the DLL and POSIX's in the native test
   (tests/wikiassettest.c), where a loose path is matched without case, component by component,
   as the Windows file system and Wine match it, and a folder lists in name order without case. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_hpi.h"
#include "tagpu_vfs.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#endif

#define FULLMAX   (2 * TAGPU_VFS_PATHMAX)
#define LISTMAX   4096            /* a loose folder's entries the native list takes */

typedef struct {
    TAGPU_HPI* hpi;
    char       name[64];          /* the file name, for the report                              */
    char       why[160];          /* the refusal, when hpi is NULL                              */
#ifdef _WIN32
    HANDLE     h;
#else
    FILE*      f;
#endif
} ARC;

struct TAGPU_VFS {
    char cwd[TAGPU_VFS_PATHMAX];
    char lang[64];
    int  narc;
    ARC* arc;
};

static void copy(char* dst, size_t cap, const char* src)
{
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

int tagpu_vfs_match(const char* name, const char* pattern)
{
    /* the classic two-pointer glob: a mismatch after a '*' retries one character further on */
    const char *n = name, *p = pattern, *star = NULL, *mark = NULL;
    while (*n) {
        int pc = (unsigned char)*p, nc = (unsigned char)*n;
        if (pc >= 'a' && pc <= 'z') pc -= 32;
        if (nc >= 'a' && nc <= 'z') nc -= 32;
        if (*p == '*') { star = p++; mark = n; continue; }
        if (*p && (*p == '?' || pc == nc)) { p++; n++; continue; }
        if (!star) return 0;
        p = star + 1;
        n = ++mark;
    }
    while (*p == '*') p++;
    return !*p;
}

/* ---- the file system ------------------------------------------------------------------------ */

#ifdef _WIN32

static int full(const TAGPU_VFS* v, const char* rel, char* out)
{
    const int n = snprintf(out, FULLMAX, "%s\\%s", v->cwd, rel);
    return n > 0 && n < FULLMAX;
}

/* 1 a file, 2 a folder, 0 nothing */
static int loose_kind(const TAGPU_VFS* v, const char* rel, uint32_t* size)
{
    char p[FULLMAX];
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!full(v, rel, p) || !GetFileAttributesExA(p, GetFileExInfoStandard, &a)) return 0;
    if (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return 2;
    if (size) *size = a.nFileSizeHigh ? 0xFFFFFFFFu : a.nFileSizeLow;
    return 1;
}

static int loose_read(const TAGPU_VFS* v, const char* rel, uint32_t off, uint32_t n, void* dst)
{
    char p[FULLMAX];
    HANDLE h;
    DWORD got = 0;
    int ok;
    if (!full(v, rel, p)) return 0;
    h = CreateFileA(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    ok = SetFilePointer(h, (LONG)off, NULL, FILE_BEGIN) != INVALID_SET_FILE_POINTER &&
         ReadFile(h, dst, n, &got, NULL) && got == n;
    CloseHandle(h);
    return ok;
}

/* each entry of the loose folder `rel` that the OS lists for `pattern`, in its order */
typedef int (*LISTEACH)(void* ctx, const char* name, int is_dir);
static int loose_list(const TAGPU_VFS* v, const char* rel, const char* pattern, LISTEACH each, void* ctx)
{
    char p[FULLMAX], q[FULLMAX];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int go = 1;
    const int nq = snprintf(q, FULLMAX, "%s\\%s", rel, pattern);
    if (nq <= 0 || nq >= FULLMAX || !full(v, q, p)) return 1;
    if ((h = FindFirstFileA(p, &fd)) == INVALID_HANDLE_VALUE) return 1;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        go = each(ctx, fd.cFileName, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
    } while (go && FindNextFileA(h, &fd));
    FindClose(h);
    return go;
}

static int arc_read(void* ctx, uint32_t off, void* dst, uint32_t n)
{
    ARC* a = (ARC*)ctx;
    DWORD got = 0;
    return SetFilePointer(a->h, (LONG)off, NULL, FILE_BEGIN) != INVALID_SET_FILE_POINTER &&
           ReadFile(a->h, dst, n, &got, NULL) && got == n;
}

static int arc_file(const TAGPU_VFS* v, ARC* a, const char* path, uint32_t* size)
{
    char p[FULLMAX];
    DWORD hi = 0, lo;
    const int absolute = (path[0] && path[1] == ':') || path[0] == '\\' || path[0] == '/';
    if (absolute) copy(p, sizeof p, path);
    else if (!full(v, path, p)) return 0;
    a->h = CreateFileA(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (a->h == INVALID_HANDLE_VALUE) { a->h = NULL; return 0; }
    lo = GetFileSize(a->h, &hi);
    if (lo == INVALID_FILE_SIZE || hi || lo > 0x7FFFFFFFu) return 0;     /* SetFilePointer's reach */
    *size = lo;
    return 1;
}

static void arc_shut(ARC* a)
{
    if (a->h) CloseHandle(a->h);
    a->h = NULL;
}

#else /* POSIX, for the native test */

/* `rel` under the root, each component matched without case, into `out`; its kind as loose_kind */
static int resolve(const TAGPU_VFS* v, const char* rel, char* out)
{
    char part[TAGPU_VFS_PATHMAX];
    const char* p = rel;
    struct stat st;
    copy(out, FULLMAX, v->cwd);
    while (*p) {
        const char* e = strchr(p, '\\');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        DIR* d;
        struct dirent* de;
        int hit = 0;
        if (!n || n >= sizeof part) return 0;
        memcpy(part, p, n);
        part[n] = 0;
        if (!(d = opendir(out))) return 0;
        while ((de = readdir(d)) != NULL)
            if (!strcasecmp(de->d_name, part)) {
                if (strlen(out) + 1 + strlen(de->d_name) >= FULLMAX) break;
                strcat(out, "/");
                strcat(out, de->d_name);
                hit = 1;
                break;
            }
        closedir(d);
        if (!hit) return 0;
        p = e ? e + 1 : p + n;
    }
    if (stat(out, &st)) return 0;
    return S_ISDIR(st.st_mode) ? 2 : 1;
}

static int loose_kind(const TAGPU_VFS* v, const char* rel, uint32_t* size)
{
    char p[FULLMAX];
    struct stat st;
    const int k = resolve(v, rel, p);
    if (k == 1 && size) *size = stat(p, &st) || st.st_size > 0xFFFFFFFFLL ? 0xFFFFFFFFu : (uint32_t)st.st_size;
    return k;
}

static int loose_read(const TAGPU_VFS* v, const char* rel, uint32_t off, uint32_t n, void* dst)
{
    char p[FULLMAX];
    FILE* f;
    int ok;
    if (resolve(v, rel, p) != 1 || !(f = fopen(p, "rb"))) return 0;
    ok = !fseek(f, (long)off, SEEK_SET) && fread(dst, 1, n, f) == n;
    fclose(f);
    return ok;
}

typedef int (*LISTEACH)(void* ctx, const char* name, int is_dir);
static int byname(const void* a, const void* b)
{
    return strcasecmp(*(char* const*)a, *(char* const*)b);
}

static int loose_list(const TAGPU_VFS* v, const char* rel, const char* pattern, LISTEACH each, void* ctx)
{
    char p[FULLMAX], q[FULLMAX];
    char* names[LISTMAX];
    int n = 0, i, go = 1;
    DIR* d;
    struct dirent* de;
    if (resolve(v, rel, p) != 2 || !(d = opendir(p))) return 1;
    while (n < LISTMAX && (de = readdir(d)) != NULL)
        if (strcmp(de->d_name, ".") && strcmp(de->d_name, "..") && tagpu_vfs_match(de->d_name, pattern))
            if ((names[n] = (char*)malloc(strlen(de->d_name) + 1)) != NULL) strcpy(names[n++], de->d_name);
    closedir(d);
    qsort(names, (size_t)n, sizeof *names, byname);
    for (i = 0; i < n; i++) {
        struct stat st;
        if (go && names[i] && snprintf(q, sizeof q, "%s/%s", p, names[i]) < (int)sizeof q && !stat(q, &st))
            go = each(ctx, names[i], S_ISDIR(st.st_mode));
        free(names[i]);
    }
    return go;
}

static int arc_read(void* ctx, uint32_t off, void* dst, uint32_t n)
{
    ARC* a = (ARC*)ctx;
    return !fseek(a->f, (long)off, SEEK_SET) && fread(dst, 1, n, a->f) == n;
}

static int arc_file(const TAGPU_VFS* v, ARC* a, const char* path, uint32_t* size)
{
    long n;
    (void)v;
    if (!(a->f = fopen(path, "rb")) || fseek(a->f, 0, SEEK_END) || (n = ftell(a->f)) < 0 || n > 0x7FFFFFFFL) return 0;
    *size = (uint32_t)n;
    return 1;
}

static void arc_shut(ARC* a)
{
    if (a->f) fclose(a->f);
    a->f = NULL;
}

#endif

/* ---- the sources ---------------------------------------------------------------------------- */

TAGPU_VFS* tagpu_vfs_open(const char* cwd, const char (*arc)[TAGPU_VFS_PATHMAX], int narc, const char* language)
{
    TAGPU_VFS* v = (TAGPU_VFS*)calloc(1, sizeof *v);
    int i;
    if (narc > TAGPU_VFS_ARCMAX) narc = TAGPU_VFS_ARCMAX;
    if (narc < 0) narc = 0;
    if (!v || !(v->arc = (ARC*)calloc((size_t)narc + 1, sizeof *v->arc))) { free(v); return NULL; }
    copy(v->cwd, sizeof v->cwd, cwd);
    copy(v->lang, sizeof v->lang, language);
    v->narc = narc;
    for (i = 0; i < narc; i++) {
        ARC* a = &v->arc[i];
        char path[TAGPU_VFS_PATHMAX];
        const char* base;
        size_t k;
        TAGPU_HPISRC src;
        memcpy(path, arc[i], sizeof path);
        path[sizeof path - 1] = 0;
        for (base = path, k = 0; path[k]; k++)
            if (path[k] == '\\' || path[k] == '/') base = path + k + 1;
        copy(a->name, sizeof a->name, base);
        if (!arc_file(v, a, path, &src.size)) {
            arc_shut(a);
            copy(a->why, sizeof a->why, "the archive could not be opened for reading");
            continue;
        }
        src.ctx = a;
        src.read = arc_read;
        if (!(a->hpi = tagpu_hpi_open(&src, a->why, sizeof a->why))) arc_shut(a);
    }
    return v;
}

void tagpu_vfs_close(TAGPU_VFS* v)
{
    int i;
    if (!v) return;
    for (i = 0; i < v->narc; i++) {
        tagpu_hpi_close(v->arc[i].hpi);
        arc_shut(&v->arc[i]);
    }
    free(v->arc);
    free(v);
}

int tagpu_vfs_narc(const TAGPU_VFS* v)
{
    return v ? v->narc : 0;
}

const char* tagpu_vfs_arc_name(const TAGPU_VFS* v, int i)
{
    return v && i >= 0 && i < v->narc ? v->arc[i].name : "";
}

const char* tagpu_vfs_arc_refused(const TAGPU_VFS* v, int i)
{
    return v && i >= 0 && i < v->narc && !v->arc[i].hpi ? v->arc[i].why : NULL;
}

const char* tagpu_vfs_source(const TAGPU_VFS* v, int src)
{
    return src < 0 ? "loose" : tagpu_vfs_arc_name(v, src);
}

/* archive `k`'s entry for `path` when it is a file, else -1 */
static int arc_file_entry(const TAGPU_VFS* v, int k, const char* path)
{
    const int e = v->arc[k].hpi ? tagpu_hpi_find(v->arc[k].hpi, path) : -1;
    return e >= 0 && !tagpu_hpi_is_dir(v->arc[k].hpi, e) ? e : -1;
}

int tagpu_vfs_find(TAGPU_VFS* v, const char* path, TAGPU_VFILE* out)
{
    uint32_t size = 0;
    int k;
    if (!v || strlen(path) >= TAGPU_VFS_PATHMAX) return 0;
    if (loose_kind(v, path, &size) == 1) {
        out->src = -1;
        out->entry = -1;
        out->size = size;
        copy(out->path, sizeof out->path, path);
        return 1;
    }
    for (k = 0; k < v->narc; k++) {
        const int e = arc_file_entry(v, k, path);
        if (e < 0) continue;
        out->src = k;
        out->entry = e;
        out->size = tagpu_hpi_size(v->arc[k].hpi, e);
        copy(out->path, sizeof out->path, path);
        return 1;
    }
    return 0;
}

/* 0x4290F0's text: "<dir>[-<lang>]\<name>", cut at its last '.', then "." and the extension */
static int built(char* out, const char* dir, const char* lang, const char* name, const char* ext)
{
    char* dot;
    const int n = lang ? snprintf(out, TAGPU_VFS_PATHMAX, "%s-%s\\%s", dir, lang, name)
                       : snprintf(out, TAGPU_VFS_PATHMAX, "%s\\%s", dir, name);
    if (n <= 0 || n >= TAGPU_VFS_PATHMAX) return 0;
    if (ext && *ext) {
        if ((dot = strrchr(out, '.')) != NULL) *dot = 0;
        if (strlen(out) + 1 + strlen(ext) >= TAGPU_VFS_PATHMAX) return 0;
        strcat(out, ".");
        strcat(out, ext);
    }
    return 1;
}

int tagpu_vfs_find_built(TAGPU_VFS* v, const char* dir, const char* name, const char* ext, TAGPU_VFILE* out)
{
    char p[TAGPU_VFS_PATHMAX];
    if (!v) return 0;
    if (built(p, dir, v->lang, name, ext) && tagpu_vfs_find(v, p, out)) return 1;
    return built(p, dir, NULL, name, ext) && tagpu_vfs_find(v, p, out);
}

int tagpu_vfs_read(TAGPU_VFS* v, const TAGPU_VFILE* f, uint32_t off, uint32_t n, void* dst, char* why, size_t nwhy)
{
    if ((uint64_t)off + n > f->size) {
        snprintf(why, nwhy, "a read past the file's %lu bytes", (unsigned long)f->size);
        return 0;
    }
    if (f->src < 0) {
        if (!loose_read(v, f->path, off, n, dst)) { snprintf(why, nwhy, "the loose file could not be read"); return 0; }
        return 1;
    }
    if (f->src >= v->narc || !v->arc[f->src].hpi) { snprintf(why, nwhy, "no such archive"); return 0; }
    return tagpu_hpi_read(v->arc[f->src].hpi, f->entry, off, n, dst, why, nwhy);
}

unsigned char* tagpu_vfs_load(TAGPU_VFS* v, const TAGPU_VFILE* f, uint32_t max, char* why, size_t nwhy)
{
    unsigned char* b;
    if (f->size > max) {
        snprintf(why, nwhy, "%lu bytes, past the %lu this file may have", (unsigned long)f->size, (unsigned long)max);
        return NULL;
    }
    if (!(b = (unsigned char*)malloc((size_t)f->size + 1))) { snprintf(why, nwhy, "out of memory"); return NULL; }
    if (!tagpu_vfs_read(v, f, 0, f->size, b, why, nwhy)) { free(b); return NULL; }
    b[f->size] = 0;
    return b;
}

/* ---- the enumerations ----------------------------------------------------------------------- */

typedef struct {
    TAGPU_VFS*    v;
    const char*   dir;
    const char*   pattern;
    int           recursive;
    TAGPU_VFSEACH each;
    void*         ctx;
    int           go;
} WALK;

/* the mount's shadow mark for archive `k`'s entry `e` at `path`: an earlier source holds the path
   as a file and `e` is the entry a lookup of it in `k` finds */
static int shadowed(TAGPU_VFS* v, int k, int e, const char* path)
{
    int j;
    if (tagpu_hpi_shadowed(v->arc[k].hpi, e)) return 1;
    if (arc_file_entry(v, k, path) != e) return 0;
    if (loose_kind(v, path, NULL) == 1) goto mark;
    for (j = 0; j < k; j++)
        if (arc_file_entry(v, j, path) >= 0) goto mark;
    return 0;
mark:
    tagpu_hpi_shadow(v->arc[k].hpi, e);
    return 1;
}

static void walk_arc(WALK* w, int k, const char* dir);

typedef struct { WALK* w; const char* dir; } LOOSECTX;

static void walk_loose(WALK* w, const char* dir);

static int loose_each(void* ctx, const char* name, int is_dir)
{
    LOOSECTX* c = (LOOSECTX*)ctx;
    TAGPU_VFILE f;
    uint32_t size = 0;
    if (snprintf(f.path, sizeof f.path, "%s\\%s", c->dir, name) >= (int)sizeof f.path) return 1;
    if (is_dir) {
        if (c->w->recursive) walk_loose(c->w, f.path);
        return c->w->go;
    }
    if (c->w->recursive && !tagpu_vfs_match(name, c->w->pattern)) return 1;
    if (loose_kind(c->w->v, f.path, &size) != 1) return 1;
    f.src = -1;
    f.entry = -1;
    f.size = size;
    c->w->go = c->w->each(c->w->ctx, &f);
    return c->w->go;
}

static void walk_loose(WALK* w, const char* dir)
{
    LOOSECTX c;
    c.w = w;
    c.dir = dir;
    /* the recursive walk lists "<dir>\*" and filters itself; the flat one asks the OS for the
       pattern, as _findfirst does */
    loose_list(w->v, dir, w->recursive ? "*" : w->pattern, loose_each, &c);
}

static void walk_arc(WALK* w, int k, const char* dir)
{
    TAGPU_HPI* a = w->v->arc[k].hpi;
    const int d = a ? tagpu_hpi_find(a, dir) : -1;
    int i, n;
    if (d < 0 || !tagpu_hpi_is_dir(a, d)) return;
    n = tagpu_hpi_children(a, d);
    for (i = 0; i < n && w->go; i++) {
        const int e = tagpu_hpi_child(a, d, i);
        const char* name = tagpu_hpi_name(a, e);
        TAGPU_VFILE f;
        if (snprintf(f.path, sizeof f.path, "%s\\%s", dir, name) >= (int)sizeof f.path) continue;
        if (tagpu_hpi_is_dir(a, e)) {
            if (w->recursive) walk_arc(w, k, f.path);
            continue;
        }
        if (!tagpu_vfs_match(name, w->pattern) || shadowed(w->v, k, e, f.path)) continue;
        f.src = k;
        f.entry = e;
        f.size = tagpu_hpi_size(a, e);
        w->go = w->each(w->ctx, &f);
    }
}

void tagpu_vfs_enum(TAGPU_VFS* v, const char* dir, const char* pattern, int recursive, TAGPU_VFSEACH each, void* ctx)
{
    WALK w;
    int k;
    if (!v) return;
    w.v = v;
    w.dir = dir;
    w.pattern = pattern;
    w.recursive = recursive;
    w.each = each;
    w.ctx = ctx;
    w.go = 1;
    walk_loose(&w, dir);
    for (k = 0; k < v->narc && w.go; k++) walk_arc(&w, k, dir);
}
