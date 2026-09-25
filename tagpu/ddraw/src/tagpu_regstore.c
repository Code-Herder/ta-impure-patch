/* tagpu_regstore -- TotalA.exe's registry, answered from a file in a tacli test folder.
   The contract (what is served, refused and passed, the handles, the file, the threads)
   is in tagpu_regstore.h. */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hook.h"
#include "tagpu_log.h"
#include "tagpu_regstore.h"

#define RS_ROOT        "HKCU\\Software\\Cavedog Entertainment"
#define RS_ROOT_LEN    (sizeof(RS_ROOT) - 1)
#define RS_STATE_DIR   L"tacli-state"
#define RS_FILE        L"tacli-state\\registry.txt"
#define RS_TMP         L"tacli-state\\registry.txt.tmp"

/* The launch's token: tacli's scheduled task puts it on TotalA.exe's command line, and the
   engine ignores it. CmdlineArgsNormalize 0x49EE30 splits its arguments at blanks (strtok,
   loop head 0x49EED3); a token starting with '-' or '/' dispatches on its SECOND character,
   less 'B', and anything above 0x35 ('w') takes `ja 0x49F461`, the loop tail every unknown
   switch goes to [DISASSEMBLED 2026-09-25]. 'x' is 0x36 there. The token starts with none of
   the debug switches 0x4DA0E0 sets aside before that (-memfussy, -dprinton, -gonzo, ...). */
#define RS_TOKEN       L"-xtacli-test"
#define RS_TOKEN_A     "-xtacli-test"

/* The handle range: above every kernel handle (the table's 2^24 entries * 4 = 0x04000000)
   and below the predefined keys (0x80000000..). */
#define RS_BASE        0x6D5A0000u

/* The store's limits, which tools/taremote.py's RegStore enforces too: a key path of up to
   RS_PATH_MAX - 1 bytes, a value name of up to RS_NAME_MAX - 1, a value of up to RS_MAX_DATA
   (an sz's text and its NUL), RS_MAX_VALUES values a key, RS_MAX_KEYS keys (the root and every
   key under it), a file of up to RS_MAX_FILE bytes. */
#define RS_MAX_KEYS    1024
#define RS_MAX_VALUES  512
#define RS_MAX_DATA    65536u
#define RS_MAX_FILE    (4u << 20)
#define RS_PATH_MAX    512
#define RS_NAME_MAX    1024
#define RS_SEEN_MAX    256

/* What a read-only open may ask for: KEY_READ, the WOW64 view bits, SYNCHRONIZE and
   GENERIC_READ. Any other bit (a write right, DELETE, WRITE_DAC, MAXIMUM_ALLOWED, ...)
   makes the open a write. */
#define RS_READ_SAM    (KEY_READ | KEY_WOW64_32KEY | KEY_WOW64_64KEY | SYNCHRONIZE | GENERIC_READ)

enum { RS_REAL, RS_PREFIX, RS_STORE };

typedef struct {
    char*  name;
    DWORD  type;
    BYTE*  data;
    DWORD  size;
} RS_VALUE;

typedef struct {
    char*     path;
    int       stored;     /* 1: a key of the store; 0: a prefix of RS_ROOT, which holds nothing */
    RS_VALUE* v;
    int       nv, cap;
} RS_KEY;

enum { RS_BY_TOKEN = 1, RS_BY_FOLDER = 2 };

static int              s_by;         /* the signals tagpu_regstore_decide found */
static char             s_why[160];   /* what it could not establish, for the log */
static WCHAR*           s_dir;        /* the exe's folder, with its trailing backslash */
static char             s_exe[MAX_PATH] = "the exe";  /* its file name (UTF-8), for the log:
                                         TotalA.exe, or whatever else loaded this DLL there */
static int              s_active;
static int              s_dirty;
static CRITICAL_SECTION s_lock;
static RS_KEY*          s_keys;       /* RS_MAX_KEYS entries, allocated once: never moves */
static int              s_nkeys;
static WCHAR*           s_file;
static WCHAR*           s_tmp;
static unsigned long long s_seen[RS_SEEN_MAX];
static int              s_nseen;
static int              s_seenFull;
static int              s_saveLogged;

static volatile LONG s_nOpen, s_nCreated, s_nRead, s_nMiss, s_nWrite, s_nSaved, s_nSaveFail;
static volatile LONG s_nRealOpen, s_nRealRead, s_nRealClose, s_nRefused;

/* ------------------------------------------------------------------ keys and values */

static const char* rs_hive_name(HKEY h)
{
    switch ((ULONG_PTR)h) {
    case 0x80000000u: return "HKCR";
    case 0x80000001u: return "HKCU";
    case 0x80000002u: return "HKLM";
    case 0x80000003u: return "HKU";
    case 0x80000005u: return "HKCC";
    }
    return NULL;
}

static HKEY rs_hive(const char* name)
{
    if (!_stricmp(name, "HKCR")) return HKEY_CLASSES_ROOT;
    if (!_stricmp(name, "HKCU")) return HKEY_CURRENT_USER;
    if (!_stricmp(name, "HKLM")) return HKEY_LOCAL_MACHINE;
    if (!_stricmp(name, "HKU"))  return HKEY_USERS;
    if (!_stricmp(name, "HKCC")) return HKEY_CURRENT_CONFIG;
    return NULL;
}

static HKEY rs_handle(int i)
{
    return (HKEY)(ULONG_PTR)(RS_BASE + 4u * (unsigned)i);
}

/* The store key a handle names, or -1 for any handle this module did not hand out. */
static int rs_index(HKEY h)
{
    ULONG_PTR v = (ULONG_PTR)h;
    if (v < RS_BASE || v - RS_BASE >= 4u * RS_MAX_KEYS || ((v - RS_BASE) & 3u)) return -1;
    v = (v - RS_BASE) / 4u;
    return (int)v < s_nkeys ? (int)v : -1;
}

