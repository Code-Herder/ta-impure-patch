#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_profiles.h"
#include "tagpu_settings.h"
#include "tagpu_log.h"
#include "tagpu_menu.h"
#include "tagpu_resource.h"
#include "tagpu_modcat.h"
#include "tagpu_modinst.h"
#include "tagpu_pak.h"

/* A MOD'S PLAY OPTIONS COME IN THREE LAYERS, each with one home (research/notes/mod-manager-plan.md):
   the built-in defaults here, for the stock game and an unidentified mod; the mod's entry in the
   catalog, whether it runs from a folder or from mods\<id>\; and the player's changes in
   profiles\<id>.ini, where a missing key means the mod's default.

   tagpu_rules.on replaces the rules of the first two layers, for a test: read_rules' keys.
   It is read as often as the player's file, so a test can change the rules at the main menu. */
#define PROFILE_CAP 16384
#define RADIUS_MAX MODRULES_RADIUS_MAX
#define RULES_LEVER "tagpu_rules.on"
#define MIGRATED ".migrated"
#define HIDE_KEY "hide."
#define MUTATOR_KEY "mutator."
#define FILE_KEY "file."

static const char* const names[MOD_N] = {
    "unknown", "retail", "escalation", "mayhem", "prota", "tazero", "twilight"
};
static int s_attached, s_mod, s_match;
static char s_version[64], s_path[64];
static char s_error[240], s_refused[400];
static TagpuResourcePolicy s_policy, s_base;
static int s_choice[TAGPU_CHOICES];     /* the player's, -1 where the file has no key */
static const char* const s_choiceKey[TAGPU_CHOICES] = { "resource_radius", "quick_build" };
static DWORD s_poll;

/* THE PLAYER'S FILE IS WRITTEN OFF THE GAME THREAD. TA is lockstep and a disk write inside a click is
   an unbounded stall (tagpu_menu_present), so a click records its value in s_pending and the render
   thread writes it, retrying once a second, and the last writer is the exit's.

   s_dirty counts the changes and a writer clears it only by the count it saw, once all it saw has
   landed. A click and the menu tick are both on the game thread, so while the tick sees s_dirty at 0
   no write is pending or can start, and it reads the file the clicks are in. */
#define PENDING_NONE (-2)
#define RETRY_MS 1000
static volatile LONG s_pending[MOD_N][TAGPU_CHOICES];
static volatile LONG s_dirty;
static CRITICAL_SECTION s_io;
static DWORD s_retryAt;                 /* writers only, under s_io */
static int s_failing;                   /* writers only, under s_io */

/* The rules of `mod`: the catalog's entry, or the built-in defaults for the stock game and an
   unidentified mod. An entry that cannot be read turns its options off, named in the log: it is
   this DLL's own resource, so only a bad build does it, and modpack.py check refuses that build. */
static MODRULES s_cache[MOD_N];
static signed char s_state[MOD_N];      /* 0 unread, 1 from the catalog, -1 none */

static TagpuResourcePolicy mod_rules(int mod)
{
    TagpuResourcePolicy off = { 0, 0, 0, 0, 0 }, on = { 1, 1, 3, 3, 1 };
    if (!s_state[mod]) {
        size_t n = 0;
        const char* cat = tagpu_modinst_catalog(&n);
        char err[200];
        int got = cat ? tagpu_modcat_rules(cat, n, names[mod], &s_cache[mod], err, sizeof err) : -1;
        if (got < 0) tagpu_logf("profiles: %s", cat ? err : "no catalog in this DLL");
        s_state[mod] = got == 1 ? 1 : -1;
    }
    if (s_state[mod] == 1) {
        TagpuResourcePolicy p;
        p.manual = s_cache[mod].manual; p.quick = s_cache[mod].quick;
        p.radius = s_cache[mod].radius; p.maximum = s_cache[mod].maximum;
        p.quick_default = s_cache[mod].quick_default;
        return p;
    }
    return mod == MOD_RETAIL ? on : off;
}

