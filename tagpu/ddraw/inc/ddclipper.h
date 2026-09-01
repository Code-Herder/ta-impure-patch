#ifndef DDCLIPPER_H
#define DDCLIPPER_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "ddraw.h"
#include "IDirectDrawClipper.h"


HRESULT ddc_GetClipList(IDirectDrawClipperImpl* This, LPRECT lpRect, LPRGNDATA lpClipList, LPDWORD lpdwSiz);
HRESULT ddc_GetHWnd(IDirectDrawClipperImpl* This, HWND FAR* lphWnd);
HRESULT ddc_IsClipListChanged(IDirectDrawClipperImpl* This, BOOL FAR* lpbChanged);
HRESULT ddc_SetClipList(IDirectDrawClipperImpl* This, LPRGNDATA lpClipList, DWORD dwFlags);
HRESULT ddc_SetHWnd(IDirectDrawClipperImpl* This, DWORD dwFlags, HWND hWnd);
HRESULT ddc_SetClipRect(IDirectDrawClipperImpl* This, LPRECT lpRect);
HRESULT dd_CreateClipper(DWORD dwFlags, IDirectDrawClipperImpl** lplpDDClipper, IUnknown FAR* pUnkOuter);

#endif
