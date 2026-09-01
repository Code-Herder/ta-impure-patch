#ifndef IAMMEDIASTREAM_H 
#define IAMMEDIASTREAM_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>


DEFINE_GUID(IID_IMediaStream, 0xb502d1bd, 0x9a57, 0x11d0, 0x8f, 0xde, 0x00, 0xc0, 0x4f, 0xd9, 0x18, 0x9d);
DEFINE_GUID(IID_IAMMediaStream, 0xbebe595d, 0x9a6f, 0x11d0, 0x8f, 0xde, 0x00, 0xc0, 0x4f, 0xd9, 0x18, 0x9d);

extern struct IAMMediaStreamImplVtbl g_ms_vtbl;

struct IAMMediaStreamImpl;
struct IAMMediaStreamImplVtbl;

typedef struct IAMMediaStreamImpl
{
    struct IAMMediaStreamImplVtbl* lpVtbl;

    ULONG ref;

} IAMMediaStreamImpl;

typedef struct IAMMediaStreamImplVtbl IAMMediaStreamImplVtbl;

struct IAMMediaStreamImplVtbl
{
    HRESULT(__stdcall* QueryInterface) (IAMMediaStreamImpl*, const IID* const riid, LPVOID* ppvObj);
    ULONG(__stdcall* AddRef) (IAMMediaStreamImpl*);
    ULONG(__stdcall* Release) (IAMMediaStreamImpl*);
    /* IMediaStream */
    HRESULT(__stdcall* GetMultiMediaStream)(IAMMediaStreamImpl* This, int a);
    HRESULT(__stdcall* GetInformation)(IAMMediaStreamImpl* This, int a, int b);
    HRESULT(__stdcall* SetSameFormat)(IAMMediaStreamImpl* This, int a, int b);
    HRESULT(__stdcall* AllocateSample)(IAMMediaStreamImpl* This, int a, int b);
    HRESULT(__stdcall* CreateSharedSample)(IAMMediaStreamImpl* This, int a, int b, int c);
    HRESULT(__stdcall* SendEndOfStream)(IAMMediaStreamImpl* This, int a);
    /* IAMMediaStream */
    HRESULT(__stdcall* Initialize)(IAMMediaStreamImpl* This, int a, int b, int c, int d);
    HRESULT(__stdcall* SetState)(IAMMediaStreamImpl* This, int a);
    HRESULT(__stdcall* JoinAMMultiMediaStream)(IAMMediaStreamImpl* This, int a);
    HRESULT(__stdcall* JoinFilter)(IAMMediaStreamImpl* This, int a);
    HRESULT(__stdcall* JoinFilterGraph)(IAMMediaStreamImpl* This, int a);
};

#endif
