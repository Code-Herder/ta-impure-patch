/* Run with make windowtest, then Wine on tests/windowtest.exe on a test display.
   Exercises the real comctl32 subclass chain across DirectDraw's restoration. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "dd.h"
#include "tagpu_window.h"

CNCDDRAW g_ddraw;
static HWND owner;
static WNDPROC saved;
static int stopped, joined, destroyed, failed;
static HANDLE stop_event;
static volatile LONG renderer_finished;
static LONG owner_generation;

static void check(int yes, const char *what)
{
    if (!yes) { fprintf(stderr, "FAIL: %s\n", what); failed = 1; }
}

void tagpu_log(const char *line) { (void)line; }
void tagpu_logf(const char *fmt, ...) { (void)fmt; }
int tagpu_vk_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)wp; (void)lp;
    check(msg == WM_DESTROY, "only destruction reaches the presentation observer");
    check(tagpu_window_generation(hwnd) != 0, "observer sees the departing lifetime");
    if (hwnd != owner) return 0;
    owner_generation = tagpu_window_generation(hwnd);
    return 1;
}
HRESULT dd_RestoreDisplayMode(void)
{
    SetEvent(stop_event);
    WaitForSingleObject(g_ddraw.render.thread, INFINITE);
    CloseHandle(g_ddraw.render.thread);
    g_ddraw.render.thread = NULL;
    ++joined;
    return DD_OK;
}
void tagpu_vk_render_stop(void)
{
    check(!tagpu_window_generation(owner), "lifetime is removed before teardown");
    check(!g_ddraw.render.thread, "no renderer owns the lane during teardown");
    ++stopped;
}

void tagpu_vk_render_stop_window(LONG generation)
{
    if (generation == owner_generation) tagpu_vk_render_stop();
}

static LRESULT CALLBACK engine_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_APP) return 77;
    if (msg == WM_DESTROY) {
        ++destroyed;
        check(!tagpu_window_generation(hwnd), "engine sees removed lifetime at destruction");
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK proxy_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    return CallWindowProcA(saved, hwnd, msg, wp, lp);
}

static HWND make_window(void)
{
    HWND hwnd = CreateWindowA("ImpureWindowTest", "window lifetime test", WS_OVERLAPPEDWINDOW,
                              0, 0, 320, 240, NULL, NULL, GetModuleHandle(NULL), NULL);
    check(hwnd != NULL, "window creation");
    return hwnd;
}

static DWORD WINAPI attach_from_other_thread(void *arg)
{
    check(!tagpu_window_attach((HWND)arg), "attachment rejects a foreign thread");
    return 0;
}

static DWORD WINAPI renderer(void *arg)
{
    (void)arg;
    WaitForSingleObject(stop_event, INFINITE);
    InterlockedExchange(&renderer_finished, 1);
    return 0;
}

int main(void)
{
    WNDCLASSA cls = {0};
    HWND other;
    LONG first, previous;
    HANDLE thread;
    int i;
    cls.lpfnWndProc = engine_proc;
    cls.hInstance = GetModuleHandle(NULL);
    cls.lpszClassName = "ImpureWindowTest";
    check(RegisterClassA(&cls) != 0, "class registration");
    owner = make_window();
    thread = CreateThread(NULL, 0, attach_from_other_thread, owner, 0, NULL);
    check(thread != NULL, "test thread creation");
    if (thread) { WaitForSingleObject(thread, INFINITE); CloseHandle(thread); }
    check(tagpu_window_attach(owner), "owner-thread attachment");
    first = tagpu_window_generation(owner);
    check(first > 0, "attached lifetime is positive");
    check(tagpu_window_attach(owner) && tagpu_window_generation(owner) == first,
          "repeated cooperative level retains the same lifetime");
    saved = (WNDPROC)SetWindowLongA(owner, GWL_WNDPROC, (LONG)proxy_proc);
    check(SendMessageA(owner, WM_APP, 0, 0) == 77, "proxy forwards through the subclass");
    SetWindowLongA(owner, GWL_WNDPROC, (LONG)saved);
    check(SendMessageA(owner, WM_APP, 0, 0) == 77, "restored procedure still forwards");
    check(tagpu_window_generation(owner) == first, "DirectDraw restoration keeps ownership");

    other = make_window();
    check(tagpu_window_attach(other), "second window attachment");
    previous = tagpu_window_generation(other);
    check(previous > first, "a different window has a different lifetime");
    DestroyWindow(other);
    check(stopped == 0, "another window cannot tear down retained presentation");
    DestroyWindow(owner);
    check(stopped == 1 && joined == 0, "restored subclass tears down after final Release");
    check(!tagpu_window_generation(owner), "destroyed window loses ownership");

    owner = make_window();
    check(tagpu_window_attach(owner), "retained owner before replacement mode setup");
    previous = tagpu_window_generation(owner);
    g_ddraw.ref = 1;
    g_ddraw.hwnd = NULL;
    DestroyWindow(owner);
    check(stopped == 2 && joined == 0,
          "replacement DirectDraw object without a renderer cannot defer teardown");

    owner = make_window();
    check(tagpu_window_attach(owner), "replacement window attachment");
    check(tagpu_window_generation(owner) > previous, "replacement cannot reuse a lifetime");
    previous = tagpu_window_generation(owner);
    g_ddraw.ref = 1;
    g_ddraw.hwnd = owner;
    stop_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    check(stop_event != NULL, "renderer stop event");
    g_ddraw.render.thread = CreateThread(NULL, 0, renderer, NULL, 0, NULL);
    check(g_ddraw.render.thread != NULL, "renderer creation");
    DestroyWindow(owner);
    check(stopped == 3 && joined == 1 && renderer_finished,
          "active DirectDraw joins a real renderer before teardown");
    CloseHandle(stop_event);
    g_ddraw.ref = 0;
    owner = NULL;
    for (i = 0; i < 256; ++i) {
        other = make_window();
        check(tagpu_window_attach(other), "recycled-window attachment");
        check(tagpu_window_generation(other) > previous, "lifetimes never repeat");
        previous = tagpu_window_generation(other);
        DestroyWindow(other);
    }
    check(destroyed == 260, "every destruction reaches the engine procedure once");
    check(stopped == 3, "unrelated lifetimes leave the lane alone");
    if (!failed) puts("ok: window ownership, restored subclass, scoped teardown, 256 new lifetimes");
    return failed;
}
