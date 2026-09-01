#include <windows.h>
#include "versionhelpers.h"
#include "delay_imports.h"

NTQUERYINFORMATIONTHREADPROC delay_NtQueryInformationThread;
RTLVERIFYVERSIONINFOPROC delay_RtlVerifyVersionInfo;
WINE_GET_VERSIONPROC delay_wine_get_version;
WINE_GET_HOST_VERSIONPROC delay_wine_get_host_version;

VERSETCONDITIONMASKPROC delay_VerSetConditionMask;
GETMODULEHANDLEEXAPROC delay_GetModuleHandleExA;

void delay_imports_init()
{
    HMODULE mod = GetModuleHandleA("ntdll.dll");
    if (mod)
    {
        delay_NtQueryInformationThread = (NTQUERYINFORMATIONTHREADPROC)GetProcAddress(mod, "NtQueryInformationThread");
        delay_RtlVerifyVersionInfo = (RTLVERIFYVERSIONINFOPROC)GetProcAddress(mod, "RtlVerifyVersionInfo");
        delay_wine_get_version = (WINE_GET_VERSIONPROC)GetProcAddress(mod, "wine_get_version");
        delay_wine_get_host_version = (WINE_GET_HOST_VERSIONPROC)GetProcAddress(mod, "wine_get_host_version");
    }

    mod = GetModuleHandleA("Kernel32.dll");
    if (mod)
    {
        delay_VerSetConditionMask = (VERSETCONDITIONMASKPROC)GetProcAddress(mod, "VerSetConditionMask");
        delay_GetModuleHandleExA = (GETMODULEHANDLEEXAPROC)GetProcAddress(mod, "GetModuleHandleExA");
    }
}