static int rs_class(const char* p)
{
    size_t n = strlen(p);
    if (n >= RS_ROOT_LEN && !_strnicmp(p, RS_ROOT, RS_ROOT_LEN) && (!p[RS_ROOT_LEN] || p[RS_ROOT_LEN] == '\\'))
        return RS_STORE;
    if (n && n < RS_ROOT_LEN && !_strnicmp(p, RS_ROOT, n) && RS_ROOT[n] == '\\')
        return RS_PREFIX;
    return RS_REAL;
}

/* The full path of `sub` under `base` in `out`, with empty components dropped: 1; 0 when
   `base` is a handle this module does not know (neither a predefined key nor a store
   handle); -1 when the path does not fit. */
static int rs_compose(HKEY base, const char* sub, char* out, size_t cap)
{
    const char* b;
    size_t n;
    int i = rs_index(base);

    if (i >= 0) b = s_keys[i].path;
    else if (!(b = rs_hive_name(base))) return 0;
    n = strlen(b);
    if (n >= cap) return -1;
    memcpy(out, b, n + 1);
    while (sub && *sub) {
        const char* e;
        while (*sub == '\\') sub++;
        if (!*sub) break;
        for (e = sub; *e && *e != '\\'; e++) {}
        if (n + 1 + (size_t)(e - sub) >= cap) return -1;
        out[n++] = '\\';
        memcpy(out + n, sub, (size_t)(e - sub));
        n += (size_t)(e - sub);
        out[n] = 0;
        sub = e;
    }
    return 1;
}

static int rs_find(const char* path)
{
    int i;
    for (i = 0; i < s_nkeys; i++)
        if (!_stricmp(s_keys[i].path, path)) return i;
    return -1;
}

static int rs_add(const char* path, int stored)
{
    char* p;
    if (s_nkeys >= RS_MAX_KEYS || !(p = _strdup(path))) return -1;
    memset(&s_keys[s_nkeys], 0, sizeof s_keys[0]);
    s_keys[s_nkeys].path = p;
    s_keys[s_nkeys].stored = stored;
    return s_nkeys++;
}

/* A store path's key, and every missing key between it and RS_ROOT: the index, or -1 when
   the table is full. *made says whether anything was created. */
static int rs_create(const char* path, int* made)
{
    char buf[RS_PATH_MAX];
    size_t n = strlen(path), p;
    int i = -1;

    *made = 0;
    if (n >= sizeof buf) return -1;
    for (p = RS_ROOT_LEN; p <= n; p++) {
        if (p < n && path[p] != '\\') continue;
        memcpy(buf, path, p);
        buf[p] = 0;
        i = rs_find(buf);
        if (i < 0) {
            if ((i = rs_add(buf, 1)) < 0) return -1;
            *made = 1;
        }
    }
    return i;
}

static RS_VALUE* rs_value(RS_KEY* k, const char* name)
{
    int j;
    if (!name) name = "";
    for (j = 0; j < k->nv; j++)
        if (!_stricmp(k->v[j].name, name)) return &k->v[j];
    return NULL;
}

/* Set a value; *changed says whether it differs from the one held. */
static LONG rs_set(RS_KEY* k, const char* name, DWORD type, const BYTE* data, DWORD size, int* changed)
{
    RS_VALUE* v;
    BYTE* d = NULL;

    *changed = 0;
    if (!name) name = "";
    if (size > RS_MAX_DATA || strlen(name) >= RS_NAME_MAX) return ERROR_INVALID_PARAMETER;
    if (size && !data) return ERROR_NOACCESS;
    v = rs_value(k, name);
    if (v && v->type == type && v->size == size && (!size || !memcmp(v->data, data, size)))
        return ERROR_SUCCESS;
    if (size) {
        if (!(d = (BYTE*)malloc(size))) return ERROR_NOT_ENOUGH_MEMORY;
        memcpy(d, data, size);
    }
    if (!v) {
        char* nm;
        if (k->nv >= RS_MAX_VALUES) { free(d); return ERROR_NOT_ENOUGH_MEMORY; }
        if (k->nv == k->cap) {
            int cap = k->cap ? k->cap * 2 : 16;
            RS_VALUE* nvv = (RS_VALUE*)realloc(k->v, (size_t)cap * sizeof *nvv);
            if (!nvv) { free(d); return ERROR_NOT_ENOUGH_MEMORY; }
            k->v = nvv;
            k->cap = cap;
        }
        if (!(nm = _strdup(name))) { free(d); return ERROR_NOT_ENOUGH_MEMORY; }
        v = &k->v[k->nv++];
        v->name = nm;
        v->data = NULL;
    }
    free(v->data);
    v->data = d;
    v->size = size;
    v->type = type;
    *changed = 1;
    return ERROR_SUCCESS;
}

/* ------------------------------------------------------------------ the file */

typedef struct { char* p; size_t n, cap; int bad; } RS_BUF;

static void rs_put(RS_BUF* b, const char* s, size_t n)
{
    if (b->bad) return;
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        char* np;
        while (b->n + n + 1 > cap) cap *= 2;
        if (cap > RS_MAX_FILE || !(np = (char*)realloc(b->p, cap))) { b->bad = 1; return; }
        b->p = np;
        b->cap = cap;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

static void rs_puts(RS_BUF* b, const char* s)
{
    rs_put(b, s, strlen(s));
}

static void rs_put_esc(RS_BUF* b, const BYTE* s, size_t n)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t i;
    for (i = 0; i < n; i++) {
        BYTE c = s[i];
        if (c == '%' || c < 0x20 || c >= 0x7F) {
            char e[3] = { '%', hex[c >> 4], hex[c & 15] };
            rs_put(b, e, 3);
        } else {
            rs_put(b, (const char*)&c, 1);
        }
    }
}

