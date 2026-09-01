#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "IDirectDrawClipper.h"
#include "ddclipper.h"
#include "debug.h"
#include "dd.h"


HRESULT ddc_GetClipList(IDirectDrawClipperImpl* This, LPRECT lpRect, LPRGNDATA lpClipList, LPDWORD lpdwSiz)
{
    EnterCriticalSection(&This->cs);

    if (!This->region)
    {
        LeaveCriticalSection(&This->cs);
        return DDERR_NOCLIPLIST;
    }    

    if (!lpdwSiz)
    {
        LeaveCriticalSection(&This->cs);
        return DDERR_INVALIDPARAMS;
    }

    HRGN region = NULL;

    if (lpRect)
    {
        region = CreateRectRgnIndirect(lpRect);

        if (!region)
        {
            LeaveCriticalSection(&This->cs);
            return DDERR_INVALIDPARAMS;
        }

        if (CombineRgn(region, This->region, region, RGN_AND) == ERROR)
        {
            DeleteObject(region);

            LeaveCriticalSection(&This->cs);
            return DDERR_GENERIC;
        }
    }
    else
    {
        region = This->region;
    }

    *lpdwSiz = GetRegionData(region, *lpdwSiz, lpClipList);

    if (lpRect)
        DeleteObject(region);

    if (*lpdwSiz == 0)
    {
        LeaveCriticalSection(&This->cs);
        return DDERR_REGIONTOOSMALL;
    }

    LeaveCriticalSection(&This->cs);
    return DD_OK;
}

HRESULT ddc_GetHWnd(IDirectDrawClipperImpl* This, HWND FAR* lphWnd)
{
    EnterCriticalSection(&This->cs);

    if (!lphWnd)
    {
        LeaveCriticalSection(&This->cs);
        return DDERR_INVALIDPARAMS;
    }

    *lphWnd = This->hwnd;

    LeaveCriticalSection(&This->cs);
    return DD_OK;
}

HRESULT ddc_IsClipListChanged(IDirectDrawClipperImpl* This, BOOL FAR* lpbChanged)
{
    EnterCriticalSection(&This->cs);

    if (!lpbChanged)
    {
        LeaveCriticalSection(&This->cs);
        return DDERR_INVALIDPARAMS;
    }

    *lpbChanged = FALSE; /* Always return FALSE - See ddc_SetHWnd for remarks */

    LeaveCriticalSection(&This->cs);
    return DD_OK;
}

HRESULT ddc_SetClipList(IDirectDrawClipperImpl* This, LPRGNDATA lpClipList, DWORD dwFlags)
{
    EnterCriticalSection(&This->cs);

    if (This->hwnd)
    {
        LeaveCriticalSection(&This->cs);
        return DDERR_CLIPPERISUSINGHWND;
    }

    if (lpClipList)
    {
        if (!lpClipList->rdh.nCount)
        {
            LeaveCriticalSection(&This->cs);
            return DDERR_INVALIDCLIPLIST;
        }

        if (This->region)
            DeleteObject(This->region);

        RECT* rc = (RECT*)lpClipList->Buffer;

        This->region = CreateRectRgnIndirect(&rc[0]);

        if (!This->region)
        {
            LeaveCriticalSection(&This->cs);
            return DDERR_INVALIDCLIPLIST;
        }

        for (int i = 1; i < lpClipList->rdh.nCount; ++i)
        {
            HRGN region = CreateRectRgnIndirect(&rc[i]);

            if (!region)
            {
                LeaveCriticalSection(&This->cs);
                return DDERR_INVALIDCLIPLIST;
            }

            if (CombineRgn(This->region, region, This->region, RGN_OR) == ERROR)
            {
                DeleteObject(region);
                DeleteObject(This->region);
                This->region = NULL;

                LeaveCriticalSection(&This->cs);
                return DDERR_INVALIDCLIPLIST;
            }

            DeleteObject(region);
        }
    }
    else
    {
        if (This->region)
            DeleteObject(This->region);

        This->region = NULL;
    }

    LeaveCriticalSection(&This->cs);
    return DD_OK;
}

HRESULT ddc_SetHWnd(IDirectDrawClipperImpl* This, DWORD dwFlags, HWND hWnd)
{
    EnterCriticalSection(&This->cs);
    /* 
    We don't use the regions from the hwnd here since everything is emulated and we need the entire
    emulated surface to be redrawn all the time
    */
    This->hwnd = hWnd;

    if (hWnd && !This->region && g_ddraw.width)
    {
        RECT rc = { 0, 0, g_ddraw.width, g_ddraw.height };
        ddc_SetClipRect(This, &rc);
    }

    LeaveCriticalSection(&This->cs);
    return DD_OK;
}

HRESULT ddc_SetClipRect(IDirectDrawClipperImpl* This, LPRECT lpRect)
{
    EnterCriticalSection(&This->cs);

    if (This->region)
        DeleteObject(This->region);

    This->region = CreateRectRgnIndirect(lpRect);

    LeaveCriticalSection(&This->cs);
    return DD_OK;
}

HRESULT dd_CreateClipper(DWORD dwFlags, IDirectDrawClipperImpl** lplpDDClipper, IUnknown FAR* pUnkOuter)
{
    if (!lplpDDClipper)
        return DDERR_INVALIDPARAMS;

    IDirectDrawClipperImpl* c =
        (IDirectDrawClipperImpl*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(IDirectDrawClipperImpl));

    TRACE("     clipper = %p\n", c);

    c->lpVtbl = &g_ddc_vtbl;
    IDirectDrawClipper_AddRef(c);
    InitializeCriticalSection(&c->cs);

    *lplpDDClipper = c;

    return DD_OK;
}
