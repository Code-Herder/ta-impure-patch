/**
 * This file is part of the mingw-w64 runtime package.
 * No warranty is given; refer to the file DISCLAIMER within this package.
 */

#ifndef _INC_VERSIONHELPERS
#define _INC_VERSIONHELPERS

#ifdef __cplusplus
#define VERSIONHELPERAPI inline bool
#else
#define VERSIONHELPERAPI FORCEINLINE BOOL
#endif

#ifndef _WIN32_WINNT_WIN8
#define _WIN32_WINNT_WIN8                   0x0602
#endif
#ifndef _WIN32_WINNT_WINBLUE
#define _WIN32_WINNT_WINBLUE                0x0603
#endif
#ifndef _WIN32_WINNT_WINTHRESHOLD
#define _WIN32_WINNT_WINTHRESHOLD           0x0A00 /* ABRACADABRA_THRESHOLD*/
#endif
#ifndef _WIN32_WINNT_WIN10
#define _WIN32_WINNT_WIN10                  0x0A00 /* ABRACADABRA_THRESHOLD*/
#endif
#ifndef _WIN32_WINNT_WIN11
#define _WIN32_WINNT_WIN11                  0x0A00
#endif

#if (_WIN32_WINNT < _WIN32_WINNT_WIN2K)
#define VerifyVersionInfoW(a,b,c) 0 
#define VerSetConditionMask verhelp_set_mask
#endif

BOOL verhelp_verify_version(PRTL_OSVERSIONINFOEXW versionInfo, ULONG typeMask, ULONGLONG conditionMask);
ULONGLONG verhelp_set_mask(ULONGLONG ConditionMask, DWORD TypeMask, BYTE Condition);
const char* verhelp_wine_get_version();
void verhelp_wine_get_host_version(const char** sysname, const char** release);

VERSIONHELPERAPI IsWindowsVersionOrGreater(DWORD major, DWORD minor, DWORD build, WORD servpack)
{
    RTL_OSVERSIONINFOEXW vi = { sizeof(vi),major,minor,build,0,{0},servpack };
    return verhelp_verify_version(&vi, VER_MAJORVERSION | VER_MINORVERSION | VER_BUILDNUMBER | VER_SERVICEPACKMAJOR,
        VerSetConditionMask(VerSetConditionMask(VerSetConditionMask(VerSetConditionMask(0,
            VER_MAJORVERSION, VER_GREATER_EQUAL),
            VER_MINORVERSION, VER_GREATER_EQUAL),
            VER_BUILDNUMBER, VER_GREATER_EQUAL),
            VER_SERVICEPACKMAJOR, VER_GREATER_EQUAL));
}

VERSIONHELPERAPI IsWindowsVersion(DWORD major, DWORD minor, DWORD build, WORD servpack)
{
    RTL_OSVERSIONINFOEXW vi = { sizeof(vi),major,minor,build,0,{0},servpack };
    return verhelp_verify_version(&vi, VER_MAJORVERSION | VER_MINORVERSION | VER_BUILDNUMBER | VER_SERVICEPACKMAJOR,
        VerSetConditionMask(VerSetConditionMask(VerSetConditionMask(VerSetConditionMask(0,
            VER_MAJORVERSION, VER_EQUAL),
            VER_MINORVERSION, VER_EQUAL),
            VER_BUILDNUMBER, VER_GREATER_EQUAL),
            VER_SERVICEPACKMAJOR, VER_EQUAL));
}

VERSIONHELPERAPI IsWindowsVersionExcactBuild(DWORD major, DWORD minor, DWORD build, WORD servpack)
{
    RTL_OSVERSIONINFOEXW vi = { sizeof(vi),major,minor,build,0,{0},servpack };
    return verhelp_verify_version(&vi, VER_MAJORVERSION | VER_MINORVERSION | VER_BUILDNUMBER | VER_SERVICEPACKMAJOR,
        VerSetConditionMask(VerSetConditionMask(VerSetConditionMask(VerSetConditionMask(0,
            VER_MAJORVERSION, VER_EQUAL),
            VER_MINORVERSION, VER_EQUAL),
            VER_BUILDNUMBER, VER_EQUAL),
            VER_SERVICEPACKMAJOR, VER_EQUAL));
}

VERSIONHELPERAPI IsWindowsVersionAnySP(DWORD major, DWORD minor, DWORD build)
{
    RTL_OSVERSIONINFOEXW vi = { sizeof(vi),major,minor,build,0,{0},0 };
    return verhelp_verify_version(&vi, VER_MAJORVERSION | VER_MINORVERSION | VER_BUILDNUMBER | VER_SERVICEPACKMAJOR,
        VerSetConditionMask(VerSetConditionMask(VerSetConditionMask(VerSetConditionMask(0,
            VER_MAJORVERSION, VER_EQUAL),
            VER_MINORVERSION, VER_EQUAL),
            VER_BUILDNUMBER, VER_GREATER_EQUAL),
            VER_SERVICEPACKMAJOR, VER_GREATER_EQUAL));
}

