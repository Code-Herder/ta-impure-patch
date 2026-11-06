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
#include "tagpu_pak.h"

/* A MOD'S PLAY OPTIONS COME IN THREE LAYERS, each with one home (research/notes/mod-manager-plan.md):
   the built-in defaults here, for the stock game and an unidentified mod; the mod's entry in the
   catalog, whether it runs from a folder or from mods\<id>\; and the player's changes in
   profiles\<id>.ini, where a missing key means the mod's default.

   tagpu_rules.on replaces the rules of the first two layers, for a test: read_rules' keys.
   It is read as often as the player's file, so a test can change the rules at the main menu. */
#define PROFILE_CAP 4096
#define RADIUS_MAX MODRULES_RADIUS_MAX
#define RULES_LEVER "tagpu_rules.on"
#define MIGRATED ".migrated"

static const char* const names[MOD_N] = {
    "unknown", "retail", "escalation", "mayhem", "prota", "tazero", "twilight"
};
static int s_attached, s_mod, s_match;
static char s_version[64], s_path[64];
static char s_error[240], s_refused[200];
static TagpuResourcePolicy s_policy, s_base;
static int s_choice[TAGPU_CHOICES];     /* the player's, -1 where the file has no key */
static const char* const s_choiceKey[TAGPU_CHOICES] = { "resource_radius", "quick_build" };
static DWORD s_poll;

/* The rules of `mod`: the catalog's entry, or the built-in defaults for the stock game and an
   unidentified mod. An entry that cannot be read turns its options off, named in the log: it is
   this DLL's own resource, so only a bad build does it, and modpack.py check refuses that build. */
static MODRULES s_cache[MOD_N];
static signed char s_state[MOD_N];      /* 0 unread, 1 from the catalog, -1 none */

