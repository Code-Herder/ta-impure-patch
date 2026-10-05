#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_profiles.h"
#include "tagpu_settings.h"
#include "tagpu_log.h"
#include "tagpu_menu.h"

#define PROFILE_CAP 4096
#define RADIUS_MAX 8

static const char* const names[MOD_N] = {
    "unknown", "retail", "escalation", "mayhem", "prota", "tazero", "twilight"
};
static int s_attached, s_mod, s_match;
static char s_version[64], s_general[64], s_override[192], s_selected[192];
static char s_error[240];
static TagpuResourcePolicy s_policy;
static DWORD s_poll;

static TagpuResourcePolicy defaults(int mod)
{
    int on = mod == MOD_RETAIL || mod == MOD_MAYHEM || mod == MOD_PROTA || mod == MOD_TWILIGHT;
    TagpuResourcePolicy p = { on, on, on ? 3 : 0, on ? 3 : 0, on };
    return p;
}

void tagpu_profiles_attach(void)
{
    int i;
    if (s_attached) return;
    s_attached = 1;
    s_policy = defaults(MOD_UNKNOWN);
    if (!CreateDirectoryA("profiles", NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        tagpu_logf("profiles: cannot create profiles directory (%lu)", GetLastError());
    for (i = 0; i < MOD_N; i++) {
        char path[64], data[256];
        DWORD put;
        HANDLE f;
        TagpuResourcePolicy p = defaults(i);
        int n;
        snprintf(path, sizeof path, "profiles/%s.ini", names[i]);
        f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f == INVALID_HANDLE_VALUE) {
            if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS)
                tagpu_logf("profiles: cannot create %s (%lu)", path, GetLastError());
            continue;
        }
        n = snprintf(data, sizeof data,
            "# Resource rules; version files inherit omitted keys.\r\n"
            "manual_snapping=%d\r\nquick_build=%d\r\ndefault_radius=%d\r\n"
            "max_radius=%d\r\nquick_build_default=%d\r\n",
            p.manual, p.quick, p.radius, p.maximum, p.quick_default);
        if (!WriteFile(f, data, (DWORD)n, &put, NULL) || put != (DWORD)n || !FlushFileBuffers(f))
            tagpu_logf("profiles: cannot finish creating %s (%lu)", path, GetLastError());
        CloseHandle(f);
    }
}

static char* trim(char* p)
{
    char* end;
    while (*p == ' ' || *p == '\t' || *p == '\r') p++;
    end = p + strlen(p);
    while (end > p && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) *--end = 0;
    return p;
}

/* Bounded file input. The handle denies writers, and the candidate remains
   private until both files and the resolved policy have been validated. */
static int read_profile(const char* path, TagpuResourcePolicy* p, char* error, unsigned cap)
{
    HANDLE f;
    char data[PROFILE_CAP + 1], *line, *next;
    DWORD size, got;
    unsigned seen = 0, number = 0;
    f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND) return 0;
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

static TagpuSetting radius_key(void) { return TS_RESOURCE_BASE + s_mod * 2; }
static TagpuSetting quick_key(void) { return TS_RESOURCE_BASE + s_mod * 2 + 1; }

void tagpu_profiles_menu_tick(void)
{
    TagpuResourcePolicy p;
    char error[sizeof s_error] = "";
    DWORD now = GetTickCount();
    int general, version = 0, value;
    if (s_match || !s_general[0] || (s_poll && now - s_poll < 500)) return;
    s_poll = now;
    p = defaults(s_mod);
    general = read_profile(s_general, &p, error, sizeof error);
    if (general >= 0 && s_override[0]) version = read_profile(s_override, &p, error, sizeof error);
    if (general >= 0 && version >= 0 && p.radius > p.maximum)
        snprintf(error, sizeof error, "%s: default_radius exceeds max_radius",
                 version ? s_override : s_general);
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
    lstrcpynA(s_selected, version ? s_override : s_general, sizeof s_selected);
    if (tagpu_settings_get(radius_key(), &value) && value == -1)
        tagpu_settings_set(radius_key(), p.radius);
    if (tagpu_settings_get(quick_key(), &value) && value == -1)
        tagpu_settings_set(quick_key(), p.quick_default);
    tagpu_menu_resource_refresh();
}

void tagpu_profiles_select(int mod, const char* version)
{
    static const char hex[] = "0123456789abcdef";
    unsigned i, n;
    int different;
    if (mod < 0 || mod >= MOD_N) mod = MOD_UNKNOWN;
    n = version ? (unsigned)strlen(version) : 0;
    if (n >= sizeof s_version) n = 0;
    different = mod != s_mod || strlen(s_version) != n || (n && memcmp(s_version, version, n));
    s_mod = mod;
    if (n) memcpy(s_version, version, n);
    s_version[n] = 0;
    if (different) s_policy = defaults(mod);
    snprintf(s_general, sizeof s_general, "profiles/%s.ini", names[mod]);
    s_override[0] = 0;
    if (n) {
        unsigned at = (unsigned)snprintf(s_override, sizeof s_override, "profiles/%s@", names[mod]);
        for (i = 0; i < n; i++) {
            unsigned c = (unsigned char)s_version[i];
            s_override[at++] = hex[c >> 4]; s_override[at++] = hex[c & 15];
        }
        memcpy(s_override + at, ".ini", 5);
    }
    s_match = 0; s_poll = 0;
    tagpu_profiles_menu_tick();
}

void tagpu_profiles_begin_match(void) { s_match = 1; }
const TagpuResourcePolicy* tagpu_profiles_policy(void) { return &s_policy; }
int tagpu_profiles_mod(void) { return s_mod; }

int tagpu_profiles_radius(void)
{
    int v = s_policy.radius;
    if (!s_policy.manual || tagpu_settings_ignored()) return 0;
    tagpu_settings_get(radius_key(), &v);
    if (v < 0) v = s_policy.radius;
    return v < s_policy.maximum ? v : s_policy.maximum;
}

int tagpu_profiles_quick(void)
{
    int v = s_policy.quick_default;
    if (!s_policy.quick || tagpu_settings_ignored()) return 0;
    tagpu_settings_get(quick_key(), &v);
    return v < 0 ? s_policy.quick_default : v == 1;
}

void tagpu_profiles_set_radius(int value)
{
    if (s_policy.manual && value >= 0 && value <= s_policy.maximum)
        tagpu_settings_set(radius_key(), value);
}
void tagpu_profiles_set_quick(int value)
{
    if (s_policy.quick && (value == 0 || value == 1)) tagpu_settings_set(quick_key(), value);
}
void tagpu_profiles_status(char* out, unsigned cap)
{
    if (s_error[0]) snprintf(out, cap, "Profile error: %s", s_error);
    else snprintf(out, cap, "%s %s | %s", names[s_mod], s_version, s_selected);
}