VERSIONHELPERAPI IsWindows2000OrGreater(void) {
    return IsWindowsVersionOrGreater(HIBYTE(_WIN32_WINNT_WIN2K), LOBYTE(_WIN32_WINNT_WIN2K), 0, 0);
}

VERSIONHELPERAPI IsWindowsXPOrGreater(void) {
    return IsWindowsVersionOrGreater(HIBYTE(_WIN32_WINNT_WINXP), LOBYTE(_WIN32_WINNT_WINXP), 0, 0);
}

VERSIONHELPERAPI IsWindowsXPSP1OrGreater(void) {
    return IsWindowsVersionOrGreater(HIBYTE(_WIN32_WINNT_WINXP), LOBYTE(_WIN32_WINNT_WINXP), 0, 1);
}

VERSIONHELPERAPI IsWindowsXPSP2OrGreater(void) {
    return IsWindowsVersionOrGreater(HIBYTE(_WIN32_WINNT_WINXP), LOBYTE(_WIN32_WINNT_WINXP), 0, 2);
}

VERSIONHELPERAPI IsWindowsXPSP3OrGreater(void) {
    return IsWindowsVersionOrGreater(HIBYTE(_WIN32_WINNT_WINXP), LOBYTE(_WIN32_WINNT_WINXP), 0, 3);
}

VERSIONHELPERAPI IsWindowsVistaOrGreater(void) {
    return IsWindowsVersionOrGreater(HIBYTE(_WIN32_WINNT_VISTA), LOBYTE(_WIN32_WINNT_VISTA), 0, 0);
}

VERSIONHELPERAPI IsWindowsVistaSP1OrGreater(void) {
    return IsWindowsVersionOrGreater(HIBYTE(_WIN32_WINNT_VISTA), LOBYTE(_WIN32_WINNT_VISTA), 0, 1);
}

VERSIONHELPERAPI IsWindowsVistaSP2OrGreater(void) {
    return IsWindowsVersionOrGreater(HIBYTE(_WIN32_WINNT_VISTA), LOBYTE(_WIN32_WINNT_VISTA), 0, 2);
}

VERSIONHELPERAPI IsWindows7OrGreater(void) {
    return IsWindowsVersionOrGreater(HIBYTE(_WIN32_WINNT_WIN7), LOBYTE(_WIN32_WINNT_WIN7), 0, 0);
}

VERSIONHELPERAPI IsWindows7SP1OrGreater(void) {
    return IsWindowsVersionOrGreater(HIBYTE(_WIN32_WINNT_WIN7), LOBYTE(_WIN32_WINNT_WIN7), 0, 1);
}

VERSIONHELPERAPI IsWindows8OrGreater(void) {
    return IsWindowsVersionOrGreater(HIBYTE(_WIN32_WINNT_WIN8), LOBYTE(_WIN32_WINNT_WIN8), 0, 0);
}

VERSIONHELPERAPI IsWindows8Point1OrGreater(void) {
    return IsWindowsVersionOrGreater(HIBYTE(_WIN32_WINNT_WINBLUE), LOBYTE(_WIN32_WINNT_WINBLUE), 0, 0);
}

VERSIONHELPERAPI IsWindowsThresholdOrGreater(void) {
    return IsWindowsVersionOrGreater(HIBYTE(_WIN32_WINNT_WINTHRESHOLD), LOBYTE(_WIN32_WINNT_WINTHRESHOLD), 0, 0);
}

VERSIONHELPERAPI IsWindows10OrGreater(void) {
    return IsWindowsThresholdOrGreater();
}

VERSIONHELPERAPI IsWindows10Version1803OrGreater(void) {
    return IsWindowsVersionOrGreater(HIBYTE(_WIN32_WINNT_WIN10), LOBYTE(_WIN32_WINNT_WIN10), 17134, 0);
}

VERSIONHELPERAPI IsWindows11OrGreater(void) {
    return IsWindowsVersionOrGreater(HIBYTE(_WIN32_WINNT_WIN11), LOBYTE(_WIN32_WINNT_WIN11), 22000, 0);
}

VERSIONHELPERAPI IsWindows11Version24H2OrGreater(void) {
    return IsWindowsVersionOrGreater(HIBYTE(_WIN32_WINNT_WIN11), LOBYTE(_WIN32_WINNT_WIN11), 26100, 0);
}

VERSIONHELPERAPI IsWindowsServer(void) {
    OSVERSIONINFOEXW vi = {sizeof(vi),0,0,0,0,{0},0,0,0,VER_NT_WORKSTATION};
    return !verhelp_verify_version(&vi, VER_PRODUCT_TYPE, VerSetConditionMask(0, VER_PRODUCT_TYPE, VER_EQUAL));
}

VERSIONHELPERAPI IsWindows2000(void) {
    return IsWindowsVersionAnySP(HIBYTE(_WIN32_WINNT_WIN2K), LOBYTE(_WIN32_WINNT_WIN2K), 0);
}

