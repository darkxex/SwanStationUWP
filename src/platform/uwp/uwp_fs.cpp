/*
 * SwanStation Xbox - the POSIX file calls the frontend relies on, as UWP allows them: dirent on
 * FindFirstFileExFromAppW and an overwriting rename on MoveFileExFromAppW (the *FromApp calls work on the app's own
 * folders and, where the user granted it, on drives).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <windows.h>
#include <stdlib.h>
#include <string.h>

#include "compat/dirent.h"

extern "C" {

/* UTF-8 <-> UTF-16, the frontend's strings being UTF-8 throughout */
static int to_wide(const char *s, wchar_t *out, int cap)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap);
    return n > 0;
}

int uwp_rename(const char *from, const char *to)
{
    wchar_t a[1024], b[1024];
    if (!to_wide(from, a, 1024) || !to_wide(to, b, 1024))
        return -1;
    return MoveFileExFromAppW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) ? 0 : -1;
}

/* fopen with UTF-8 names (the CRT would read them in the ANSI code page: a game called "Pokémon" would not open) */
FILE *uwp_fopen(const char *path, const char *mode)
{
    wchar_t w[1024], m[16];
    if (!to_wide(path, w, 1024) || !to_wide(mode, m, 16))
        return NULL;
    return _wfopen(w, m);
}

struct DIR
{
    HANDLE find;
    WIN32_FIND_DATAW data;
    bool first;
    struct dirent entry;
};

DIR *opendir(const char *path)
{
    wchar_t pattern[1024];
    if (!to_wide(path, pattern, 1000))
        return NULL;
    size_t n = wcslen(pattern);
    if (n && pattern[n - 1] != L'\\' && pattern[n - 1] != L'/')
        pattern[n++] = L'\\';
    pattern[n++] = L'*';
    pattern[n] = 0;
    DIR *d = (DIR *)calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->find = FindFirstFileExFromAppW(pattern, FindExInfoBasic, &d->data, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (d->find == INVALID_HANDLE_VALUE)
    {
        free(d);
        return NULL;
    }
    d->first = true;
    return d;
}

struct dirent *readdir(DIR *d)
{
    if (!d)
        return NULL;
    if (d->first)
        d->first = false;
    else if (!FindNextFileW(d->find, &d->data))
        return NULL;
    WideCharToMultiByte(CP_UTF8, 0, d->data.cFileName, -1, d->entry.d_name, sizeof(d->entry.d_name), NULL, NULL);
    d->entry.d_type = (d->data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? DT_DIR : DT_REG;
    return &d->entry;
}

int closedir(DIR *d)
{
    if (!d)
        return -1;
    FindClose(d->find);
    free(d);
    return 0;
}

} // extern "C"
