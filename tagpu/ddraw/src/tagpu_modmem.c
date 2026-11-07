/* tagpu_modmem -- see tagpu_modmem.h */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "tagpu_addr.h"
#include "tagpu_log.h"
#include "tagpu_modcat.h"
#include "tagpu_modmem.h"
#include "tagpu_modpak.h"

#define SUFFIX_KEPT L".pre-4gb"
#define SUFFIX_TMP  L".4gb.tmp"
#define SUFFIX_OLD  L".4gb-old"
#define HEADERS_MAX 0x1000u     /* the PE headers of every known exe lie inside the first page */

static int exe_path(wchar_t* path, const wchar_t* suffix)
{
    const DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (!n || n >= MAX_PATH || (size_t)n + lstrlenW(suffix) >= MAX_PATH) return 0;
    lstrcatW(path, suffix);
    return 1;
}

static uint8_t* read_whole(const wchar_t* path, size_t* out)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    const DWORD size = f == INVALID_HANDLE_VALUE ? 0 : GetFileSize(f, NULL);
    uint8_t* data = NULL;
    DWORD got = 0;
    if (f == INVALID_HANDLE_VALUE) return NULL;
    if (size != INVALID_FILE_SIZE && size && (data = (uint8_t*)malloc(size)) && (!ReadFile(f, data, size, &got, NULL) || got != size)) {
        free(data);
        data = NULL;
    }
    CloseHandle(f);
    *out = size;
    return data;
}

int tagpu_modmem_exe_has_bit(void)
{
    wchar_t path[MAX_PATH];
    uint8_t head[HEADERS_MAX];
    DWORD got = 0;
    HANDLE f;
    int ok;
    if (!exe_path(path, L"")) return -1;
    f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return -1;
    ok = ReadFile(f, head, sizeof head, &got, NULL);
    CloseHandle(f);
    return ok ? tagpu_modpak_exe_has_laa(head, got) : -1;
}

static int write_whole(const wchar_t* path, const uint8_t* data, size_t n)
{
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD put = 0;
    int ok;
    if (f == INVALID_HANDLE_VALUE) return 0;
    ok = WriteFile(f, data, (DWORD)n, &put, NULL) && put == n && FlushFileBuffers(f);
    CloseHandle(f);
    if (!ok) DeleteFileW(path);
    return ok;
}

/* `exe` to `aside` and `fresh` to `exe`. If the second fails the first goes back, so there is a file named exe unless the machine stops between them. */
static int swap_in(const wchar_t* exe, const wchar_t* fresh, const wchar_t* aside)
{
    if (!MoveFileExW(exe, aside, MOVEFILE_REPLACE_EXISTING)) return 0;
    if (MoveFileExW(fresh, exe, 0)) return 1;
    {
        const DWORD e = GetLastError();
        MoveFileExW(aside, exe, 0);
        SetLastError(e);
    }
    return 0;
}

static int same_exe(const uint8_t* a, size_t na, const uint8_t* b, size_t nb)
{
    uint8_t ka[32], kb[32];
    return tagpu_modpak_file_key(a, na, 1, ka) && tagpu_modpak_file_key(b, nb, 1, kb) && !memcmp(ka, kb, sizeof ka);
}

int tagpu_modmem_apply(int memory, char* why, size_t nwhy)
{
    wchar_t exe[MAX_PATH], kept[MAX_PATH], tmp[MAX_PATH], old[MAX_PATH];
    uint8_t* data = NULL;
    uint8_t* backup = NULL;
    size_t n = 0, nbackup = 0;
    int has, ok = 0;
    why[0] = 0;
    if (memory == MODCAT_MEMORY_LEAVE) return 1;
    if (!exe_path(exe, L"") || !exe_path(kept, SUFFIX_KEPT) || !exe_path(tmp, SUFFIX_TMP) || !exe_path(old, SUFFIX_OLD)) {
        snprintf(why, nwhy, "the game's folder is too deep");
        return 0;
    }
    if (!(data = read_whole(exe, &n)) || (has = tagpu_modpak_exe_has_laa(data, n)) < 0) {
        snprintf(why, nwhy, "the game's exe could not be read");
        goto done;
    }
    if (has == (memory == MODCAT_MEMORY_4GB)) { ok = 1; goto done; }
    if (memory == MODCAT_MEMORY_BIT_OFF && (backup = read_whole(kept, &nbackup)) && same_exe(data, n, backup, nbackup) && !tagpu_modpak_exe_has_laa(backup, nbackup)) {
        ok = swap_in(exe, kept, old);
    } else {
        /* with no copy of this exe to put back, the same file with the bit changed */
        if (!tagpu_modpak_exe_set_laa(data, n, memory == MODCAT_MEMORY_4GB)) {
            snprintf(why, nwhy, "the game's exe could not be read");
            goto done;
        }
        ok = write_whole(tmp, data, n) && swap_in(exe, tmp, memory == MODCAT_MEMORY_4GB ? kept : old);
        DeleteFileW(tmp);
    }
    if (!ok) snprintf(why, nwhy, "the game's exe could not be replaced (error %lu)", (unsigned long)GetLastError());
    else tagpu_logf("modmem: the exe's large-address bit is %s", memory == MODCAT_MEMORY_4GB ? "on, the old file kept as .pre-4gb" : "off");
done:
    free(data);
    free(backup);
    return ok;
}

void tagpu_modmem_sweep(void)
{
    wchar_t old[MAX_PATH];
    if (exe_path(old, SUFFIX_OLD)) DeleteFileW(old);
    tagpu_logf("modmem: this process's address space ends at 0x%08lx", (unsigned long)tagpu_user_top_bound);
}