static void rs_put_value(RS_BUF* b, const RS_VALUE* v)
{
    char num[32];
    if (v->type == REG_DWORD && v->size == 4) {
        DWORD d;
        memcpy(&d, v->data, 4);
        _snprintf(num, sizeof num, "dword\t%lu", (unsigned long)d);
        num[sizeof num - 1] = 0;
        rs_puts(b, num);
    } else if (v->type == REG_SZ && v->size >= 1 && !v->data[v->size - 1]
               && !memchr(v->data, 0, v->size - 1)) {
        rs_puts(b, "sz\t");
        rs_put_esc(b, v->data, v->size - 1);
    } else {
        static const char hex[] = "0123456789abcdef";
        DWORD i;
        _snprintf(num, sizeof num, "hex(%lu)\t", (unsigned long)v->type);
        num[sizeof num - 1] = 0;
        rs_puts(b, num);
        for (i = 0; i < v->size; i++) {
            char h[2] = { hex[v->data[i] >> 4], hex[v->data[i] & 15] };
            rs_put(b, h, 2);
        }
    }
}

/* The counters, as the log states them: the store's side, the real registry's, the
   refusals. No counter of real writes exists because no hook calls a function that writes. */
static void rs_counters(char* out, size_t cap)
{
    _snprintf(out, cap, "the store served %ld opens (%ld keys created), %ld reads (%ld absent) and "
              "%ld writes, and the file was written %ld times (%ld failed); the real registry was "
              "asked for %ld read-only opens, %ld reads and %ld closes, and for no write (no hook "
              "calls a registry function that writes); %ld writes were refused",
              (long)s_nOpen, (long)s_nCreated, (long)s_nRead, (long)s_nMiss, (long)s_nWrite,
              (long)s_nSaved, (long)s_nSaveFail, (long)s_nRealOpen, (long)s_nRealRead,
              (long)s_nRealClose, (long)s_nRefused);
    out[cap - 1] = 0;
}

/* Write the whole store back when it changed, and log the
   counters: a game stopped by tacli is terminated, so the exit line never comes, and
   the last write's line is the latest count. Under s_lock. */
