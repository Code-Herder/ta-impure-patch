#include "IDirectDrawGammaControl.h"
#include "debug.h"


HRESULT __stdcall IDirectDrawGammaControl__QueryInterface(IDirectDrawGammaControlImpl* This, REFIID riid, LPVOID FAR* ppvObj)
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

ULONG __stdcall IDirectDrawGammaControl__AddRef(IDirectDrawGammaControlImpl* This)
{
    TRACE("-> %s(This=%p) [%p]\n", __FUNCTION__, This, _ReturnAddress());
    ULONG ret = ++This->ref;
    TRACE("<- %s(This ref=%u)\n", __FUNCTION__, ret);
    return ret;
}

ULONG __stdcall IDirectDrawGammaControl__Release(IDirectDrawGammaControlImpl* This)
{
    TRACE("-> %s(This=%p) [%p]\n", __FUNCTION__, This, _ReturnAddress());

    ULONG ret = --This->ref;

    if (This->ref == 0)
    {
        TRACE("     Released (%p)\n", This);

        HeapFree(GetProcessHeap(), 0, This);
    }

    TRACE("<- %s(This ref=%u)\n", __FUNCTION__, ret);
    return ret;
}

HRESULT __stdcall IDirectDrawGammaControl__GetGammaRamp(IDirectDrawGammaControlImpl* This, DWORD dwFlags, LPDDGAMMARAMP lpRampData)
{
    TRACE("NOT_IMPLEMENTED -> %s(This=%p) [%p]\n", __FUNCTION__, This, _ReturnAddress());
    HRESULT ret = DDERR_EXCEPTION;
    TRACE("NOT_IMPLEMENTED <- %s\n", __FUNCTION__);
    return ret;
}

HRESULT __stdcall IDirectDrawGammaControl__SetGammaRamp(IDirectDrawGammaControlImpl* This, DWORD dwFlags, LPDDGAMMARAMP lpRampData)
{
    TRACE("NOT_IMPLEMENTED -> %s(This=%p) [%p]\n", __FUNCTION__, This, _ReturnAddress());
    HRESULT ret = DDERR_EXCEPTION;
    TRACE("NOT_IMPLEMENTED <- %s\n", __FUNCTION__);
    return ret;
}

struct IDirectDrawGammaControlImplVtbl g_ddgc_vtbl =
{
    /*** IUnknown methods ***/
    IDirectDrawGammaControl__QueryInterface,
    IDirectDrawGammaControl__AddRef,
    IDirectDrawGammaControl__Release,
    /*** IDirectDrawGammaControl methods ***/
    IDirectDrawGammaControl__GetGammaRamp,
    IDirectDrawGammaControl__SetGammaRamp,
};
