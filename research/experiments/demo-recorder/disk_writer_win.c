/* Research-only WriteFile/FlushFileBuffers crash-boundary worker.
   The parent advances each acknowledged phase explicitly. TerminateProcess
   bypasses normal process cleanup; this does not simulate a power failure. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    if (argc != 4) return 2;
    HANDLE source = CreateFileA(argv[1], GENERIC_READ, FILE_SHARE_READ, NULL,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (source == INVALID_HANDLE_VALUE) return 3;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(source, &size) || size.QuadPart <= 0 ||
        size.QuadPart > 8 * 1024 * 1024) {
        CloseHandle(source);
        return 4;
    }
    DWORD length = (DWORD)size.QuadPart, used = 0;
    unsigned char *data = malloc(length);
    if (!data) { CloseHandle(source); return 5; }
    while (used < length) {
        DWORD n = 0;
        if (!ReadFile(source, data + used, length - used, &n, NULL) || !n) {
            CloseHandle(source); free(data); return 6;
        }
        used += n;
    }
    CloseHandle(source);
    FILE *plan = fopen(argv[2], "r");
    if (!plan) { free(data); return 7; }
    HANDLE target = CreateFileA(argv[3], GENERIC_WRITE, FILE_SHARE_READ, NULL,
                                CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (target == INVALID_HANDLE_VALUE) { fclose(plan); free(data); return 8; }
    char name[80]; unsigned long end; int sync, result = 0;
    used = 0;
    while (fscanf(plan, "%79s %lu %d", name, &end, &sync) == 3) {
        if (end < used || end > length || (sync != 0 && sync != 1)) {
            result = 9; break;
        }
        while (used < end) {
            DWORD n = 0;
            if (!WriteFile(target, data + used, (DWORD)end - used, &n, NULL) || !n) {
                result = 10; break;
            }
            used += n;
        }
        if (result) break;
        if (sync && !FlushFileBuffers(target)) { result = 11; break; }
        printf("%s\n", name);
        fflush(stdout);
        int command = getchar();
        if (command == 'K') {
            TerminateProcess(GetCurrentProcess(), 99);
            result = 12; break;
        }
        if (command != '+') { result = 13; break; }
    }
    CloseHandle(target);
    fclose(plan);
    free(data);
    return result;
}
