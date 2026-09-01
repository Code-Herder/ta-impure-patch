#include "IAMMediaStream.h"
#include "debug.h"

HRESULT __stdcall IAMMediaStream__QueryInterface(IAMMediaStreamImpl* This, REFIID riid, void** obj)
{
    TRACE("NOT_IMPLEMENTED -> %s(This=%p, riid=%08X, obj=%p)\n", __FUNCTION__, This, (unsigned int)riid, obj);
    HRESULT ret = E_NOINTERFACE;
    TRACE("NOT_IMPLEMENTED <- %s\n", __FUNCTION__);
    return ret;
}

ULONG __stdcall IAMMediaStream__AddRef(IAMMediaStreamImpl* This)
{
    TRACE("-> %s(This=%p)\n", __FUNCTION__, This);
    ULONG ret = ++This->ref;
    TRACE("<- %s(This ref=%u)\n", __FUNCTION__, ret);
    return ret;
}

ULONG __stdcall IAMMediaStream__Release(IAMMediaStreamImpl* This)
{
    TRACE("-> %s(This=%p)\n", __FUNCTION__, This);

    ULONG ret = --This->ref;

    if (This->ref == 0)
    {
        TRACE("     Released (%p)\n", This);

        HeapFree(GetProcessHeap(), 0, This);
    }

    TRACE("<- %s(This ref=%u)\n", __FUNCTION__, ret);
    return ret;
}

HRESULT WINAPI IAMMediaStream__GetMultiMediaStream(IAMMediaStreamImpl* This, int a)
{
    TRACE("NOT_IMPLEMENTED -> %s(This=%p)\n", __FUNCTION__, This);
    HRESULT ret = E_FAIL;
    TRACE("NOT_IMPLEMENTED <- %s\n", __FUNCTION__);
    return ret;
}

HRESULT WINAPI IAMMediaStream__GetInformation(IAMMediaStreamImpl* This, int a, int b)
{
    TRACE("NOT_IMPLEMENTED -> %s(This=%p)\n", __FUNCTION__, This);
    HRESULT ret = E_FAIL;
    TRACE("NOT_IMPLEMENTED <- %s\n", __FUNCTION__);
    return ret;
}

HRESULT WINAPI IAMMediaStream__SetSameFormat(IAMMediaStreamImpl* This, int a, int b)
{
    TRACE("NOT_IMPLEMENTED -> %s(This=%p)\n", __FUNCTION__, This);
    HRESULT ret = E_FAIL;
    TRACE("NOT_IMPLEMENTED <- %s\n", __FUNCTION__);
    return ret;
}

HRESULT WINAPI IAMMediaStream__AllocateSample(IAMMediaStreamImpl* This, int a, int b)
{
    TRACE("NOT_IMPLEMENTED -> %s(This=%p)\n", __FUNCTION__, This);
    HRESULT ret = E_FAIL;
    TRACE("NOT_IMPLEMENTED <- %s\n", __FUNCTION__);
    return ret;
}

HRESULT WINAPI IAMMediaStream__CreateSharedSample(IAMMediaStreamImpl* This, int a, int b, int c)
{
    TRACE("NOT_IMPLEMENTED -> %s(This=%p)\n", __FUNCTION__, This);
    HRESULT ret = E_FAIL;
    TRACE("NOT_IMPLEMENTED <- %s\n", __FUNCTION__);
    return ret;
}

HRESULT WINAPI IAMMediaStream__SendEndOfStream(IAMMediaStreamImpl* This, int a)
{
    TRACE("NOT_IMPLEMENTED -> %s(This=%p)\n", __FUNCTION__, This);
    HRESULT ret = E_FAIL;
    TRACE("NOT_IMPLEMENTED <- %s\n", __FUNCTION__);
    return ret;
}

// IAMMediaStream
HRESULT WINAPI IAMMediaStream__Initialize(IAMMediaStreamImpl* This, int a, int b, int c, int d)
{
    TRACE("NOT_IMPLEMENTED -> %s(This=%p)\n", __FUNCTION__, This);
    HRESULT ret = E_FAIL;
    TRACE("NOT_IMPLEMENTED <- %s\n", __FUNCTION__);
    return ret;
}

HRESULT WINAPI IAMMediaStream__SetState(IAMMediaStreamImpl* This, int a)
{
    TRACE("NOT_IMPLEMENTED -> %s(This=%p)\n", __FUNCTION__, This);
    HRESULT ret = E_FAIL;
    TRACE("NOT_IMPLEMENTED <- %s\n", __FUNCTION__);
    return ret;
}

HRESULT WINAPI IAMMediaStream__JoinAMMultiMediaStream(IAMMediaStreamImpl* This, int a)
{
    TRACE("NOT_IMPLEMENTED -> %s(This=%p)\n", __FUNCTION__, This);
    HRESULT ret = E_FAIL;
    TRACE("NOT_IMPLEMENTED <- %s\n", __FUNCTION__);
    return ret;
}

HRESULT WINAPI IAMMediaStream__JoinFilter(IAMMediaStreamImpl* This, int a)
{
    TRACE("NOT_IMPLEMENTED -> %s(This=%p)\n", __FUNCTION__, This);
    HRESULT ret = E_FAIL;
    TRACE("NOT_IMPLEMENTED <- %s\n", __FUNCTION__);
    return ret;
}

HRESULT WINAPI IAMMediaStream__JoinFilterGraph(IAMMediaStreamImpl* This, int a)
{
    TRACE("NOT_IMPLEMENTED -> %s(This=%p)\n", __FUNCTION__, This);
    HRESULT ret = E_FAIL;
    TRACE("NOT_IMPLEMENTED <- %s\n", __FUNCTION__);
    return ret;
}

struct IAMMediaStreamImplVtbl g_ms_vtbl =
{
    /* IUnknown */
    IAMMediaStream__QueryInterface,
    IAMMediaStream__AddRef,
    IAMMediaStream__Release,
    /* IMediaStream */
    IAMMediaStream__GetMultiMediaStream,
    IAMMediaStream__GetInformation,
    IAMMediaStream__SetSameFormat,
    IAMMediaStream__AllocateSample,
    IAMMediaStream__CreateSharedSample,
    IAMMediaStream__SendEndOfStream,
    /* IAMMediaStream */
    IAMMediaStream__Initialize,
    IAMMediaStream__SetState,
    IAMMediaStream__JoinAMMultiMediaStream,
    IAMMediaStream__JoinFilter,
    IAMMediaStream__JoinFilterGraph,
};
