/*
 * SwanStationUWP - the POSIX file calls the frontend relies on, as UWP allows them: dirent on
 * FindFirstFileExFromAppW and an overwriting rename on ReplaceFileFromAppW (the *FromApp calls work on the app's own
 * folders and, where the user granted it, on drives).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <windows.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
    // There is no MoveFileExFromApp: a target that exists is replaced by ReplaceFile (atomic), else a plain move
    WIN32_FILE_ATTRIBUTE_DATA attr;
    if (GetFileAttributesExFromAppW(b, GetFileExInfoStandard, &attr))
    {
        if (ReplaceFileFromAppW(b, a, NULL, 0, NULL, NULL))
            return 0;
        // not replaceable (another volume, a folder): delete the old one, then move
        if (!DeleteFileFromAppW(b))
            return -1;
    }
    return MoveFileFromAppW(a, b) ? 0 : -1;
}

static unsigned file_error;
unsigned uwp_last_error(void) { return file_error; }

/* mkdir: one folder, on CreateDirectoryFromApp. "Already there" is EEXIST, as the CRT's. */
int uwp_mkdir(const char *path)
{
    wchar_t w[1024];
    if (!to_wide(path, w, 1024))
    {
        errno = EINVAL;
        return -1;
    }
    if (CreateDirectoryFromAppW(w, NULL))
        return 0;
    file_error = GetLastError();
    errno = file_error == ERROR_ALREADY_EXISTS ? EEXIST
            : file_error == ERROR_PATH_NOT_FOUND ? ENOENT
            : file_error == ERROR_ACCESS_DENIED ? EACCES
            : EIO;
    return -1;
}

/* fopen with UTF-8 names (the CRT would read them in the ANSI code page: a game called "Pokémon" would not open). The
 * CRT only gets at the app's own folders; a file on a drive the user allowed (Settings > Privacy > File system) is
 * opened with CreateFile2FromApp and handed to the CRT as a descriptor. */
FILE *uwp_fopen(const char *path, const char *mode)
{
    wchar_t w[1024], m[16];
    if (!to_wide(path, w, 1024) || !to_wide(mode, m, 16))
        return NULL;
    FILE *f = _wfopen(w, m);
    if (f)
        return f;
    const bool plus = strchr(mode, '+') != NULL;
    DWORD access, share = FILE_SHARE_READ, disposition;
    int flags = 0;
    switch (mode[0])
    {
    case 'r':
        access = GENERIC_READ | (plus ? GENERIC_WRITE : 0);
        disposition = OPEN_EXISTING;
        flags = plus ? _O_RDWR : _O_RDONLY;
        break;
    case 'w':
        access = GENERIC_WRITE | (plus ? GENERIC_READ : 0);
        disposition = CREATE_ALWAYS;
        flags = plus ? _O_RDWR : _O_WRONLY;
        break;
    case 'a':
        access = FILE_APPEND_DATA | (plus ? GENERIC_READ : 0);
        disposition = OPEN_ALWAYS;
        flags = (plus ? _O_RDWR : _O_WRONLY) | _O_APPEND;
        break;
    default:
        return NULL;
    }
    if (strchr(mode, 'b'))
        flags |= _O_BINARY;
    HANDLE h = CreateFile2FromAppW(w, access, share, disposition, NULL);
    if (h == INVALID_HANDLE_VALUE)
    {
        file_error = GetLastError();
        return NULL;
    }
    int fd = _open_osfhandle((intptr_t)h, flags);
    if (fd < 0)
    {
        CloseHandle(h);
        return NULL;
    }
    f = _wfdopen(fd, m);
    if (!f)
        _close(fd);
    return f;
}

/* stat on GetFileAttributesExFromApp: a folder or a file, its size and time; nothing else is filled in */
int uwp_stat(const char *path, struct stat *st)
{
    wchar_t w[1024];
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!to_wide(path, w, 1024) || !GetFileAttributesExFromAppW(w, GetFileExInfoStandard, &a))
    {
        errno = ENOENT;
        return -1;
    }
    memset(st, 0, sizeof(*st));
    st->st_mode = (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? (_S_IFDIR | 0777) : (_S_IFREG | 0666);
    st->st_nlink = 1;
    st->st_size = (_off_t)((((unsigned long long)a.nFileSizeHigh) << 32) | a.nFileSizeLow);
    const unsigned long long t = (((unsigned long long)a.ftLastWriteTime.dwHighDateTime) << 32) | a.ftLastWriteTime.dwLowDateTime;
    st->st_mtime = t > 116444736000000000ULL ? (time_t)((t - 116444736000000000ULL) / 10000000ULL) : 0;
    st->st_atime = st->st_ctime = st->st_mtime;
    return 0;
}

struct DIR
{
    HANDLE find;
    WIN32_FIND_DATAW data;
    bool first;
    struct dirent entry;
};

static unsigned dir_error;
unsigned uwp_opendir_error(void) { return dir_error; }

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
        dir_error = GetLastError();
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