/* Why the running mod has an option off, for the log: the catalog's words, or the fallback for a
   rule from the built-in layer or the test lever. */
static const char* reason(int which)
{
    const char* r = which == TAGPU_CHOICE_RADIUS ? s_cache[s_mod].manual_reason : s_cache[s_mod].quick_reason;
    return s_state[s_mod] == 1 && r[0] ? r : "this mod does not allow it";
}

/* A whole small file into `data` (PROFILE_CAP + 1 bytes), NUL-terminated. 1 read, 0 no such file,
   -1 with the reason in `error`. The handle denies writers. */
static int read_small(const char* path, char* data, char* error, unsigned cap)
{
    HANDLE f;
    DWORD size, got;
    f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return 0;
        snprintf(error, cap, "%s: read error %lu", path, e);
        return -1;
    }
    size = GetFileSize(f, NULL);
    if (size > PROFILE_CAP || !ReadFile(f, data, size, &got, NULL) || got != size) {
        snprintf(error, cap, "%s: unreadable or larger than %d bytes", path, PROFILE_CAP);
        CloseHandle(f); return -1;
    }
    CloseHandle(f);
    if (memchr(data, 0, size)) {
        snprintf(error, cap, "%s: embedded NUL", path); return -1;
    }
    data[size] = 0;
    return 1;
}

/* The one-time move out of the old layout. A step is finished by what it leaves behind, so what did
   not finish runs again next launch: a rule file not yet renamed, a key not yet gone from impure.cfg.
   A key leaves impure.cfg only once its value is in the player's file or needs no place there.
   Renamed, never deleted, and no earlier copy overwritten, as in the store's own migration. Rule files
   held the rules the catalog now owns, so their values are dropped. A preference equal to the mod's
   default is dropped too, because the old menu wrote the default on first sight. */
static int write_choice(const char* path, const char* key, int value, char* why, unsigned cap);

static int move_rules_aside(const char* path)
{
    char to[MAX_PATH];
    int n;
    for (n = 0; n < 10; n++) {
        if (n) snprintf(to, sizeof to, "%s" MIGRATED ".%d", path, n);
        else snprintf(to, sizeof to, "%s" MIGRATED, path);
        if (MoveFileExA(path, to, 0)) {
            tagpu_logf("profiles: rule file moved aside: %s", to);
            return 1;
        }
        if (GetLastError() != ERROR_ALREADY_EXISTS && GetLastError() != ERROR_FILE_EXISTS) break;
    }
    tagpu_logf("profiles: cannot move the rule file %s aside (%lu)", path, GetLastError());
    return 0;
}

/* whether a line, not a comment, names one of the old layout's rule keys */
static int file_has_rules(const char* path)
{
    static const char* const rule[] = { "manual_snapping", "default_radius", "max_radius", "quick_build_default" };
    char data[PROFILE_CAP + 1], error[16], *line, *next;
    if (read_small(path, data, error, sizeof error) <= 0) return 0;
    for (line = data; line; line = next) {
        unsigned i;
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        while (*line == ' ' || *line == '\t') line++;
        for (i = 0; i < sizeof rule / sizeof *rule; i++) {
            size_t n = strlen(rule[i]);
            if (!_strnicmp(line, rule[i], n) && (line[n] == '=' || line[n] == ' ' || line[n] == '\t')) return 1;
        }
    }
    return 0;
}

