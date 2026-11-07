/* tagpu_crashlog.c -- an unhandled exception names itself.

   A fault on a thread the engine's own handler does not cover leaves nothing behind: the process
   sits under the system's debugger or its error box, and what is seen from outside is a game that
   stopped. So the top-level filter writes where it happened to log\crash-report.txt, and then
   answers as whatever filter was there before, so a crash ends exactly as it would have.

   IT RUNS ON A THREAD THAT JUST FAULTED, with the heap and every lock in an unknown state: it
   allocates nothing and takes no lock, which is why it is not a tagpu_log line and why it opens its
   own file (the log's lock may be held by the thread that faulted, or by one that never returns).
   Only the first fault is written; a second would be the first one's consequence. No path, name or
   setting goes in the report, so a player can paste it. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "hook.h"
#include "tagpu_log.h"
#include "tagpu_crashlog.h"

enum { STACK_WORDS = 256, STACK_SHOWN = 24, NO_MODULE_BELOW = 0x10000 };

static LPTOP_LEVEL_EXCEPTION_FILTER s_prev;
static volatile LONG s_written;

/* `addr` as module+offset, or the bare address when no module holds it. */
static int where_is(char* out, size_t cap, DWORD addr)
{
    HMODULE m = NULL;
    char path[MAX_PATH];
    const char* base;
    /* GetModuleHandleEx answers NULL with the exe, so a word of zero would read as the exe's -- and
       a call through a null pointer as code of the exe's. Nothing is ever mapped this low. */
    if (addr < NO_MODULE_BELOW ||
        !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(size_t)addr, &m) ||
        !GetModuleFileNameA(m, path, sizeof path))
        return _snprintf(out, cap, "%08lX", (unsigned long)addr);
    base = strrchr(path, '\\');
    return _snprintf(out, cap, "%08lX %s+0x%lX", (unsigned long)addr, base ? base + 1 : path,
                     (unsigned long)(addr - (DWORD)(size_t)m));
}

/* The stack words that lie inside a module: the return addresses among them are the call chain. The
   walk ends at the thread's own stack base, so it cannot run off the mapping. */
static size_t stack_report(char* out, size_t cap, DWORD esp)
{
    const DWORD base = (DWORD)(size_t)((NT_TIB*)NtCurrentTeb())->StackBase;
    size_t used = 0;
    int i, shown = 0;
    for (i = 0; i < STACK_WORDS && shown < STACK_SHOWN; i++) {
        const DWORD at = esp + (DWORD)i * 4;
        HMODULE m = NULL;
        DWORD v;
        int n;
        if (at < esp || at + 4 > base) break;
        v = *(const DWORD*)(size_t)at;
        if (v < NO_MODULE_BELOW ||
            !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(size_t)v, &m))
            continue;
        if (used + 96 >= cap) break;
        n = _snprintf(out + used, cap - used, "  [esp+0x%03X] ", i * 4);
        if (n < 0) break;
        used += (size_t)n;
        n = where_is(out + used, cap - used, v);
        if (n < 0) break;
        used += (size_t)n;
        out[used++] = '\r'; out[used++] = '\n';
        shown++;
    }
    return used;
}

static LONG WINAPI crash_filter(EXCEPTION_POINTERS* ep)
{
    if (!InterlockedExchange(&s_written, 1)) {
        static char text[4096];     /* static: the faulting thread's stack may be what ran out */
        char at[160], path[MAX_PATH];
        const EXCEPTION_RECORD* r = ep->ExceptionRecord;
        const CONTEXT* c = ep->ContextRecord;
        const char* dir = tagpu_log_dir();
        size_t used;
        int n;
        where_is(at, sizeof at, (DWORD)(size_t)r->ExceptionAddress);
        at[sizeof at - 1] = 0;
        n = _snprintf(text, sizeof text,
                      "crash: exception %08lX on thread %lu at %s\r\n"
                      "access: kind %ld address %08lX\r\n"
                      "eax=%08lX ebx=%08lX ecx=%08lX edx=%08lX esi=%08lX edi=%08lX ebp=%08lX esp=%08lX\r\n"
                      "stack:\r\n",
                      (unsigned long)r->ExceptionCode, (unsigned long)GetCurrentThreadId(), at,
                      r->NumberParameters > 0 ? (long)r->ExceptionInformation[0] : -1L,
                      r->NumberParameters > 1 ? (unsigned long)r->ExceptionInformation[1] : 0UL,
                      (unsigned long)c->Eax, (unsigned long)c->Ebx, (unsigned long)c->Ecx,
                      (unsigned long)c->Edx, (unsigned long)c->Esi, (unsigned long)c->Edi,
                      (unsigned long)c->Ebp, (unsigned long)c->Esp);
        used = n > 0 && (size_t)n < sizeof text ? (size_t)n : 0;
        used += stack_report(text + used, sizeof text - used, (DWORD)c->Esp);
        if (dir && *dir && _snprintf(path, sizeof path, "%scrash-report.txt", dir) > 0) {
            HANDLE f;
            path[sizeof path - 1] = 0;
            f = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, NULL);
            if (f != INVALID_HANDLE_VALUE) {
                DWORD w;
                WriteFile(f, text, (DWORD)used, &w, NULL);
                CloseHandle(f);
            }
        }
    }
    return s_prev ? s_prev(ep) : EXCEPTION_CONTINUE_SEARCH;
}

void tagpu_crashlog_install(void)
{
    const char* dir = tagpu_log_dir();
    char path[MAX_PATH];
    /* the last run's report says nothing about this one */
    if (dir && *dir && _snprintf(path, sizeof path, "%scrash-report.txt", dir) > 0) {
        path[sizeof path - 1] = 0;
        DeleteFileA(path);
    }
    s_prev = real_SetUnhandledExceptionFilter(crash_filter);
}
