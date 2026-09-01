#include "IDirectDrawPalette.h"
#include "ddpalette.h"
#include "ddsurface.h"
#include "debug.h"


HRESULT __stdcall IDirectDrawPalette__QueryInterface(IDirectDrawPaletteImpl* This, REFIID riid, LPVOID FAR* ppvObj)
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

ULONG __stdcall IDirectDrawPalette__AddRef(IDirectDrawPaletteImpl* This)
{
    TRACE("-> %s(This=%p) [%p]\n", __FUNCTION__, This, _ReturnAddress());
    ULONG ret = ++This->ref;
    TRACE("<- %s(This ref=%u)\n", __FUNCTION__, ret);
    return ret;
}

ULONG __stdcall IDirectDrawPalette__Release(IDirectDrawPaletteImpl* This)
{
    TRACE("-> %s(This=%p) [%p]\n", __FUNCTION__, This, _ReturnAddress());

    ULONG ret = --This->ref;

    if (This->ref == 0)
    {
        TRACE("     Released (%p)\n", This);

        if (g_ddraw.ref)
            g_ddraw.last_freed_palette = This;

        HeapFree(GetProcessHeap(), 0, This);
    }

    TRACE("<- %s(This ref=%u)\n", __FUNCTION__, ret);
    return ret;
}

HRESULT __stdcall IDirectDrawPalette__GetCaps(IDirectDrawPaletteImpl* This, LPDWORD lpdwCaps)
{
    TRACE("NOT_IMPLEMENTED -> %s(This=%p, lpdwCaps=%p) [%p]\n", __FUNCTION__, This, lpdwCaps, _ReturnAddress());
    HRESULT ret = DDERR_INVALIDOBJECT;
    TRACE("NOT_IMPLEMENTED <- %s\n", __FUNCTION__);
    return ret;
}

HRESULT __stdcall IDirectDrawPalette__GetEntries(
    IDirectDrawPaletteImpl* This,
    DWORD dwFlags,
    DWORD dwBase,
    DWORD dwNumEntries,
    LPPALETTEENTRY lpEntries)
{
    TRACE(
        "-> %s(This=%p, dwFlags=%08X, dwBase=%u, dwNumEntries=%u, lpEntries=%p) [%p]\n",
        __FUNCTION__,
        This,
        dwFlags,
        dwBase,
        dwNumEntries,
        lpEntries, 
        _ReturnAddress());

    HRESULT ret = ddp_GetEntries(This, dwFlags, dwBase, dwNumEntries, lpEntries);

    TRACE("<- %s\n", __FUNCTION__);
    return ret;
}

HRESULT __stdcall IDirectDrawPalette__Initialize(
    IDirectDrawPaletteImpl* This,
    LPDIRECTDRAW lpDD,
    DWORD dwFlags,
    LPPALETTEENTRY lpDDColorTable)
{
    TRACE("-> %s(This=%p) [%p]\n", __FUNCTION__, This, _ReturnAddress());
    HRESULT ret = DD_OK;
    TRACE("<- %s\n", __FUNCTION__);
    return ret;
}

HRESULT __stdcall IDirectDrawPalette__SetEntries(
    IDirectDrawPaletteImpl* This,
    DWORD dwFlags,
    DWORD dwStartingEntry,
    DWORD dwCount,
    LPPALETTEENTRY lpEntries)
{
    TRACE_EXT(
        "-> %s(This=%p, dwFlags=%08X, dwStartingEntry=%u, dwCount=%u, lpEntries=%p) [%p]\n",
        __FUNCTION__,
        This,
        dwFlags,
        dwStartingEntry,
        dwCount,
        lpEntries, 
        _ReturnAddress());

    HRESULT ret = ddp_SetEntries(This, dwFlags, dwStartingEntry, dwCount, lpEntries);

    TRACE_EXT("<- %s\n", __FUNCTION__);
    return ret;
}

struct IDirectDrawPaletteImplVtbl g_ddp_vtbl =
{
    /*** IUnknown methods ***/
    IDirectDrawPalette__QueryInterface,
    IDirectDrawPalette__AddRef,
    IDirectDrawPalette__Release,
    /*** IDirectDrawPalette methods ***/
    IDirectDrawPalette__GetCaps,
    IDirectDrawPalette__GetEntries,
    IDirectDrawPalette__Initialize,
    IDirectDrawPalette__SetEntries
};