static void migrate(void)
{
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA("profiles\\*@*.ini", &fd);
    int i, k;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            char path[MAX_PATH];
            snprintf(path, sizeof path, "profiles\\%s", fd.cFileName);
            move_rules_aside(path);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    for (i = 0; i < MOD_N; i++) {
        TagpuResourcePolicy p = mod_rules(i);
        char path[64];
        int can_write;
        snprintf(path, sizeof path, "profiles/%s.ini", names[i]);
        can_write = !file_has_rules(path) || move_rules_aside(path);
        /* a catalog that cannot be read says nothing of the mod's defaults; mod_rules logged it */
        if (s_state[i] < 0 && i != MOD_RETAIL && i != MOD_UNKNOWN) continue;
        for (k = 0; k < TAGPU_CHOICES; k++) {
            char key[64], value[16], why[160] = "", *end;
            long v;
            snprintf(key, sizeof key, "%s.%s", s_choiceKey[k], names[i]);
            if (!tagpu_settings_extra(key, value, sizeof value)) continue;
            if (!lstrcmpiA(value, "on")) v = 1;
            else if (!lstrcmpiA(value, "off")) v = 0;
            else {
                v = strtol(value, &end, 10);
                if (!*value || *end || v < 0 || v > (k == TAGPU_CHOICE_RADIUS ? RADIUS_MAX : 1)) v = -1;
            }
            if (v >= 0 && v != (k == TAGPU_CHOICE_RADIUS ? p.radius : p.quick_default)) {
                if (!can_write) continue;
                if (!write_choice(path, s_choiceKey[k], (int)v, why, sizeof why)) {
                    tagpu_logf("profiles: %s=%s stays in impure.cfg: %s", key, value, why);
                    continue;
                }
            }
            tagpu_settings_extra_drop(key);
            tagpu_logf("profiles: %s=%s moved out of impure.cfg", key, value);
        }
    }
}

