#include "IDirect3D.h"
#include "debug.h"


HRESULT __stdcall IDirect3D3__QueryInterface(IDirect3D3Impl* This, REFIID riid, void** obj)
{
    TRACE(
        "NOT_IMPLEMENTED -> %s(This=%p, riid=%08X, obj=%p) [%p]\n", 
        __FUNCTION__, 
        This, 
        (unsigned int)riid, 
        obj, 
        _ReturnAddress());

    HRESULT ret = E_FAIL;

    if (riid)
    {
        TRACE("NOT_IMPLEMENTED     GUID = %08X\n", ((GUID*)riid)->Data1);
    }

    TRACE("NOT_IMPLEMENTED <- %s\n", __FUNCTION__);
    return ret;
}

ULONG __stdcall IDirect3D3__AddRef(IDirect3D3Impl* This)
{
    TRACE("-> %s(This=%p) [%p]\n", __FUNCTION__, This, _ReturnAddress());
    ULONG ret = ++This->ref;
    TRACE("<- %s(This ref=%u)\n", __FUNCTION__, ret);
    return ret;
}

ULONG __stdcall IDirect3D3__Release(IDirect3D3Impl* This)
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

HRESULT __stdcall IDirect3D3__EnumDevices(
    IDirect3D3Impl* This,
    LPD3DENUMDEVICESCALLBACK lpEnumDevicesCallback,
    LPVOID lpUserArg)
{
    TRACE("NOT_IMPLEMENTED -> %s(This=%p) [%p]\n", __FUNCTION__, This, _ReturnAddress());
    HRESULT ret = S_OK;

    if (lpEnumDevicesCallback)
    {
        //D3DDEVICEDESC desc = { 0 };
        //lpEnumDevicesCallback((GUID FAR*)&GUID_NULL, "NULL", "NULL", &desc, &desc, lpUserArg);
    }

    TRACE("NOT_IMPLEMENTED <- %s\n", __FUNCTION__);
    return ret;
}

HRESULT __stdcall IDirect3D3__CreateLight(IDirect3D3Impl* This, int a, int b)
{
    TRACE("NOT_IMPLEMENTED -> %s(This=%p) [%p]\n", __FUNCTION__, This, _ReturnAddress());
    HRESULT ret = E_FAIL;
    TRACE("NOT_IMPLEMENTED <- %s\n", __FUNCTION__);
    return ret;
}

HRESULT __stdcall IDirect3D3__CreateMaterial(IDirect3D3Impl* This, int a, int b)
{
    TRACE("NOT_IMPLEMENTED -> %s(This=%p) [%p]\n", __FUNCTION__, This, _ReturnAddress());
    HRESULT ret = E_FAIL;
    TRACE("NOT_IMPLEMENTED <- %s\n", __FUNCTION__);
    return ret;
}

HRESULT __stdcall IDirect3D3__CreateViewport(IDirect3D3Impl* This, int a, int b)
{
    TRACE("NOT_IMPLEMENTED -> %s(This=%p) [%p]\n", __FUNCTION__, This, _ReturnAddress());
    HRESULT ret = E_FAIL;
    TRACE("NOT_IMPLEMENTED <- %s\n", __FUNCTION__);
    return ret;
}

HRESULT __stdcall IDirect3D3__FindDevice(IDirect3D3Impl* This, int a, int b)
{
    TRACE("NOT_IMPLEMENTED -> %s(This=%p) [%p]\n", __FUNCTION__, This, _ReturnAddress());
    HRESULT ret = E_FAIL;
    TRACE("NOT_IMPLEMENTED <- %s\n", __FUNCTION__);
    return ret;
}

HRESULT __stdcall IDirect3D3__CreateDevice(IDirect3D3Impl* This, int a, int b, int c, int d)
{
    TRACE("NOT_IMPLEMENTED -> %s(This=%p) [%p]\n", __FUNCTION__, This, _ReturnAddress());
    HRESULT ret = E_FAIL;
    TRACE("NOT_IMPLEMENTED <- %s\n", __FUNCTION__);
    return ret;
}

HRESULT __stdcall IDirect3D3__CreateVertexBuffer(IDirect3D3Impl* This, int a, int b, int c, int d)
{
    TRACE("NOT_IMPLEMENTED -> %s(This=%p) [%p]\n", __FUNCTION__, This, _ReturnAddress());
    HRESULT ret = E_FAIL;
    TRACE("NOT_IMPLEMENTED <- %s\n", __FUNCTION__);
    return ret;
}

HRESULT __stdcall IDirect3D3__EnumZBufferFormats(IDirect3D3Impl* This, int a, int b, int c)
{
    TRACE("NOT_IMPLEMENTED -> %s(This=%p) [%p]\n", __FUNCTION__, This, _ReturnAddress());
    HRESULT ret = E_FAIL;
    TRACE("NOT_IMPLEMENTED <- %s\n", __FUNCTION__);
    return ret;
}

HRESULT __stdcall IDirect3D3__EvictManagedTextures(IDirect3D3Impl* This)
{
    TRACE("NOT_IMPLEMENTED -> %s(This=%p) [%p]\n", __FUNCTION__, This, _ReturnAddress());
    HRESULT ret = E_FAIL;
    TRACE("NOT_IMPLEMENTED <- %s\n", __FUNCTION__);
    return ret;
}

struct IDirect3D3ImplVtbl g_d3d3_vtbl =
{
    /* IUnknown */
    IDirect3D3__QueryInterface,
    IDirect3D3__AddRef,
    IDirect3D3__Release,
    /* IDirect3D3Impl */
    IDirect3D3__EnumDevices,
    IDirect3D3__CreateLight,
    IDirect3D3__CreateMaterial,
    IDirect3D3__CreateViewport,
    IDirect3D3__FindDevice,
    IDirect3D3__CreateDevice,
    IDirect3D3__CreateVertexBuffer,
    IDirect3D3__EnumZBufferFormats,
    IDirect3D3__EvictManagedTextures,
};