VERSIONHELPERAPI IsWindowsXP(void) {
    return IsWindowsVersionAnySP(HIBYTE(_WIN32_WINNT_WINXP), LOBYTE(_WIN32_WINNT_WINXP), 0);
}

VERSIONHELPERAPI IsWindowsXPRTM(void) {
    return IsWindowsVersion(HIBYTE(_WIN32_WINNT_WINXP), LOBYTE(_WIN32_WINNT_WINXP), 0, 0);
}

VERSIONHELPERAPI IsWindowsXPSP1(void) {
    return IsWindowsVersion(HIBYTE(_WIN32_WINNT_WINXP), LOBYTE(_WIN32_WINNT_WINXP), 0, 1);
}

VERSIONHELPERAPI IsWindowsXPSP2(void) {
    return IsWindowsVersion(HIBYTE(_WIN32_WINNT_WINXP), LOBYTE(_WIN32_WINNT_WINXP), 0, 2);
}

VERSIONHELPERAPI IsWindowsXPSP3(void) {
    return IsWindowsVersion(HIBYTE(_WIN32_WINNT_WINXP), LOBYTE(_WIN32_WINNT_WINXP), 0, 3);
}

VERSIONHELPERAPI IsWindowsVista(void) {
    return IsWindowsVersionAnySP(HIBYTE(_WIN32_WINNT_VISTA), LOBYTE(_WIN32_WINNT_VISTA), 0);
}

VERSIONHELPERAPI IsWindowsVistaRTM(void) {
    return IsWindowsVersion(HIBYTE(_WIN32_WINNT_VISTA), LOBYTE(_WIN32_WINNT_VISTA), 0, 0);
}

VERSIONHELPERAPI IsWindowsVistaSP1(void) {
    return IsWindowsVersion(HIBYTE(_WIN32_WINNT_VISTA), LOBYTE(_WIN32_WINNT_VISTA), 0, 1);
}

VERSIONHELPERAPI IsWindowsVistaSP2(void) {
    return IsWindowsVersion(HIBYTE(_WIN32_WINNT_VISTA), LOBYTE(_WIN32_WINNT_VISTA), 0, 2);
}

VERSIONHELPERAPI IsWindows7(void) {
    return IsWindowsVersionAnySP(HIBYTE(_WIN32_WINNT_WIN7), LOBYTE(_WIN32_WINNT_WIN7), 0);
}

VERSIONHELPERAPI IsWindows7RTM(void) {
    return IsWindowsVersion(HIBYTE(_WIN32_WINNT_WIN7), LOBYTE(_WIN32_WINNT_WIN7), 0, 0);
}

VERSIONHELPERAPI IsWindows7SP1(void) {
    return IsWindowsVersion(HIBYTE(_WIN32_WINNT_WIN7), LOBYTE(_WIN32_WINNT_WIN7), 0, 1);
}

VERSIONHELPERAPI IsWindows8(void) {
    return IsWindowsVersion(HIBYTE(_WIN32_WINNT_WIN8), LOBYTE(_WIN32_WINNT_WIN8), 0, 0);
}

VERSIONHELPERAPI IsWindows8Point1(void) {
    return IsWindowsVersion(HIBYTE(_WIN32_WINNT_WINBLUE), LOBYTE(_WIN32_WINNT_WINBLUE), 0, 0);
}

VERSIONHELPERAPI IsWindowsThreshold(void) {
    return IsWindows10OrGreater() && !IsWindows11OrGreater();
}

VERSIONHELPERAPI IsWindows10(void) {
    return IsWindowsThreshold();
}

VERSIONHELPERAPI IsWindows11(void) {
    return IsWindowsVersion(HIBYTE(_WIN32_WINNT_WIN11), LOBYTE(_WIN32_WINNT_WIN11), 22000, 0);
}

VERSIONHELPERAPI IsWindows11Version24H2(void) {
    return IsWindowsVersionExcactBuild(HIBYTE(_WIN32_WINNT_WIN11), LOBYTE(_WIN32_WINNT_WIN11), 26100, 0);
}

VERSIONHELPERAPI IsWine(void) {
    return verhelp_wine_get_version() != NULL;
}

VERSIONHELPERAPI IsMacOS(void) {
    const char* sysname = NULL;
    const char* release = NULL;
    verhelp_wine_get_host_version(&sysname, &release);

    return sysname && _strcmpi(sysname, "Darwin") == 0;
}

VERSIONHELPERAPI IsLinux(void) {
    const char* sysname = NULL;
    const char* release = NULL;
    verhelp_wine_get_host_version(&sysname, &release);

    return sysname && _strcmpi(sysname, "Linux") == 0;
}

VERSIONHELPERAPI IsAndroid(void) {
    const char* sysname = NULL;
    const char* release = NULL;
    verhelp_wine_get_host_version(&sysname, &release);

    return release && strstr(release, "android") != NULL;
}

VERSIONHELPERAPI IsSteamDeck(void) {
    return IsWine() && GetEnvironmentVariable("STEAMDECK", NULL, 0);
}

#endif
