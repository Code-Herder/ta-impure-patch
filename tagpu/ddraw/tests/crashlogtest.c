/* The crash report's test: a thread faults on a known address, and the filter that was there before
   tagpu_crashlog_install runs next, so it is both the check that the chain holds and the place the
   report is read back. Exit 0 when the report names the fault; run from a folder where log\ may be
   created. A hand-run test, like logtest: `make crashlogtest`, `wine tests/crashlogtest.exe`. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "tagpu_crashlog.h"
#include "tagpu_log.h"

/* hook.c holds this in the DLL */
LPTOP_LEVEL_EXCEPTION_FILTER (WINAPI* real_SetUnhandledExceptionFilter)(LPTOP_LEVEL_EXCEPTION_FILTER) = SetUnhandledExceptionFilter;
HMODULE g_ddraw_module;     /* NULL: the log folder is beside this exe */

static char s_text[8192];
static volatile DWORD s_faultAt = 4;

static int report_says(const char* what)
{
    if (strstr(s_text, what)) return 1;
    printf("FAIL: the report does not say \"%s\"\n", what);
    return 0;
}

static LONG WINAPI after(EXCEPTION_POINTERS* ep)
{
    char path[MAX_PATH];
    FILE* f;
    size_t n = 0;
    int ok;
    (void)ep;
    _snprintf(path, sizeof path, "%scrash-report.txt", tagpu_log_dir());
    path[sizeof path - 1] = 0;
    f = fopen(path, "rb");
    if (f) { n = fread(s_text, 1, sizeof s_text - 1, f); fclose(f); }
    s_text[n] = 0;
    fputs(s_text, stdout);
    ok = report_says("exception C0000005") && report_says("access: kind 1 address 00000004") &&
         report_says("crashlogtest.exe+0x") && report_says("stack:");
    if (strstr(s_text, "+0xFFC")) { puts("FAIL: a word of zero read as the exe's"); ok = 0; }
    puts(ok ? "PASS" : "FAIL");
    fflush(stdout);
    ExitProcess(ok ? 0 : 1);
    return EXCEPTION_CONTINUE_SEARCH;
}

static DWORD WINAPI fault(void* unused)
{
    (void)unused;
    volatile int* const low = (volatile int*)(size_t)s_faultAt;
    *low = 1;
    return 0;
}

int main(void)
{
    tagpu_log_init();
    SetUnhandledExceptionFilter(after);
    tagpu_crashlog_install();
    CreateThread(NULL, 0, fault, NULL, 0, NULL);
    Sleep(30000);
    puts("FAIL: nothing faulted in 30 s, or the chain did not reach the earlier filter");
    return 1;
}
