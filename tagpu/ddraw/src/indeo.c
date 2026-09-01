#include <windows.h>


void indeo_enable()
{
    HKEY hkey;
    LONG status =
        RegCreateKeyExA(
            HKEY_CURRENT_USER,
            "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Drivers32",
            0,
            NULL,
            REG_OPTION_NON_VOLATILE,
            KEY_WRITE | KEY_QUERY_VALUE,
            NULL,
            &hkey,
            NULL);

    if (status == ERROR_SUCCESS)
    {
        LPCTSTR iv31 = "ir32_32.dll";
        RegSetValueExA(hkey, "vidc.iv31", 0, REG_SZ, (const BYTE*)iv31, strlen(iv31) + 1);

        LPCTSTR iv32 = "ir32_32.dll";
        RegSetValueExA(hkey, "vidc.iv32", 0, REG_SZ, (const BYTE*)iv32, strlen(iv32) + 1);

        LPCTSTR iv41 = "ir41_32.ax";
        RegSetValueExA(hkey, "vidc.iv41", 0, REG_SZ, (const BYTE*)iv41, strlen(iv41) + 1);

        LPCTSTR iv50 = "ir50_32.dll";
        RegSetValueExA(hkey, "vidc.iv50", 0, REG_SZ, (const BYTE*)iv50, strlen(iv50) + 1);

        RegCloseKey(hkey);
    }
}

void indeo_disable()
{
    HKEY hkey;
    LONG status =
        RegCreateKeyExA(
            HKEY_CURRENT_USER,
            "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Drivers32",
            0,
            NULL,
            REG_OPTION_NON_VOLATILE,
            KEY_WRITE | KEY_QUERY_VALUE,
            NULL,
            &hkey,
            NULL);

    if (status == ERROR_SUCCESS)
    {
        RegDeleteValueA(hkey, "vidc.iv31");
        RegDeleteValueA(hkey, "vidc.iv32");
        RegDeleteValueA(hkey, "vidc.iv41");
        RegDeleteValueA(hkey, "vidc.iv50");

        RegCloseKey(hkey);
    }
}