static void rs_save(void)
{
    RS_BUF b = { 0 };
    HANDLE f;
    DWORD wrote = 0;
    int i, j, ok;

    if (!s_dirty) return;
    rs_puts(&b, "# tacli registry store: <key> or <key>\\t<name>\\t<type>\\t<data> (tagpu_regstore.h)\r\n");
    for (i = 0; i < s_nkeys; i++) {
        const RS_KEY* k = &s_keys[i];
        if (!k->stored) continue;
        rs_put_esc(&b, (const BYTE*)k->path, strlen(k->path));
        rs_puts(&b, "\r\n");
        for (j = 0; j < k->nv; j++) {
            rs_put_esc(&b, (const BYTE*)k->path, strlen(k->path));
            rs_puts(&b, "\t");
            rs_put_esc(&b, (const BYTE*)k->v[j].name, strlen(k->v[j].name));
            rs_puts(&b, "\t");
            rs_put_value(&b, &k->v[j]);
            rs_puts(&b, "\r\n");
        }
    }
    ok = !b.bad;
    if (ok) {
        f = CreateFileW(s_tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        ok = f != INVALID_HANDLE_VALUE;
        if (ok) {
            ok = WriteFile(f, b.p, (DWORD)b.n, &wrote, NULL) && wrote == (DWORD)b.n && FlushFileBuffers(f);
            CloseHandle(f);
        }
        ok = ok && MoveFileExW(s_tmp, s_file, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }
    if (ok) {
        char c[512];
        s_dirty = 0;
        InterlockedIncrement(&s_nSaved);
        rs_counters(c, sizeof c);
        tagpu_logf("registry: tacli-state\\registry.txt written: %s", c);
    } else {
        DWORD e = b.bad ? ERROR_NOT_ENOUGH_MEMORY : GetLastError();
        InterlockedIncrement(&s_nSaveFail);
        if (!s_saveLogged) {
            s_saveLogged = 1;
            tagpu_logf("registry: the store could not be written back (error %lu); it stays changed "
                       "in memory and the next change tries again", (unsigned long)e);
        }
    }
    free(b.p);
}

static int rs_hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* %XX-unescape [s, e) into out (cap bytes, NUL-terminated): the length, or -1 for a raw
   byte the writer escapes, a broken escape, an escaped NUL when !nul_ok, or overflow. */
static int rs_unesc(const char* s, const char* e, char* out, size_t cap, int nul_ok)
{
    size_t n = 0;
    while (s < e) {
        int c = (unsigned char)*s;
        if (c < 0x20 || c >= 0x7F) return -1;
        if (c == '%') {
            int h, l;
            if (e - s < 3 || (h = rs_hexval(s[1])) < 0 || (l = rs_hexval(s[2])) < 0) return -1;
            c = h * 16 + l;
            if (!c && !nul_ok) return -1;
            s += 3;
        } else {
            s++;
        }
        if (n + 1 >= cap) return -1;
        out[n++] = (char)c;
    }
    out[n] = 0;
    return (int)n;
}

static int rs_decimal(const char* s, const char* e, unsigned long long max, unsigned long long* out)
{
    unsigned long long v = 0;
    if (s == e || e - s > 10) return 0;
    for (; s < e; s++) {
        if (*s < '0' || *s > '9') return 0;
        v = v * 10 + (unsigned)(*s - '0');
    }
    if (v > max) return 0;
    *out = v;
    return 1;
}

/* One line of the file: 1 parsed (or a comment), 0 malformed. */
static int rs_parse_line(const char* s, const char* e, BYTE* scratch)
{
    const char* f[5];
    int nf = 0, i, made, changed;
    char key[RS_PATH_MAX], name[RS_NAME_MAX];
    const char* p;
    DWORD type, size = 0;

    if (s == e || *s == '#') return 1;
    f[nf++] = s;
    for (p = s; p < e; p++)
        if (*p == '\t') {
            if (nf == 4) return 0;
            f[nf++] = p + 1;
        }
    f[nf] = e + 1;
    if (nf != 1 && nf != 4) return 0;
    if (rs_unesc(f[0], f[1] - 1, key, sizeof key, 0) < 0 || rs_class(key) != RS_STORE) return 0;
    for (p = key; *p; p++)
        if (*p == '\\' && (p[1] == '\\' || !p[1])) return 0;     /* an empty component */
    if ((i = rs_create(key, &made)) < 0) return 0;
    if (nf == 1) return 1;
    if (rs_unesc(f[1], f[2] - 1, name, sizeof name, 0) < 0) return 0;
    {
        const char* ts = f[2], *te = f[3] - 1, *ds = f[3], *de = e;
        unsigned long long v;
        if (te - ts == 5 && !memcmp(ts, "dword", 5)) {
            DWORD d;
            if (!rs_decimal(ds, de, 0xFFFFFFFFull, &v)) return 0;
            d = (DWORD)v;
            type = REG_DWORD;
            memcpy(scratch, &d, 4);
            size = 4;
        } else if (te - ts == 2 && !memcmp(ts, "sz", 2)) {
            int n = rs_unesc(ds, de, (char*)scratch, RS_MAX_DATA, 0);
            if (n < 0) return 0;
            type = REG_SZ;
            size = (DWORD)n + 1;
        } else if (te - ts > 5 && !memcmp(ts, "hex(", 4) && te[-1] == ')') {
            if (!rs_decimal(ts + 4, te - 1, 0xFFFFFFFFull, &v)) return 0;
            type = (DWORD)v;
            if ((de - ds) % 2 || (size_t)(de - ds) / 2 > RS_MAX_DATA) return 0;
            for (p = ds; p < de; p += 2) {
                int h = rs_hexval(p[0]), l = rs_hexval(p[1]);
                if (h < 0 || l < 0) return 0;
                scratch[size++] = (BYTE)(h * 16 + l);
            }
        } else {
            return 0;
        }
    }
    return rs_set(&s_keys[i], name, type, scratch, size, &changed) == ERROR_SUCCESS;
}

/* Read the file into the store: 1 when every line parsed. */
static int rs_load(int* nvalues)
{
    HANDLE f;
    LARGE_INTEGER sz;
    char* text;
    BYTE* scratch;
    DWORD got = 0;
    const char* s, *end;
    int line = 0, bad = 0, i;

    *nvalues = 0;
    f = CreateFileW(s_file, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        tagpu_logf("registry: tacli-state\\registry.txt cannot be opened (error %lu)",
                   (unsigned long)GetLastError());
        return 0;
    }
    if (!GetFileSizeEx(f, &sz) || sz.QuadPart > RS_MAX_FILE) {
        tagpu_logf("registry: tacli-state\\registry.txt is over %u bytes or has no size", RS_MAX_FILE);
        CloseHandle(f);
        return 0;
    }
    text = (char*)malloc((size_t)sz.QuadPart + 1);
    scratch = (BYTE*)malloc(RS_MAX_DATA + 1);
    if (!text || !scratch || !ReadFile(f, text, (DWORD)sz.QuadPart, &got, NULL) || got != (DWORD)sz.QuadPart) {
        tagpu_log("registry: tacli-state\\registry.txt could not be read");
        CloseHandle(f);
        free(text);
        free(scratch);
        return 0;
    }
    CloseHandle(f);
    text[got] = 0;
    for (s = text, end = text + got; s < end; ) {
        const char* e = memchr(s, '\n', (size_t)(end - s));
        const char* next = e ? e + 1 : end;
        if (!e) e = end;
        if (e > s && e[-1] == '\r') e--;
        line++;
        if (!rs_parse_line(s, e, scratch)) {
            if (++bad <= 3) tagpu_logf("registry: tacli-state\\registry.txt line %d does not parse", line);
        }
        s = next;
    }
    free(text);
    free(scratch);
    for (i = 0; i < s_nkeys; i++) *nvalues += s_keys[i].nv;
    if (bad) tagpu_logf("registry: %d line(s) of tacli-state\\registry.txt do not parse", bad);
    return !bad;
}

/* ------------------------------------------------------------------ the first-seen log */

/* One line the first time an (operation, key, value) is seen, up to RS_SEEN_MAX of them:
   the record of what a test run touched. Under s_lock. */
static void rs_seen(const char* op, const char* path, const char* name, const char* what)
{
    unsigned long long h = 1469598103934665603ull;
    const char* parts[3] = { op, path, name ? name : "\x01" };
    int i;
    for (i = 0; i < 3; i++) {
        const unsigned char* c;
        for (c = (const unsigned char*)parts[i]; *c; c++) {
            unsigned char l = (unsigned char)(*c >= 'A' && *c <= 'Z' ? *c + 32 : *c);
            h = (h ^ l) * 1099511628211ull;
        }
        h = (h ^ 0xFF) * 1099511628211ull;
    }
    for (i = 0; i < s_nseen; i++)
        if (s_seen[i] == h) return;
    if (s_nseen == RS_SEEN_MAX) {
        if (!s_seenFull) {
            s_seenFull = 1;
            tagpu_log("registry: the first-seen record is full; the counters at exit cover the rest");
        }
        return;
    }
    s_seen[s_nseen++] = h;
    if (name) tagpu_logf("registry: %s %s [%s]: %s", op, path, name, what);
    else      tagpu_logf("registry: %s %s: %s", op, path, what);
}

static void rs_describe(char* out, size_t cap, DWORD type, const BYTE* data, DWORD size)
{
    if (type == REG_DWORD && size == 4) {
        DWORD d;
        memcpy(&d, data, 4);
        _snprintf(out, cap, "dword %lu", (unsigned long)d);
    } else if ((type == REG_SZ || type == REG_EXPAND_SZ) && size) {
        char t[48];
        DWORD i, n = 0;
        for (i = 0; i < size && data[i] && n < sizeof t - 1; i++)
            t[n++] = (data[i] >= 0x20 && data[i] < 0x7F) ? (char)data[i] : '?';
        t[n] = 0;
        _snprintf(out, cap, "sz \"%s\"%s", t, (i < size && data[i]) ? "..." : "");
    } else {
        _snprintf(out, cap, "type %lu, %lu bytes", (unsigned long)type, (unsigned long)size);
    }
    out[cap - 1] = 0;
}

/* ------------------------------------------------------------------ the hooks */

static LONG rs_refuse(const char* op, const char* path, const char* name, const char* why)
{
    rs_seen(op, path, name, why);
    InterlockedIncrement(&s_nRefused);
    return ERROR_ACCESS_DENIED;
}

/* An open (create == 0) or a create of `sub` under `h`, with the rights `sam`. */
static LONG rs_open(HKEY h, LPCSTR sub, int create, REGSAM sam, PHKEY out, LPDWORD disp)
{
    char full[RS_PATH_MAX], what[64];
    const char* op = create ? "create" : "open";
    int c, i, made;
    HKEY hive;
    const char* rest;
    LONG r;

    if (!out) return ERROR_INVALID_PARAMETER;
    *out = NULL;
    EnterCriticalSection(&s_lock);
    c = rs_compose(h, sub, full, sizeof full);
    if (c == 0) {
        /* a real handle this module did not make: read-only, straight through */
        if (create || (sam & ~RS_READ_SAM)) {
            r = rs_refuse(op, "(a real handle's subkey)", sub ? sub : "", "REFUSED: a write under a real key");
            LeaveCriticalSection(&s_lock);
            return r;
        }
        LeaveCriticalSection(&s_lock);
        InterlockedIncrement(&s_nRealOpen);
        return RegOpenKeyExA(h, sub, 0, sam, out);
    }
    if (c < 0) {
        LeaveCriticalSection(&s_lock);
        return ERROR_INVALID_PARAMETER;
    }
    switch (rs_class(full)) {
    case RS_REAL:
        if (create || (sam & ~RS_READ_SAM)) {
            _snprintf(what, sizeof what, "REFUSED (%s, rights 0x%lX)", create ? "create" : "open",
                      (unsigned long)sam);
            what[sizeof what - 1] = 0;
            r = rs_refuse(op, full, NULL, what);
            LeaveCriticalSection(&s_lock);
            return r;
        }
        rs_seen(op, full, NULL, "read-only, passed to the real registry");
        LeaveCriticalSection(&s_lock);
        rest = strchr(full, '\\');
        *(rest ? (char*)rest : full + strlen(full)) = 0;
        hive = rs_hive(full);
        InterlockedIncrement(&s_nRealOpen);
        return RegOpenKeyExA(hive, rest ? rest + 1 : NULL, 0, sam & RS_READ_SAM, out);
    case RS_PREFIX:
        i = rs_find(full);
        if (i < 0) i = rs_add(full, 0);
        if (i >= 0) {
            *out = rs_handle(i);
            if (disp) *disp = REG_OPENED_EXISTING_KEY;
        }
        LeaveCriticalSection(&s_lock);
        return i >= 0 ? ERROR_SUCCESS : ERROR_NOT_ENOUGH_MEMORY;
    }
    InterlockedIncrement(&s_nOpen);
    i = rs_find(full);
    if (i < 0 && !create) {
        rs_seen(op, full, NULL, "absent (store)");
        InterlockedIncrement(&s_nMiss);
        LeaveCriticalSection(&s_lock);
        return ERROR_FILE_NOT_FOUND;
    }
    if (i < 0) {
        i = rs_create(full, &made);
        if (i < 0) {
            rs_seen(op, full, NULL, "the store's key table is full");
            LeaveCriticalSection(&s_lock);
            return ERROR_NOT_ENOUGH_MEMORY;
        }
        if (made) {
            s_dirty = 1;
            InterlockedIncrement(&s_nCreated);
            rs_seen(op, full, NULL, "created (store)");
            rs_save();
        }
        if (disp) *disp = REG_CREATED_NEW_KEY;
    } else {
        rs_seen(op, full, NULL, "served (store)");
        if (disp) *disp = REG_OPENED_EXISTING_KEY;
    }
    *out = rs_handle(i);
    LeaveCriticalSection(&s_lock);
    return ERROR_SUCCESS;
}

static LONG WINAPI rs_RegOpenKeyExA(HKEY h, LPCSTR sub, DWORD options, REGSAM sam, PHKEY out)
{
    (void)options;
    return rs_open(h, sub, 0, sam, out, NULL);
}

/* RegOpenKeyA asks for MAXIMUM_ALLOWED; off the store it becomes a read-only open. */
static LONG WINAPI rs_RegOpenKeyA(HKEY h, LPCSTR sub, PHKEY out)
{
    return rs_open(h, sub, 0, KEY_READ, out, NULL);
}

static LONG WINAPI rs_RegCreateKeyA(HKEY h, LPCSTR sub, PHKEY out)
{
    return rs_open(h, sub, 1, KEY_ALL_ACCESS, out, NULL);
}

static LONG WINAPI rs_RegCreateKeyExA(HKEY h, LPCSTR sub, DWORD reserved, LPSTR cls, DWORD options,
                                      REGSAM sam, LPSECURITY_ATTRIBUTES sa, PHKEY out, LPDWORD disp)
{
    (void)reserved; (void)cls; (void)options; (void)sa;
    return rs_open(h, sub, 1, sam, out, disp);
}

/* A value read through a prefix handle (HKCU, HKCU\Software): the real key's, read-only. */
static LONG rs_query_prefix(const char* path, LPCSTR name, LPDWORD type, LPBYTE data, LPDWORD cb)
{
    char hivename[8];
    const char* rest = strchr(path, '\\');
    size_t n = rest ? (size_t)(rest - path) : strlen(path);
    HKEY k;
    LONG r;

    if (n >= sizeof hivename) return ERROR_FILE_NOT_FOUND;
    memcpy(hivename, path, n);
    hivename[n] = 0;
    InterlockedIncrement(&s_nRealOpen);
    r = RegOpenKeyExA(rs_hive(hivename), rest ? rest + 1 : NULL, 0, KEY_QUERY_VALUE, &k);
    if (r != ERROR_SUCCESS) return r;
    InterlockedIncrement(&s_nRealRead);
    r = RegQueryValueExA(k, name, NULL, type, data, cb);
    InterlockedIncrement(&s_nRealClose);
    RegCloseKey(k);
    return r;
}

static LONG WINAPI rs_RegQueryValueExA(HKEY h, LPCSTR name, LPDWORD reserved, LPDWORD type, LPBYTE data,
                                       LPDWORD cb)
{
    char what[80], path[RS_PATH_MAX];
    RS_KEY* k;
    RS_VALUE* v;
    LONG r;
    int i;

    EnterCriticalSection(&s_lock);
    i = rs_index(h);
    if (i < 0) {
        LeaveCriticalSection(&s_lock);
        InterlockedIncrement(&s_nRealRead);
        return RegQueryValueExA(h, name, reserved, type, data, cb);
    }
    k = &s_keys[i];
    if (!k->stored) {
        strcpy(path, k->path);
        rs_seen("read", path, name ? name : "", "through a prefix key, passed to the real registry");
        LeaveCriticalSection(&s_lock);
        return rs_query_prefix(path, name, type, data, cb);
    }
    InterlockedIncrement(&s_nRead);
    v = rs_value(k, name);
    if (!v) {
        rs_seen("read", k->path, name ? name : "", "absent (store)");
        InterlockedIncrement(&s_nMiss);
        LeaveCriticalSection(&s_lock);
        return ERROR_FILE_NOT_FOUND;
    }
    rs_describe(what, sizeof what, v->type, v->data, v->size);
    rs_seen("read", k->path, v->name, what);
    if (type) *type = v->type;
    if (!data) {
        if (cb) *cb = v->size;
        r = ERROR_SUCCESS;
    } else if (!cb) {
        r = ERROR_INVALID_PARAMETER;
    } else if (*cb < v->size) {
        *cb = v->size;
        r = ERROR_MORE_DATA;
    } else {
        if (v->size) memcpy(data, v->data, v->size);
        *cb = v->size;
        r = ERROR_SUCCESS;
    }
    LeaveCriticalSection(&s_lock);
    return r;
}

/* The default value of `sub` under `h`, as a string. */
static LONG WINAPI rs_RegQueryValueA(HKEY h, LPCSTR sub, LPSTR data, PLONG cb)
{
    char full[RS_PATH_MAX], hivename[8];
    const char* rest;
    RS_VALUE* v;
    LONG r;
    DWORD size;
    const BYTE* src;
    int c, i, cls;

    EnterCriticalSection(&s_lock);
    c = rs_compose(h, sub, full, sizeof full);
    if (c == 0) {
        LeaveCriticalSection(&s_lock);
        InterlockedIncrement(&s_nRealRead);
        return RegQueryValueA(h, sub, data, cb);
    }
    if (c < 0) {
        LeaveCriticalSection(&s_lock);
        return ERROR_INVALID_PARAMETER;
    }
    cls = rs_class(full);
    if (cls != RS_STORE) {
        rs_seen("read", full, "", "read-only, passed to the real registry");
        LeaveCriticalSection(&s_lock);
        rest = strchr(full, '\\');
        c = (int)(rest ? (size_t)(rest - full) : strlen(full));
        if (c >= (int)sizeof hivename) return ERROR_FILE_NOT_FOUND;
        memcpy(hivename, full, (size_t)c);
        hivename[c] = 0;
        InterlockedIncrement(&s_nRealRead);
        return RegQueryValueA(rs_hive(hivename), rest ? rest + 1 : NULL, data, cb);
    }
    InterlockedIncrement(&s_nRead);
    i = rs_find(full);
    if (i < 0) {
        rs_seen("read", full, "", "absent key (store)");
        InterlockedIncrement(&s_nMiss);
        LeaveCriticalSection(&s_lock);
        return ERROR_FILE_NOT_FOUND;
    }
    v = rs_value(&s_keys[i], "");
    if (v && v->type != REG_SZ && v->type != REG_EXPAND_SZ) {
        LeaveCriticalSection(&s_lock);
        return ERROR_INVALID_DATA;
    }
    src = v ? v->data : (const BYTE*)"";
    size = v ? v->size : 1;
    rs_seen("read", full, "", v ? "served (store)" : "no default value: empty (store)");
    if (!data) {
        if (cb) *cb = (LONG)size;
        r = ERROR_SUCCESS;
    } else if (!cb) {
        r = ERROR_INVALID_PARAMETER;
    } else if ((DWORD)*cb < size) {
        *cb = (LONG)size;
        r = ERROR_MORE_DATA;
    } else {
        memcpy(data, src, size);
        *cb = (LONG)size;
        r = ERROR_SUCCESS;
    }
    LeaveCriticalSection(&s_lock);
    return r;
}

static LONG WINAPI rs_RegSetValueExA(HKEY h, LPCSTR name, DWORD reserved, DWORD type, const BYTE* data,
                                     DWORD cb)
{
    char what[80];
    LONG r;
    int i, changed;

    (void)reserved;
    EnterCriticalSection(&s_lock);
    i = rs_index(h);
    if (i < 0 || !s_keys[i].stored) {
        r = rs_refuse("write", i < 0 ? "(a real handle)" : s_keys[i].path, name ? name : "",
                      "REFUSED: a value write outside the store");
        LeaveCriticalSection(&s_lock);
        return r;
    }
    InterlockedIncrement(&s_nWrite);
    r = rs_set(&s_keys[i], name, type, data, cb, &changed);
    if (r == ERROR_SUCCESS) {
        rs_describe(what, sizeof what, type, data, cb);
        rs_seen("write", s_keys[i].path, name ? name : "", what);
        if (changed) {
            s_dirty = 1;
            rs_save();
        }
    }
    LeaveCriticalSection(&s_lock);
    return r;
}

static LONG WINAPI rs_RegFlushKey(HKEY h)
{
    EnterCriticalSection(&s_lock);
    if (rs_index(h) >= 0) rs_save();
    LeaveCriticalSection(&s_lock);
    return ERROR_SUCCESS;
}

static LONG WINAPI rs_RegCloseKey(HKEY h)
{
    int i;
    EnterCriticalSection(&s_lock);
    i = rs_index(h);
    if (i >= 0) rs_save();
    LeaveCriticalSection(&s_lock);
    if (i >= 0) return ERROR_SUCCESS;
    InterlockedIncrement(&s_nRealClose);
    return RegCloseKey(h);
}

/* ------------------------------------------------------------------ attach and exit */

static const struct { const char* name; PROC fn; } s_hooks[] = {
    { "RegOpenKeyExA",    (PROC)rs_RegOpenKeyExA },
    { "RegOpenKeyA",      (PROC)rs_RegOpenKeyA },
    { "RegCreateKeyA",    (PROC)rs_RegCreateKeyA },
    { "RegCreateKeyExA",  (PROC)rs_RegCreateKeyExA },
    { "RegQueryValueExA", (PROC)rs_RegQueryValueExA },
    { "RegQueryValueA",   (PROC)rs_RegQueryValueA },
    { "RegSetValueExA",   (PROC)rs_RegSetValueExA },
    { "RegFlushKey",      (PROC)rs_RegFlushKey },
    { "RegCloseKey",      (PROC)rs_RegCloseKey },
};
#define RS_NHOOKS ((int)(sizeof s_hooks / sizeof s_hooks[0]))

static int rs_is_hook(ULONG_PTR fn)
{
    int j;
    for (j = 0; j < RS_NHOOKS; j++)
        if ((ULONG_PTR)s_hooks[j].fn == fn) return 1;
    return 0;
}

/* A module's ADVAPI32 imports whose names begin with "Reg", and how many of their import
   table slots now hold one of our hooks -- read from the module's own import directory. */
static void rs_count(HMODULE m, int* regs, int* ours)
{
    BYTE* base = (BYTE*)m;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    DWORD rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    IMAGE_IMPORT_DESCRIPTOR* d;

    *regs = *ours = 0;
    if (!rva) return;
    for (d = (IMAGE_IMPORT_DESCRIPTOR*)(base + rva); d->FirstThunk; d++) {
        IMAGE_THUNK_DATA* t, *o;
        if (!d->Name || !d->OriginalFirstThunk || _stricmp((char*)(base + d->Name), "ADVAPI32.dll")) continue;
        t = (IMAGE_THUNK_DATA*)(base + d->FirstThunk);
        o = (IMAGE_THUNK_DATA*)(base + d->OriginalFirstThunk);
        for (; o->u1.AddressOfData; t++, o++) {
            const char* nm;
            if (o->u1.Ordinal & IMAGE_ORDINAL_FLAG) {
                (*regs)++;                  /* by ordinal: cannot be told apart, so counted */
                continue;
            }
            nm = (const char*)((IMAGE_IMPORT_BY_NAME*)(base + o->u1.AddressOfData))->Name;
            if (strncmp(nm, "Reg", 3)) continue;
            (*regs)++;
            if (rs_is_hook((ULONG_PTR)t->u1.Function)) (*ours)++;
        }
    }
}

static void rs_hook(HMODULE m)
{
    int j;
    for (j = 0; j < RS_NHOOKS; j++)
        hook_patch_iat(m, FALSE, "ADVAPI32.dll", (char*)s_hooks[j].name, s_hooks[j].fn);
}

/* Whether the command line carries RS_TOKEN as a whole argument. The program name is
   skipped as the CRT skips it (quoted, or up to the first blank); the rest is split at
   blanks, as the engine's parser splits it. */
static int rs_has_token(void)
{
    const WCHAR* p = GetCommandLineW();
    const size_t n = wcslen(RS_TOKEN);

    if (!p) return 0;
    if (*p == L'"') {
        for (p++; *p && *p != L'"'; p++) {}
        if (*p) p++;
    } else {
        while (*p && *p != L' ' && *p != L'\t') p++;
    }
    for (;;) {
        const WCHAR* s;
        while (*p == L' ' || *p == L'\t') p++;
        if (!*p) return 0;
        for (s = p; *p && *p != L' ' && *p != L'\t'; p++) {}
        if ((size_t)(p - s) == n && !wcsncmp(s, RS_TOKEN, n)) return 1;
    }
}

int tagpu_regstore_decide(void)
{
    const DWORD cap = 32768;
    WCHAR* exe;
    WCHAR* slash = NULL;
    WCHAR* dir;
    DWORD n, at, err;
    size_t len;

    if (rs_has_token()) s_by |= RS_BY_TOKEN;
    exe = (WCHAR*)malloc(cap * sizeof(WCHAR));
    n = exe ? GetModuleFileNameW(NULL, exe, cap) : 0;
    if (n && n < cap) slash = wcsrchr(exe, L'\\');
    if (!slash) {
        _snprintf(s_why, sizeof s_why, exe ? "the exe's own path could not be read"
                                           : "no memory to read the exe's own path");
        free(exe);
        return s_by != 0;
    }
    if (!WideCharToMultiByte(CP_UTF8, 0, slash + 1, -1, s_exe, sizeof s_exe, NULL, NULL))
        strcpy(s_exe, "the exe");
    slash[1] = 0;
    s_dir = exe;
    len = wcslen(s_dir);
    dir = (WCHAR*)malloc((len + wcslen(RS_STATE_DIR) + 1) * sizeof(WCHAR));
    if (!dir) {
        _snprintf(s_why, sizeof s_why, "no memory to look for tacli-state beside TotalA.exe");
        return s_by != 0;
    }
    memcpy(dir, s_dir, len * sizeof(WCHAR));
    wcscpy(dir + len, RS_STATE_DIR);
    at = GetFileAttributesW(dir);
    err = at == INVALID_FILE_ATTRIBUTES ? GetLastError() : 0;
    free(dir);
    if (at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_DIRECTORY))
        s_by |= RS_BY_FOLDER;
    else if (at != INVALID_FILE_ATTRIBUTES)
        _snprintf(s_why, sizeof s_why, "tacli-state beside TotalA.exe is a file, not a folder");
    else if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
        _snprintf(s_why, sizeof s_why, "no tacli-state folder beside TotalA.exe");
    else
        _snprintf(s_why, sizeof s_why, "tacli-state beside TotalA.exe could not be looked at "
                  "(error %lu), which is not a sign of a test folder", (unsigned long)err);
    s_why[sizeof s_why - 1] = 0;
    return s_by != 0;
}

/* Test mode, and something it needs is not there: the game is not run. Logged first.
   NEVER RETURNS, by its own construction rather than by TerminateProcess's: a caller
   goes on as though the store were whole (nothing else keeps a partial store from being
   served and written back), so the loop is the guarantee if the terminate should ever
   come back. */
static void __attribute__((noreturn)) rs_refuse_run(const char* by, const char* what)
{
    tagpu_logf("registry: TEST MODE, entered by %s, but %s: the game is not run", by, what);
    TerminateProcess(GetCurrentProcess(), 1);
    for (;;)
        ExitProcess(1);
}

void tagpu_regstore_init(void)
{
    const char* by;
    char what[160];
    DWORD at, aterr;
    size_t dir;
    HMODULE w32;
    int nvalues, regs, ours, wregs = 0, wours = 0;

    if (!s_by) {
        tagpu_logf("registry: real (no " RS_TOKEN_A " token, and %s)", s_why);
        return;
    }

    /* TEST MODE from here: whatever it needs and does not find ends the process. */
    by = s_by == (RS_BY_TOKEN | RS_BY_FOLDER) ? "the " RS_TOKEN_A " token and the tacli-state folder"
       : s_by == RS_BY_TOKEN ? "the " RS_TOKEN_A " token"
       : "the tacli-state folder beside TotalA.exe";
    s_active = 1;
    InitializeCriticalSection(&s_lock);
    if (!s_dir) rs_refuse_run(by, s_why);
    dir = wcslen(s_dir);
    s_file = (WCHAR*)malloc((dir + wcslen(RS_FILE) + 1) * sizeof(WCHAR));
    s_tmp  = (WCHAR*)malloc((dir + wcslen(RS_TMP) + 1) * sizeof(WCHAR));
    s_keys = (RS_KEY*)calloc(RS_MAX_KEYS, sizeof *s_keys);
    if (!s_file || !s_tmp || !s_keys) rs_refuse_run(by, "there is no memory for the store");
    memcpy(s_file, s_dir, dir * sizeof(WCHAR));
    memcpy(s_tmp, s_dir, dir * sizeof(WCHAR));
    wcscpy(s_file + dir, RS_FILE);
    wcscpy(s_tmp + dir, RS_TMP);

    at = GetFileAttributesW(s_file);
    aterr = at == INVALID_FILE_ATTRIBUTES ? GetLastError() : 0;
    if (aterr == ERROR_FILE_NOT_FOUND || aterr == ERROR_PATH_NOT_FOUND)
        rs_refuse_run(by, "there is no tacli-state\\registry.txt beside TotalA.exe");
    if (aterr) {
        _snprintf(what, sizeof what, "tacli-state\\registry.txt could not be looked at (error %lu)",
                  (unsigned long)aterr);
        what[sizeof what - 1] = 0;
        rs_refuse_run(by, what);
    }
    if (at & FILE_ATTRIBUTE_DIRECTORY)
        rs_refuse_run(by, "tacli-state\\registry.txt is a folder, not a file");
    if (!rs_load(&nvalues))
        rs_refuse_run(by, "tacli-state\\registry.txt did not load whole (the line above says why)");
    s_dirty = 0;

    rs_hook(GetModuleHandleW(NULL));
    rs_count(GetModuleHandleW(NULL), &regs, &ours);
    if (!regs || ours != regs) {
        if (!regs)
            _snprintf(what, sizeof what, "%s imports no registry function for the store to "
                      "answer (TotalA.exe imports nine), so it is not the game", s_exe);
        else
            _snprintf(what, sizeof what, "%d of %s's %d registry imports are not answered by "
                      "the store", regs - ours, s_exe, regs);
        what[sizeof what - 1] = 0;
        rs_refuse_run(by, what);
    }
    /* win32.dll is a static import of TotalA.exe, so the loader has mapped it before any
       DllMain runs; one that is not there now would load later with its imports unhooked. */
    if ((w32 = GetModuleHandleW(L"win32.dll")) == NULL)
        rs_refuse_run(by, "win32.dll, a static import of TotalA.exe, is not loaded at attach");
    rs_hook(w32);
    rs_count(w32, &wregs, &wours);
    if (wours != wregs) {
        _snprintf(what, sizeof what, "%d of win32.dll's %d registry imports are not answered "
                  "by the store", wregs - wours, wregs);
        what[sizeof what - 1] = 0;
        rs_refuse_run(by, what);
    }
    tagpu_logf("registry: TEST MODE, entered by %s -- %s's registry is "
               "tacli-state\\registry.txt: %d keys, %d values loaded; hooks: %s %d of %d "
               "registry imports, win32.dll %d of %d",
               by, s_exe, s_nkeys, nvalues, s_exe, ours, regs, wours, wregs);
}

int tagpu_regstore_active(void)
{
    return s_active;
}

void tagpu_regstore_final(void)
{
    int locked;
    char c[512];
    if (!s_active) return;
    locked = TryEnterCriticalSection(&s_lock);
    rs_counters(c, sizeof c);
    tagpu_logf("registry: test mode, at exit: %s", c);
    if (locked) LeaveCriticalSection(&s_lock);
}
