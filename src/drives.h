/*
 * SwanStation Xbox - the drives a games folder is looked for on. On the console the USB drives and the extended storage
 * show up as drive letters; a UWP app sees one only if the user gave it access, so a letter that is not there is simply
 * left out. Games are looked for in <letter>:/<Games folder>, as "psx" by default.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SwanStationPS5_DRIVES_H
#define SwanStationPS5_DRIVES_H

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <sys/stat.h>
#include <windows.h>

#define DRIVE_EXT_COUNT 0 /* nothing here is storage the app may install to without asking */
#define DRIVE_USB_COUNT 8
#define DRIVE_COUNT (DRIVE_EXT_COUNT + DRIVE_USB_COUNT)
#define DRIVE_PREFIX_LEN 0 /* characters of a path that are not its name */

static const char *const drive_paths[DRIVE_COUNT] = {"D:", "E:", "F:", "G:", "H:", "I:", "J:", "K:"};

static inline long long drive_free_mb(const char *path)
{
    wchar_t root[8];
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, root, 6) <= 0)
        return -1;
    wcscat(root, L"\\");
    ULARGE_INTEGER avail, total, total_free;
    if (!GetDiskFreeSpaceExW(root, &avail, &total, &total_free))
        return -1;
    return (long long)(avail.QuadPart >> 20);
}

static inline bool drive_usable(int d)
{
    return drive_free_mb(drive_paths[d]) >= 256;
}

static inline int drives_list(char *buf, size_t size, int only)
{
    int found = 0;
    size_t len = 0;
    if (size)
        buf[0] = '\0';
    for (int d = 0; d < DRIVE_COUNT; ++d)
    {
        if (!drive_usable(d))
            continue;
        long long free_mb = drive_free_mb(drive_paths[d]);
        if (only >= 0 && found++ != only)
            continue;
        const char *name = drive_paths[d] + DRIVE_PREFIX_LEN;
        int n = snprintf(buf + len, size - len, "%s%s %lld.%lld GB", only < 0 && len ? ", " : "", name, free_mb >> 10,
                         (free_mb & 1023) * 10 >> 10);
        if (n < 0 || (size_t)n >= size - len)
            break;
        len += (size_t)n;
        if (only >= 0)
            break;
        ++found;
    }
    return found;
}

/* Installing a disc to extended storage: there is none here, discs are not read (no disc drive in a UWP app) */
static inline bool drives_ext_games(char *out, size_t size)
{
    (void)out, (void)size;
    return false;
}

#endif
