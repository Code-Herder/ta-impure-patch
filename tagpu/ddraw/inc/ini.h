#ifndef INI_H
#define INI_H

typedef struct
{
    char filename[MAX_PATH];

    struct {
        unsigned long hash;
        char* data;
    }*sections;
} INIFILE;

void ini_create(INIFILE* ini, char* filename);
BOOL ini_section_exists(INIFILE* ini, LPCSTR section);
DWORD ini_get_string(INIFILE* ini, LPCSTR section, LPCSTR key, LPCSTR def, LPSTR buf, DWORD size);
BOOL ini_get_bool(INIFILE* ini, LPCSTR section, LPCSTR key, BOOL def);
int ini_get_int(INIFILE* ini, LPCSTR section, LPCSTR key, int def);
float ini_get_float(INIFILE* ini, LPCSTR section, LPCSTR key, float def);
void ini_free(INIFILE* ini);

#endif
