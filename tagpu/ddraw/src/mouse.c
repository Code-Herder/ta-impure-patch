#include <windows.h>
#include "debug.h"
#include "winapi_hooks.h"
#include "dd.h"
#include "hook.h"
#include "utils.h"
#include "config.h"


BOOL g_mouse_locked;
HHOOK g_mouse_hook;
HOOKPROC g_mouse_proc;

/* tagpu: on a desktop shared with a human, the game must never move or fence the
   real pointer — the snap-to-window on activation steals the cursor mid-task.
   Drop `tagpu_nowarp.on` next to the exe to suppress both (tacli does by default).
   Cached: this is a per-instance property, and mouse_lock runs on activation. */
BOOL tagpu_mouse_nowarp(void)
{
    static int cached = -1;
    if (cached < 0)
        cached = GetFileAttributesA("tagpu_nowarp.on") != INVALID_FILE_ATTRIBUTES;
    return cached != 0;
}

void mouse_lock()
{
    if (g_config.devmode || g_ddraw.bnet_active || !g_ddraw.hwnd)
        return;

    if (g_hook_active && !g_mouse_locked && !util_is_minimized(g_ddraw.hwnd))
    {
        int game_count = InterlockedExchangeAdd((LONG*)&g_ddraw.show_cursor_count, 0);
        int cur_count = real_ShowCursor(TRUE) - 1;
        real_ShowCursor(FALSE);

        if (cur_count > game_count)
        {
            while (real_ShowCursor(FALSE) > game_count);
        }
        else if (cur_count < game_count)
        {
            while (real_ShowCursor(TRUE) < game_count);
        }

        real_SetCursor((HCURSOR)InterlockedExchangeAdd((LONG*)&g_ddraw.old_cursor, 0));

        RECT rc = { 0 };
        real_GetClientRect(g_ddraw.hwnd, &rc);
        real_MapWindowPoints(g_ddraw.hwnd, HWND_DESKTOP, (LPPOINT)&rc, 2);
        OffsetRect(&rc, g_ddraw.render.viewport.x, g_ddraw.render.viewport.y);

        int cur_x = InterlockedExchangeAdd((LONG*)&g_ddraw.cursor.x, 0);
        int cur_y = InterlockedExchangeAdd((LONG*)&g_ddraw.cursor.y, 0);

        if (!tagpu_mouse_nowarp())
        {
            real_SetCursorPos(
                g_config.adjmouse ? (int)(rc.left + (cur_x * g_ddraw.mouse.scale_x)) : rc.left + cur_x,
                g_config.adjmouse ? (int)(rc.top + (cur_y * g_ddraw.mouse.scale_y)) : rc.top + cur_y);

            CopyRect(&rc, &g_ddraw.mouse.rc);
            real_MapWindowPoints(g_ddraw.hwnd, HWND_DESKTOP, (LPPOINT)&rc, 2);
            real_ClipCursor(&rc);
        }

        g_mouse_locked = TRUE;
    }
}

void mouse_unlock()
{
    if (g_config.devmode || !g_hook_active || !g_ddraw.hwnd)
        return;

    if (g_mouse_locked)
    {
        g_mouse_locked = FALSE;

        real_ClipCursor(NULL);
        ReleaseCapture();

        RECT rc = { 0 };
        real_GetClientRect(g_ddraw.hwnd, &rc);
        real_MapWindowPoints(g_ddraw.hwnd, HWND_DESKTOP, (LPPOINT)&rc, 2);
        OffsetRect(&rc, g_ddraw.render.viewport.x, g_ddraw.render.viewport.y);

        int cur_x = InterlockedExchangeAdd((LONG*)&g_ddraw.cursor.x, 0);
        int cur_y = InterlockedExchangeAdd((LONG*)&g_ddraw.cursor.y, 0);

        if (!tagpu_mouse_nowarp())
        {
            real_SetCursorPos(
                (int)(rc.left + (cur_x * g_ddraw.mouse.scale_x)),
                (int)(rc.top + (cur_y * g_ddraw.mouse.scale_y)));
        }

        real_SetCursor(LoadCursor(NULL, IDC_ARROW));

        while (real_ShowCursor(TRUE) < 0);
    }
}

LRESULT CALLBACK mouse_hook_proc(int Code, WPARAM wParam, LPARAM lParam)
{
    if (!g_ddraw.ref)
        return g_mouse_proc(Code, wParam, lParam);

    if (Code < 0 || (!g_config.devmode && !g_mouse_locked))
        return CallNextHookEx(g_mouse_hook, Code, wParam, lParam);

    cursorpos_internal(&((MOUSEHOOKSTRUCT*)lParam)->pt);

    return g_mouse_proc(Code, wParam, lParam);
}
