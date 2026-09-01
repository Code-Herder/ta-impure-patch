#ifndef SCREENSHOT_H
#define SCREENSHOT_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "ddsurface.h"

BOOL ss_take_screenshot(struct IDirectDrawSurfaceImpl* src);

#endif