static TagpuResourcePolicy mod_rules(int mod)
{
    TagpuResourcePolicy off = { 0, 0, 0, 0, 0 }, on = { 1, 1, 3, 3, 1 };
    if (!s_state[mod]) {
        size_t npak = 0, n = 0;
        const unsigned char* pak = tagpu_pak_resource("MODPAK", &npak);
        const unsigned char* cat = pak ? tagpu_pak_file(pak, npak, "catalog.json", &n) : NULL;
        char err[200];
        int got = cat ? tagpu_modcat_rules((const char*)cat, n, names[mod], &s_cache[mod], err, sizeof err) : -1;
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

/* The one-time move out of the old layout. Each step is finished by what it leaves behind, so an
   interrupted one simply runs again: a rule file renamed aside, a key gone from impure.cfg.
   Renamed, never deleted, as the store's own migration: a bad move is undone by hand. Rule files
   held the rules the catalog now owns; their values are dropped. A preference equal to the mod's
   default is dropped too, because the old menu wrote the default on first sight. */
static void write_choice(const char* path, const char* key, int value);

static void move_rules_aside(const char* path)
{
    char to[MAX_PATH];
    snprintf(to, sizeof to, "%s" MIGRATED, path);
    if (MoveFileExA(path, to, 0)) tagpu_logf("profiles: rule file moved aside: %s", to);
    else tagpu_logf("profiles: cannot move the rule file %s aside (%lu)", path, GetLastError());
}

static int file_has_rules(const char* path)
{
    char data[PROFILE_CAP + 1];
    DWORD got = 0;
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    if (!ReadFile(f, data, PROFILE_CAP, &got, NULL)) got = 0;
    CloseHandle(f);
    data[got] = 0;
    return strstr(data, "manual_snapping") || strstr(data, "max_radius") || strstr(data, "default_radius") ||
           strstr(data, "quick_build_default");
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
        char path[64];
        snprintf(path, sizeof path, "profiles/%s.ini", names[i]);
        if (file_has_rules(path)) move_rules_aside(path);
    }
    for (i = 0; i < MOD_N; i++) {
        TagpuResourcePolicy p = mod_rules(i);
        char path[64];
        snprintf(path, sizeof path, "profiles/%s.ini", names[i]);
        for (k = 0; k < TAGPU_CHOICES; k++) {
            char key[64], value[16], *end;
            long v;
            snprintf(key, sizeof key, "%s.%s", s_choiceKey[k], names[i]);
            if (!tagpu_settings_extra(key, value, sizeof value)) continue;
            if (!strcmp(value, "on")) v = 1;
            else if (!strcmp(value, "off")) v = 0;
            else if (!strcmp(value, "auto")) v = -1;
            else {
                v = strtol(value, &end, 10);
                if (!*value || *end || v < 0 || v > (k == TAGPU_CHOICE_RADIUS ? RADIUS_MAX : 1)) v = -1;
            }
            if (v >= 0 && v != (k == TAGPU_CHOICE_RADIUS ? p.radius : p.quick_default)) write_choice(path, s_choiceKey[k], (int)v);
            tagpu_settings_extra_drop(key);
            tagpu_logf("profiles: %s=%s moved to %s", key, value, path);
        }
    }
}

void tagpu_profiles_attach(void)
{
    int i;
    if (s_attached) return;
    s_attached = 1;
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

/* `key`=`value` in `path`, every other line kept, -1 removing the key. Written aside and moved in,
   so a crash leaves the old file or the new one. */
static void write_choice(const char* path, const char* key, int value)
{
    char data[PROFILE_CAP + 1], out[PROFILE_CAP + 96], tmp[MAX_PATH], error[120] = "", *line, *next;
    size_t klen = strlen(key);
    int at = 0, r = read_small(path, data, error, sizeof error);
    HANDLE f;
    DWORD put;
    if (r < 0) { tagpu_logf("profiles: %s; the choice was not saved", error); return; }
    if (!r) at = snprintf(out, sizeof out, "# Your choices for this mod. A missing key is the mod's default.\r\n");
    if (r) for (line = data; line; line = next) {
        char* s;
        size_t n;
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (!strncmp(s, key, klen) && (s[klen] == '=' || s[klen] == ' ' || s[klen] == '\t')) continue;
        n = strlen(line);
        if (n && line[n - 1] == '\r') line[--n] = 0;
        if (!n && !next) continue;
        if ((size_t)at + n + 3 > sizeof out - 32) { tagpu_logf("profiles: %s is full; the choice was not saved", path); return; }
        at += snprintf(out + at, sizeof out - at, "%s\r\n", line);
    }
    if (value >= 0) at += snprintf(out + at, sizeof out - at, "%s=%d\r\n", key, value);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    f = CreateFileA(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) { tagpu_logf("profiles: cannot write %s (%lu)", tmp, GetLastError()); return; }
    r = WriteFile(f, out, (DWORD)at, &put, NULL) && put == (DWORD)at && FlushFileBuffers(f);
    CloseHandle(f);
    if (!r || !MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        tagpu_logf("profiles: cannot replace %s (%lu)", path, GetLastError());
}

/* A hand-edited choice of an option the mod forbids has no effect, and the log says so once. */
static void note_refused(const TagpuResourcePolicy* p)
{
    char line[sizeof s_refused] = "";
    if (s_choice[TAGPU_CHOICE_RADIUS] > 0 && !p->manual)
        snprintf(line, sizeof line, "resource_radius in %s: %s", s_path, reason(TAGPU_CHOICE_RADIUS));
    else if (s_choice[TAGPU_CHOICE_QUICK] == 1 && !p->quick)
        snprintf(line, sizeof line, "quick_build in %s: %s", s_path, reason(TAGPU_CHOICE_QUICK));
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
    if (!error[0] && !tagpu_settings_ignored()) read_choices(s_path, choice, error, sizeof error);
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
    write_choice(s_path, s_choiceKey[which], value);
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
void tagpu_profiles_status(char* out, unsigned cap)
{
    if (s_error[0]) snprintf(out, cap, "Profile error: %s", s_error);
    else if (!tagpu_resource_live()) snprintf(out, cap, "Resource hooks unavailable; see log");
    else snprintf(out, cap, "%s %s | %s", names[s_mod], s_version, s_path);
}
