#include "IDirectDrawClipper.h"
#include "ddclipper.h"
#include "debug.h"


HRESULT __stdcall IDirectDrawClipper__QueryInterface(IDirectDrawClipperImpl* This, REFIID riid, LPVOID FAR* ppvObj)
{
    TRACE(
        "NOT_IMPLEMENTED -> %s(This=%p, riid=%08X, ppvObj=%p) [%p]\n", 
        __FUNCTION__, 
        This, 
        (unsigned int)riid, 
        ppvObj, 
        _ReturnAddress());

    HRESULT ret = E_NOINTERFACE;

    if (!ppvObj)
    {
        ret = E_INVALIDARG;
    }

    TRACE("NOT_IMPLEMENTED <- %s\n", __FUNCTION__);
    return ret;
}

ULONG __stdcall IDirectDrawClipper__AddRef(IDirectDrawClipperImpl* This)
{
    TRACE("-> %s(This=%p) [%p]\n", __FUNCTION__, This, _ReturnAddress());
    ULONG ret = ++This->ref;
    TRACE("<- %s(This ref=%u)\n", __FUNCTION__, ret);
    return ret;
}

ULONG __stdcall IDirectDrawClipper__Release(IDirectDrawClipperImpl* This)
{
    TRACE("-> %s(This=%p) [%p]\n", __FUNCTION__, This, _ReturnAddress());

    ULONG ret = --This->ref;

    if (This->ref == 0)
    {
        TRACE("     Released (%p)\n", This);

        if (This->region)
            DeleteObject(This->region);

        DeleteCriticalSection(&This->cs);

        HeapFree(GetProcessHeap(), 0, This);
    }

    TRACE("<- %s(This ref=%u)\n", __FUNCTION__, ret);
    return ret;
}

HRESULT __stdcall IDirectDrawClipper__GetClipList(
    IDirectDrawClipperImpl* This,
    LPRECT lpRect,
    LPRGNDATA lpClipList,
    LPDWORD lpdwSiz)
{
    TRACE(
        "-> %s(This=%p, lpRect=%p, lpClipList=%p, lpdwSiz=%p) [%p]\n", 
        __FUNCTION__, 
        This, 
        lpRect, 
        lpClipList, 
        lpdwSiz,
        _ReturnAddress());

    HRESULT ret = ddc_GetClipList(This, lpRect, lpClipList, lpdwSiz);

    TRACE("<- %s\n", __FUNCTION__);
    return ret;
}

HRESULT __stdcall IDirectDrawClipper__GetHWnd(IDirectDrawClipperImpl* This, HWND FAR* lphWnd)
{
    TRACE("-> %s(This=%p, lphWnd=%p) [%p]\n", __FUNCTION__, This, lphWnd, _ReturnAddress());
    HRESULT ret = ddc_GetHWnd(This, lphWnd);
    TRACE("<- %s\n", __FUNCTION__);
    return ret;
}

HRESULT __stdcall IDirectDrawClipper__Initialize(IDirectDrawClipperImpl* This, LPDIRECTDRAW lpDD, DWORD dwFlags)
{
    TRACE("-> %s(This=%p) [%p]\n", __FUNCTION__, This, _ReturnAddress());
    HRESULT ret = DD_OK;
    TRACE("<- %s\n", __FUNCTION__);
    return ret;
}

HRESULT __stdcall IDirectDrawClipper__IsClipListChanged(IDirectDrawClipperImpl* This, BOOL FAR* lpbChanged)
{
    TRACE("-> %s(This=%p, lpbChanged=%p) [%p]\n", __FUNCTION__, This, lpbChanged, _ReturnAddress());
    HRESULT ret = ddc_IsClipListChanged(This, lpbChanged);
    TRACE("<- %s\n", __FUNCTION__);
    return ret;
}

HRESULT __stdcall IDirectDrawClipper__SetClipList(IDirectDrawClipperImpl* This, LPRGNDATA lpClipList, DWORD dwFlags)
{
    TRACE("-> %s(This=%p, lpClipList=%p, dwFlags=%08X) [%p]\n", __FUNCTION__, This, lpClipList, dwFlags, _ReturnAddress());
    HRESULT ret = ddc_SetClipList(This, lpClipList, dwFlags);
    TRACE("<- %s\n", __FUNCTION__);
    return ret;
}

HRESULT __stdcall IDirectDrawClipper__SetHWnd(IDirectDrawClipperImpl* This, DWORD dwFlags, HWND hWnd)
{
    TRACE("-> %s(This=%p, dwFlags=%08X, hWnd=%p) [%p]\n", __FUNCTION__, This, dwFlags, hWnd, _ReturnAddress());
    HRESULT ret = ddc_SetHWnd(This, dwFlags, hWnd);
    TRACE("<- %s\n", __FUNCTION__);
    return ret;
}

struct IDirectDrawClipperImplVtbl g_ddc_vtbl =
{
    /*** IUnknown methods ***/
    IDirectDrawClipper__QueryInterface,
    IDirectDrawClipper__AddRef,
    IDirectDrawClipper__Release,
    /*** IDirectDrawClipper methods ***/
    IDirectDrawClipper__GetClipList,
    IDirectDrawClipper__GetHWnd,
    IDirectDrawClipper__Initialize,
    IDirectDrawClipper__IsClipListChanged,
    IDirectDrawClipper__SetClipList,
    IDirectDrawClipper__SetHWnd
};
