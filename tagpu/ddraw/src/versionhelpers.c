#include <windows.h>
#include "versionhelpers.h"
#include "delay_imports.h"


BOOL verhelp_verify_version(PRTL_OSVERSIONINFOEXW versionInfo, ULONG typeMask, ULONGLONG conditionMask)
{
    return delay_RtlVerifyVersionInfo ?
        delay_RtlVerifyVersionInfo(versionInfo, typeMask, conditionMask) == 0 :
        VerifyVersionInfoW(versionInfo, typeMask, conditionMask);
}

ULONGLONG verhelp_set_mask(ULONGLONG ConditionMask, DWORD TypeMask, BYTE Condition)
{
    return delay_VerSetConditionMask ? delay_VerSetConditionMask(ConditionMask, TypeMask, Condition) : 0;
}

const char* verhelp_wine_get_version()
{
    return delay_wine_get_version ? delay_wine_get_version() : NULL;
}

void verhelp_wine_get_host_version(const char** sysname, const char** release)
{
    if (delay_wine_get_host_version)
    {
        delay_wine_get_host_version(sysname, release);
        return;
    }

    if (sysname)
        *sysname = NULL;

    if (release)
        *release = NULL;
}
