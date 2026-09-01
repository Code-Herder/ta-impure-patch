#include <windows.h>
#include <stdio.h>
#include "debug.h"
#include "config.h"
#include "crc32.h"
#include "ini.h"

// Microsoft: The maximum profile section size is 32,767 characters.
#define BUF_SIZE (8192)

void ini_create(INIFILE* ini, char* filename)
{
    if (!ini || !filename || !filename[0])
        return;

    ini->sections = calloc(sizeof(ini->sections[0]), 1);
    if (ini->sections)
    {
        strncpy(ini->filename, filename, sizeof(ini->filename) - 1);
        ini->filename[sizeof(ini->filename) - 1] = 0;

        char* names = calloc(BUF_SIZE, 1);
        if (names)
        {
            if (GetPrivateProfileSectionNamesA(names, BUF_SIZE, filename) > 0)
            {
                char* name = names;

                for (int i = 0; *name; i++)
                {
                    ini->sections = realloc(ini->sections, sizeof(ini->sections[0]) * (i + 2));

                    if (!ini->sections)
                        return;

                    memset(&ini->sections[i + 1], 0, sizeof(ini->sections[0]));

                    char* buf = malloc(BUF_SIZE);
                    if (buf)
                    {
                        DWORD size = GetPrivateProfileSectionA(name, buf, BUF_SIZE, filename);
                        if (size > 0)
                        {
                            ini->sections[i].data = malloc(size + 2);
                            if (ini->sections[i].data)
                            {
                                memcpy(ini->sections[i].data, buf, size + 2);
                            }
                        }

                        free(buf);
                    }

                    size_t len = strlen(name);

                    for (char* p = name; *p; ++p)
                        *p = tolower(*p);

                    ini->sections[i].hash = Crc32_ComputeBuf(0, name, len);

                    name += len + 1;
                }
            }

            free(names);
        }
    }
}

BOOL ini_section_exists(INIFILE* ini, LPCSTR section)
{
    if (!ini || !ini->sections || !section || strlen(section) == 0)
    {
        return FALSE;
    }

    char s[MAX_PATH];
    strncpy(s, section, sizeof(s) - 1);
    s[sizeof(s) - 1] = 0;

    for (char* p = s; *p; ++p)
        *p = tolower(*p);

    unsigned long hash = Crc32_ComputeBuf(0, s, strlen(s));

    for (int i = 0; ini->sections[i].hash; i++)
    {
        if (ini->sections[i].hash == hash)
        {
            return TRUE;
        }
    }

    return FALSE;
}

DWORD ini_get_string(INIFILE* ini, LPCSTR section, LPCSTR key, LPCSTR def, LPSTR buf, DWORD size)
{
    if (!buf || size == 0)
    {
        return 0;
    }  

    if (!ini || !ini->sections || !section || !key)
    {
        goto end;
    }

    size_t key_len = strlen(key);

    if (key_len == 0 || strlen(section) == 0)
    {
        goto end;
    }

    char s[MAX_PATH];
    strncpy(s, section, sizeof(s) - 1);
    s[sizeof(s) - 1] = 0;

    for (char* p = s; *p; ++p)
        *p = tolower(*p);
    
    unsigned long hash = Crc32_ComputeBuf(0, s, strlen(s));
  
    for (int i = 0; ini->sections[i].hash; i++)
    {
        if (ini->sections[i].hash == hash)
        {
            if (!ini->sections[i].data)
                break;

            for (char* p = ini->sections[i].data; *p; p += strlen(p) + 1)
            {
                if (_strnicmp(key, p, key_len) == 0 && p[key_len] == '=')
                {
                    strncpy(buf, &p[key_len + 1], size - 1);
                    buf[size - 1] = 0;
                    return strlen(buf);
                }
            }

            break;
        }
    }

end:
    if (def)
    {
        strncpy(buf, def, size - 1);
        buf[size - 1] = 0;
        return strlen(buf);
    }
    
    buf[0] = 0;

    return 0;
}

BOOL ini_get_bool(INIFILE* ini, LPCSTR section, LPCSTR key, BOOL def)
{
    char value[8];
    ini_get_string(ini, section, key, def ? "Yes" : "No", value, sizeof(value));

    return (_stricmp(value, "yes") == 0 || _stricmp(value, "true") == 0 || _stricmp(value, "1") == 0);
}

int ini_get_int(INIFILE* ini, LPCSTR section, LPCSTR key, int def)
{
    char def_str[32];
    _snprintf(def_str, sizeof(def_str) - 1, "%d", def);

    char value[32];
    ini_get_string(ini, section, key, def_str, value, sizeof(value));

    if (strstr(value, "0x"))
    {
        return strtol(value, NULL, 0);
    }
    else
    {
        return atoi(value);
    }
}

float ini_get_float(INIFILE* ini, LPCSTR section, LPCSTR key, float def)
{
    char def_str[32];
    _snprintf(def_str, sizeof(def_str) - 1, "%f", def);

    char value[32];
    ini_get_string(ini, section, key, def_str, value, sizeof(value));

    return (float)atof(value);
}

void ini_free(INIFILE* ini)
{
    if (!ini)
        return;

    ini->filename[0] = 0;

    if (ini->sections)
    {
        for (int i = 0; ini->sections[i].hash; i++)
        {
            if (ini->sections[i].data)
            {
                free(ini->sections[i].data);
                ini->sections[i].data = NULL;
            }  
        }

        free(ini->sections);
        ini->sections = NULL;
    }
}
