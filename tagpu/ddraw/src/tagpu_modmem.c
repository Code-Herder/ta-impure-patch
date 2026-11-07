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

/* No FILE_SHARE_WRITE: a file check that replaced the exe may still be writing it, and that is a refusal rather than a torn copy. */
static uint8_t* read_whole(const wchar_t* path, size_t* out)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
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
    f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
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
    if (!ok) {
        const DWORD e = GetLastError();
        DeleteFileW(path);
        SetLastError(e);
    }
    return ok;
}

/* A name to be replaced or removed: a read-only attribute travels with a rename and would refuse it. */
static void free_name(const wchar_t* path)
{
    SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL);
}

enum { SWAP_REFUSED, SWAP_DONE, SWAP_NO_EXE };

/* `fresh` takes the exe's name, and the file that held it ends as `kept` when there is one, otherwise as `work` for the next start's sweep. `kept` is touched
   only once `fresh` is in place, so a refusal leaves the copy already there. SWAP_NO_EXE: the file could not go back, and is at `work`. */
static int swap_in(const wchar_t* exe, const wchar_t* fresh, const wchar_t* work, const wchar_t* kept, DWORD* err)
{
    free_name(work);
    if (!MoveFileExW(exe, work, MOVEFILE_REPLACE_EXISTING)) {
        *err = GetLastError();
        return SWAP_REFUSED;
    }
    if (!MoveFileExW(fresh, exe, 0)) {
        *err = GetLastError();
        return MoveFileExW(work, exe, 0) ? SWAP_REFUSED : SWAP_NO_EXE;
    }
    if (kept) {
        free_name(kept);
        if (!MoveFileExW(work, kept, MOVEFILE_REPLACE_EXISTING))
            tagpu_logf("modmem: the old exe could not be kept as .pre-4gb (error %lu), and the next start removes it", (unsigned long)GetLastError());
    }
    return SWAP_DONE;
}

static int same_exe(const uint8_t* a, size_t na, const uint8_t* b, size_t nb)
{
    uint8_t ka[32], kb[32];
    return tagpu_modpak_file_key(a, na, 1, ka) && tagpu_modpak_file_key(b, nb, 1, kb) && !memcmp(ka, kb, sizeof ka);
}

int tagpu_modmem_apply(int memory, char* why, size_t nwhy)
{
    wchar_t exe[MAX_PATH], kept[MAX_PATH], tmp[MAX_PATH], old[MAX_PATH];
    const char* const lead = memory == MODCAT_MEMORY_4GB ? "This mod needs more memory, and" : "This mod runs without the extra memory, and";
    uint8_t* data = NULL;
    uint8_t* backup = NULL;
    size_t n = 0, nbackup = 0;
    int has, swap = SWAP_REFUSED;
    DWORD err = 0;
    why[0] = 0;
    if (memory == MODCAT_MEMORY_LEAVE) return 1;
    if (!exe_path(exe, L"") || !exe_path(kept, SUFFIX_KEPT) || !exe_path(tmp, SUFFIX_TMP) || !exe_path(old, SUFFIX_OLD)) {
        snprintf(why, nwhy, "%s the game's folder is too deep, so the game stays as it is.", lead);
        return 0;
    }
    if (!(data = read_whole(exe, &n)) || (has = tagpu_modpak_exe_has_laa(data, n)) < 0) {
        snprintf(why, nwhy, "%s the game's exe could not be read, so the game stays as it is.", lead);
        goto done;
    }
    if (has == (memory == MODCAT_MEMORY_4GB)) {
        swap = SWAP_DONE;
        goto done;
    }
    if (memory == MODCAT_MEMORY_BIT_OFF && (backup = read_whole(kept, &nbackup)) && same_exe(data, n, backup, nbackup) && !tagpu_modpak_exe_has_laa(backup, nbackup)) {
        swap = swap_in(exe, kept, old, NULL, &err);
    } else if (!tagpu_modpak_exe_set_laa(data, n, memory == MODCAT_MEMORY_4GB) || !write_whole(tmp, data, n)) {
        err = GetLastError();   /* with no copy of this exe to put back, the same file with the bit changed */
    } else {
        swap = swap_in(exe, tmp, old, memory == MODCAT_MEMORY_4GB ? kept : NULL, &err);
        DeleteFileW(tmp);
    }
    if (swap == SWAP_NO_EXE)
        snprintf(why, nwhy, "The game's exe could not be replaced or put back (error %lu). Your file is TotalA.exe.4gb-old: rename it TotalA.exe.", (unsigned long)err);
    else if (swap == SWAP_REFUSED)
        snprintf(why, nwhy, "%s the game's exe could not be replaced (error %lu), so the game stays as it is.", lead, (unsigned long)err);
    else
        tagpu_logf("modmem: the exe's large-address bit is %s", memory == MODCAT_MEMORY_4GB ? "on, the old file kept as .pre-4gb" : "off");
done:
    free(data);
    free(backup);
    return swap == SWAP_DONE;
}

void tagpu_modmem_sweep(void)
{
    wchar_t old[MAX_PATH], tmp[MAX_PATH];
    if (exe_path(old, SUFFIX_OLD)) {
        free_name(old);
        DeleteFileW(old);
    }
    if (exe_path(tmp, SUFFIX_TMP)) DeleteFileW(tmp);
    tagpu_logf("modmem: this process's address space ends at 0x%08lx", (unsigned long)tagpu_user_top_bound);
}