void tagpu_profiles_attach(void)
{
    int i;
    if (s_attached) return;
    s_attached = 1;
    InitializeCriticalSection(&s_io);
    for (i = 0; i < MOD_N * TAGPU_CHOICES; i++) s_pending[i / TAGPU_CHOICES][i % TAGPU_CHOICES] = PENDING_NONE;
    s_policy = s_base = mod_rules(MOD_UNKNOWN);
    for (i = 0; i < TAGPU_CHOICES; i++) s_choice[i] = -1;
    if (!CreateDirectoryA("profiles", NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        tagpu_logf("profiles: cannot create profiles directory (%lu)", GetLastError());
    /* never under tagpu_defaults.off: that is a control launch, and it does not touch what it does
       not own; the store read nothing it can move when it could not read its file */
    if (!tagpu_settings_ignored()) migrate();
}

static char* trim(char* p)
{
    char* end;
    while (*p == ' ' || *p == '\t' || *p == '\r') p++;
    end = p + strlen(p);
    while (end > p && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) *--end = 0;
    return p;
}

/* The test lever's rules over `p`; the candidate stays private until the caller validated it. */
static int read_rules(const char* path, TagpuResourcePolicy* p, char* error, unsigned cap)
{
    char data[PROFILE_CAP + 1], *line, *next;
    unsigned seen = 0, number = 0;
    int r = read_small(path, data, error, cap);
    if (r <= 0) return r;
    for (line = data; line; line = next) {
        static const char* const keys[] = {
            "manual_snapping", "quick_build", "default_radius", "max_radius", "quick_build_default"
        };
        char *key, *value, *eq, *end;
        long v;
        int k;
        number++;
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        key = trim(line);
        if (!*key || *key == '#' || *key == ';') continue;
        eq = strchr(key, '=');
        if (!eq) goto invalid;
        *eq = 0; key = trim(key); value = trim(eq + 1);
        for (k = 0; k < 5 && strcmp(key, keys[k]); k++) {}
        if (k == 5 || (seen & (1u << k))) goto invalid;
        if (!strcmp(value, "on")) v = 1;
        else if (!strcmp(value, "off")) v = 0;
        else {
            v = strtol(value, &end, 10);
            if (!*value || *end) goto invalid;
        }
        if (v < 0 || v > ((k == 2 || k == 3) ? RADIUS_MAX : 1)) goto invalid;
        seen |= 1u << k;
        switch (k) {
        case 0: p->manual = (int)v; break;
        case 1: p->quick = (int)v; break;
        case 2: p->radius = (int)v; break;
        case 3: p->maximum = (int)v; break;
        case 4: p->quick_default = (int)v; break;
        }
        continue;
invalid:
        snprintf(error, cap, "%s:%u: invalid or duplicate rule", path, number);
        return -1;
    }
    return 1;
}

/* The player's changes in `path` into `choice`. A key this build does not read stays in the file
   for the build that does: `file.` and `hide.`, and whatever comes later. 1 with the file's values,
   0 with no file, -1 with the reason in `error` and `choice` untouched. */
static int read_choices(const char* path, int* choice, char* error, unsigned cap)
{
    char data[PROFILE_CAP + 1], *line, *next;
    int got[TAGPU_CHOICES], k, r = read_small(path, data, error, cap);
    unsigned number = 0, seen = 0;
    if (r < 0) return r;
    for (k = 0; k < TAGPU_CHOICES; k++) got[k] = -1;
    if (r) for (line = data; line; line = next) {
        char *key, *value, *eq, *end;
        long v;
        number++;
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        key = trim(line);
        if (!*key || *key == '#' || *key == ';') continue;
        if (!(eq = strchr(key, '='))) goto invalid;
        *eq = 0; key = trim(key); value = trim(eq + 1);
        for (k = 0; k < TAGPU_CHOICES && strcmp(key, s_choiceKey[k]); k++) {}
        if (k == TAGPU_CHOICES) continue;
        if (seen & (1u << k)) goto invalid;
        if (!strcmp(value, "on")) v = 1;
        else if (!strcmp(value, "off")) v = 0;
        else {
            v = strtol(value, &end, 10);
            if (!*value || *end) goto invalid;
        }
        if (v < 0 || v > (k == TAGPU_CHOICE_RADIUS ? RADIUS_MAX : 1)) goto invalid;
        seen |= 1u << k;
        got[k] = (int)v;
        continue;
invalid:
        snprintf(error, cap, "%s:%u: invalid or duplicate choice", path, number);
        return -1;
    }
    memcpy(choice, got, sizeof got);
    return r;
}

/* `key`=`value` in `path`, every other line kept as it is, -1 removing the key. Written aside and
   moved in, so a crash leaves the old file or the new one, and never larger than read_small takes. 1,
   or 0 with the reason in `why`. */
static int write_choice(const char* path, const char* key, int value, char* why, unsigned cap)
{
    char data[PROFILE_CAP + 1], out[PROFILE_CAP + 1], tmp[MAX_PATH], *line, *next;
    size_t klen = strlen(key);
    int at = 0, r = read_small(path, data, why, cap);
    HANDLE f;
    DWORD put;
    if (r < 0) return 0;
    if (!r) at = snprintf(out, sizeof out, "# Your choices for this mod. A missing key is the mod's default.\r\n");
    if (r) for (line = data; line; line = next) {
        char* s;
        size_t n;
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (!_strnicmp(s, key, klen) && (s[klen] == '=' || s[klen] == ' ' || s[klen] == '\t')) continue;
        n = strlen(line);
        if (!n && !next) break;
        if ((size_t)at + n + 2 > PROFILE_CAP) { snprintf(why, cap, "%s is full", path); return 0; }
        memcpy(out + at, line, n);
        at += (int)n;
        if (next) out[at++] = '\n';
        else { out[at++] = '\r'; out[at++] = '\n'; }
    }
    if (value >= 0) {
        if ((size_t)at + klen + 16 > PROFILE_CAP) { snprintf(why, cap, "%s is full", path); return 0; }
        /* the content switches read on|off (research/notes/mod-manager-plan.md) */
        if (!strncmp(key, HIDE_KEY, sizeof HIDE_KEY - 1) || !strncmp(key, MUTATOR_KEY, sizeof MUTATOR_KEY - 1) ||
            !strncmp(key, FILE_KEY, sizeof FILE_KEY - 1))
            at += snprintf(out + at, sizeof out - at, "%s=%s\r\n", key, value ? "on" : "off");
        else
            at += snprintf(out + at, sizeof out - at, "%s=%d\r\n", key, value);
    }
    snprintf(tmp, sizeof tmp, "%s.new", path);
    f = CreateFileA(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) { snprintf(why, cap, "cannot write %s (%lu)", tmp, GetLastError()); return 0; }
    r = WriteFile(f, out, (DWORD)at, &put, NULL) && put == (DWORD)at && FlushFileBuffers(f);
    CloseHandle(f);
    if (!r || !MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        snprintf(why, cap, "cannot replace %s (%lu)", path, GetLastError());
        DeleteFileA(tmp);
        return 0;
    }
    return 1;
}

static void flush(int final)
{
    LONG seen;
    int m, k, ok = 1;
    char why[200] = "";
    if (!s_attached) return;
    if (!final && s_retryAt && (LONG)(GetTickCount() - s_retryAt) < 0) return;
    seen = s_dirty;
    if (!seen) return;
    if (final) {
        /* as tagpu_settings_final: no other thread can be inside, so a lock held here is orphaned */
        if (!TryEnterCriticalSection(&s_io)) {
            tagpu_logf("profiles: the writer's lock was left held by a dead thread; the last choice is not written");
            return;
        }
    } else {
        EnterCriticalSection(&s_io);
    }
    for (m = 0; m < MOD_N; m++) for (k = 0; k < TAGPU_CHOICES; k++) {
        char path[64];
        LONG v = InterlockedExchange(&s_pending[m][k], PENDING_NONE);
        if (v == PENDING_NONE) continue;
        snprintf(path, sizeof path, "profiles/%s.ini", names[m]);
        if (!write_choice(path, s_choiceKey[k], (int)v, why, sizeof why)) {
            /* put back unless a newer click has replaced it */
            InterlockedCompareExchange(&s_pending[m][k], v, PENDING_NONE);
            ok = 0;
        }
    }
    if (ok) {
        InterlockedCompareExchange(&s_dirty, 0, seen);
        s_retryAt = 0;
        s_failing = 0;
    } else {
        if (!s_failing) tagpu_logf("profiles: a choice is not saved yet: %s", why);
        s_failing = 1;
        s_retryAt = GetTickCount() + RETRY_MS;
        if (!s_retryAt) s_retryAt = 1;
    }
    LeaveCriticalSection(&s_io);
}

void tagpu_profiles_flush(void) { flush(0); }
void tagpu_profiles_flush_now(void) { s_retryAt = 0; flush(0); }
int  tagpu_profiles_saved(void) { return InterlockedCompareExchange(&s_dirty, 0, 0) == 0; }
void tagpu_profiles_final(void) { flush(1); }

/* A hand-edited choice of an option the mod forbids has no effect, and the log says so once. */
static void note_refused(const TagpuResourcePolicy* p)
{
    char line[sizeof s_refused] = "";
    int at = 0;
    if (s_choice[TAGPU_CHOICE_RADIUS] > 0 && !p->manual)
        at = snprintf(line, sizeof line, "resource_radius in %s: %s; ", s_path, reason(TAGPU_CHOICE_RADIUS));
    if (s_choice[TAGPU_CHOICE_QUICK] == 1 && !p->quick)
        snprintf(line + at, sizeof line - at, "quick_build in %s: %s", s_path, reason(TAGPU_CHOICE_QUICK));
    if (line[0] && strcmp(line, s_refused)) tagpu_logf("profiles: refused %s", line);
    lstrcpynA(s_refused, line, sizeof s_refused);
}

void tagpu_profiles_menu_tick(void)
{
    TagpuResourcePolicy p;
    char error[sizeof s_error] = "";
    int choice[TAGPU_CHOICES], lever;
    DWORD now = GetTickCount();
    if (s_match || !s_path[0] || (s_poll && now - s_poll < 500)) return;
    s_poll = now;
    p = s_base;
    lever = read_rules(RULES_LEVER, &p, error, sizeof error);
    if (lever >= 0 && p.radius > p.maximum)
        snprintf(error, sizeof error, RULES_LEVER ": default_radius exceeds max_radius");
    memcpy(choice, s_choice, sizeof choice);
    if (!error[0] && !tagpu_settings_ignored() && !s_dirty) read_choices(s_path, choice, error, sizeof error);
    if (error[0]) {
        if (strcmp(error, s_error)) tagpu_logf("profiles: rejected %s; previous rules remain", error);
        lstrcpynA(s_error, error, sizeof s_error);
        tagpu_menu_resource_refresh();
        return;
    }
    if (memcmp(&p, &s_policy, sizeof p) || s_error[0])
        tagpu_logf("profiles: %s %s: manual=%d quick=%d default=%d maximum=%d",
                   names[s_mod], s_version, p.manual, p.quick, p.radius, p.maximum);
    s_policy = p;
    s_error[0] = 0;
    memcpy(s_choice, choice, sizeof choice);
    note_refused(&p);
    tagpu_menu_resource_refresh();
}

void tagpu_profiles_select(int mod, const char* version)
{
    unsigned n;
    int different, i;
    if (mod < 0 || mod >= MOD_N) mod = MOD_UNKNOWN;
    n = version ? (unsigned)strlen(version) : 0;
    if (n >= sizeof s_version) n = 0;
    different = mod != s_mod || !s_path[0];
    s_mod = mod;
    if (n) memcpy(s_version, version, n);
    s_version[n] = 0;
    s_base = mod_rules(mod);
    if (different) {
        s_policy = s_base;
        for (i = 0; i < TAGPU_CHOICES; i++) s_choice[i] = -1;
        tagpu_logf("profiles: %s %s: manual=%d quick=%d default=%d maximum=%d",
                   names[s_mod], s_version, s_base.manual, s_base.quick, s_base.radius, s_base.maximum);
    }
    snprintf(s_path, sizeof s_path, "profiles/%s.ini", names[mod]);
    s_match = 0; s_poll = 0;
    tagpu_profiles_menu_tick();
}

void tagpu_profiles_begin_match(void) { s_match = 1; }
const TagpuResourcePolicy* tagpu_profiles_policy(void) { return &s_policy; }
int tagpu_profiles_mod(void) { return s_mod; }

int tagpu_profiles_radius(void)
{
    int v = s_choice[TAGPU_CHOICE_RADIUS];
    if (!s_policy.manual || !tagpu_resource_live() || tagpu_settings_ignored()) return 0;
    if (v < 0) v = s_policy.radius;
    return v < s_policy.maximum ? v : s_policy.maximum;
}

int tagpu_profiles_quick(void)
{
    int v = s_choice[TAGPU_CHOICE_QUICK];
    if (!s_policy.quick || !tagpu_resource_live() || tagpu_settings_ignored()) return 0;
    return v < 0 ? s_policy.quick_default : v == 1;
}

int tagpu_profiles_choice(int which) { return s_choice[which]; }

void tagpu_profiles_restore_choice(int which, int value)
{
    if (tagpu_settings_ignored() || !s_path[0]) return;
    if (value == s_choice[which]) return;
    s_choice[which] = value;
    InterlockedExchange(&s_pending[s_mod][which], value);
    /* the value first: a writer that sees the count has the value behind it */
    InterlockedIncrement(&s_dirty);
}

void tagpu_profiles_set_radius(int value)
{
    if (s_policy.manual && value >= 0 && value <= s_policy.maximum)
        tagpu_profiles_restore_choice(TAGPU_CHOICE_RADIUS, value);
}
void tagpu_profiles_set_quick(int value)
{
    if (s_policy.quick && (value == 0 || value == 1)) tagpu_profiles_restore_choice(TAGPU_CHOICE_QUICK, value);
}
const char* tagpu_profiles_name(void)
{
    return names[s_mod];
}

static void path_of_id(const char* id, char* path, size_t cap)
{
    snprintf(path, cap, "profiles/%s.ini", id);     /* s_path's spelling: the running mod is told by comparing them */
}

int tagpu_profiles_choice_of(const char* id, int which)
{
    char path[64], error[sizeof s_error] = "";
    int choice[TAGPU_CHOICES];
    if (which < 0 || which >= TAGPU_CHOICES) return -1;
    path_of_id(id, path, sizeof path);
    if (s_path[0] && !strcmp(path, s_path)) return tagpu_profiles_choice(which);
    return read_choices(path, choice, error, sizeof error) > 0 ? choice[which] : -1;
}

/* <prefix><name>=on|off in a mod's file: 1, 0, or -1 for no key. With `name` NULL, `each` gets every name under the prefix. */
static int switch_of(const char* id, const char* prefix, const char* name, void (*each)(void*, const char*), void* ctx)
{
    char path[64], data[PROFILE_CAP + 1], error[sizeof s_error] = "", *line, *next;
    const size_t nk = strlen(prefix), na = name ? strlen(name) : 0;
    int found = -1;
    int got;
    path_of_id(id, path, sizeof path);
    /* AN ORDERING: a writer replaces this file by a rename, which fails while a reader has it open, and a read during the rename finds
       no file. The writers hold s_io. */
    if (s_attached) EnterCriticalSection(&s_io);
    got = read_small(path, data, error, sizeof error);
    if (s_attached) LeaveCriticalSection(&s_io);
    if (got <= 0) return -1;
    for (line = data; line; line = next) {
        char* key;
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        key = trim(line);
        if (strncmp(key, prefix, nk)) continue;
        if (!name) {
            char* eq = strchr(key, '=');
            if (eq) {
                char* end = eq;
                while (end > key + nk && (end[-1] == ' ' || end[-1] == '\t')) end--;
                *end = 0;
                if (each) each(ctx, key + nk);
            }
            continue;
        }
        if (_strnicmp(key + nk, name, na)) continue;
        key = trim(key + nk + na);
        if (*key != '=') continue;
        key = trim(key + 1);
        if (!strcmp(key, "on")) found = 1;
        else if (!strcmp(key, "off")) found = 0;
        else tagpu_logf("profiles: %s: %s%s is not on or off; ignored", path, prefix, name);
    }
    return found;
}

int tagpu_profiles_hide_of(const char* id, const char* archive)
{
    return switch_of(id, HIDE_KEY, archive, NULL, NULL);
}

int tagpu_profiles_mutator_of(const char* base, const char* mutator)
{
    return switch_of(base, MUTATOR_KEY, mutator, NULL, NULL);
}

int tagpu_profiles_file_of(const char* base, const char* path)
{
    return switch_of(base, FILE_KEY, path, NULL, NULL);
}

void tagpu_profiles_each_file(const char* base, void (*fn)(void* ctx, const char* path), void* ctx)
{
    switch_of(base, FILE_KEY, NULL, fn, ctx);
}

static int write_key_of(const char* id, const char* key, int value)
{
    char path[64], why[sizeof s_error] = "";
    int ok;
    if (tagpu_settings_ignored()) return 0;     /* a control launch does not touch what it does not own */
    path_of_id(id, path, sizeof path);
    CreateDirectoryA("profiles", NULL);
    EnterCriticalSection(&s_io);
    ok = write_choice(path, key, value, why, sizeof why);
    LeaveCriticalSection(&s_io);
    if (!ok) tagpu_logf("profiles: %s not changed: %s", key, why);
    return ok;
}

int tagpu_profiles_set_hide_of(const char* id, const char* archive, int value)
{
    char key[80];
    snprintf(key, sizeof key, HIDE_KEY "%s", archive);
    return write_key_of(id, key, value);
}

int tagpu_profiles_set_mutator_of(const char* base, const char* mutator, int value)
{
    char key[80];
    snprintf(key, sizeof key, MUTATOR_KEY "%s", mutator);
    return write_key_of(base, key, value);
}

int tagpu_profiles_set_file_of(const char* base, const char* path, int value)
{
    char key[128];
    snprintf(key, sizeof key, FILE_KEY "%s", path);
    return write_key_of(base, key, value);
}

void tagpu_profiles_status(char* out, unsigned cap)
{
    if (s_error[0]) snprintf(out, cap, "Profile error: %s", s_error);
    else if (!tagpu_resource_live()) snprintf(out, cap, "Resource hooks unavailable; see log");
    else snprintf(out, cap, "%s %s | %s", names[s_mod], s_version, s_path);
}
