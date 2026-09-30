/* tagpu_addr.c -- the top of this process's address space (tagpu_addr.h). */
#include <windows.h>
#include "tagpu_addr.h"

size_t tagpu_user_top_bound;

void tagpu_addr_init(void)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    tagpu_user_top_bound = (size_t)si.lpMaximumApplicationAddress;
}
