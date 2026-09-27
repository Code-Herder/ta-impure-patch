/* tagpu_refuse -- the report, the file, the box and the exit of a refused launch.
   The contract is in tagpu_refuse.h. */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "tagpu_log.h"
#include "tagpu_refuse.h"

void tagpu_refuse(const char* text)
{
    char path[MAX_PATH];
    const char* dir = tagpu_log_dir();

    if (!text) text = "Total Annihilation: Impure cannot start.";
    /* The file first: it survives a box nobody is there to read, and the suite reads it. */
    if (dir && *dir && _snprintf(path, sizeof path, "%sstartup-failure.txt", dir) > 0) {
        HANDLE f;
        path[sizeof path - 1] = 0;
        f = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD w;
            WriteFile(f, text, (DWORD)strlen(text), &w, NULL);
            CloseHandle(f);
        }
    }
    tagpu_log(text);
    MessageBoxA(NULL, text, "Total Annihilation: Impure cannot start",
                MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);
    ExitProcess(ERROR_BAD_EXE_FORMAT);
}
