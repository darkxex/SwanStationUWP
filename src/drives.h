/*
 * SwanStationUWP - the drives a games folder is looked for on. Every drive letter Windows reports is looked at, as
 * RetroArch's UWP build lists them (GetLogicalDrives): on a console a USB drive or the extended storage gets whatever
 * letter the system gives it. A UWP app can read one only where the user allowed file access to it (Settings > Privacy
 * > File system), and a drive it cannot read just has nothing found on it.
 * Games are looked for in <letter>:/SwanStation, <letter>:/SwanStation/games and <letter>:/<Games folder>, "psx" by
 * default.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SwanStationPS5_DRIVES_H
#define SwanStationPS5_DRIVES_H

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

#define DRIVE_COUNT 26 /* A: to Z: */

/* The drive letters that are there now, as "X:": how many, and in paths[] (4 bytes each). */
static inline int drives_present(char paths[DRIVE_COUNT][4])
{
    const DWORD mask = GetLogicalDrives();
    int n = 0;
    for (int i = 0; i < DRIVE_COUNT; ++i)
        if (mask & (1u << i))
        {
            /* an optical drive, a network share or a drive with no disk can take long to answer and holds no games */
            wchar_t root[4] = {(wchar_t)(L'A' + i), L':', L'\\', 0};
            const UINT type = GetDriveTypeW(root);
            if (type == DRIVE_CDROM || type == DRIVE_REMOTE || type == DRIVE_RAMDISK || type == DRIVE_NO_ROOT_DIR)
                continue;
            paths[n][0] = (char)('A' + i);
            paths[n][1] = ':';
            paths[n][2] = '\0';
            ++n;
        }
    return n;
}

/* Free space of a drive in MB, -1 when it cannot be asked (no access, no disk). */
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

/* The drives with their free space as text: "F: 28.2 GB". `only` < 0: all of them, separated by commas; else the one
 * at that position. Returns how many drives it found. A drive whose space cannot be asked is still listed. */
static inline int drives_list(char *buf, size_t size, int only)
{
    char paths[DRIVE_COUNT][4];
    const int total = drives_present(paths);
    size_t len = 0;
    if (size)
        buf[0] = '\0';
    for (int d = 0; d < total; ++d)
    {
        if (only >= 0 && d != only)
            continue;
        const long long free_mb = drive_free_mb(paths[d]);
        int n;
        if (free_mb >= 0)
            n = snprintf(buf + len, size - len, "%s%s %lld.%lld GB", only < 0 && len ? ", " : "", paths[d], free_mb >> 10,
                         (free_mb & 1023) * 10 >> 10);
        else
            n = snprintf(buf + len, size - len, "%s%s", only < 0 && len ? ", " : "", paths[d]);
        if (n < 0 || (size_t)n >= size - len)
            break;
        len += (size_t)n;
        if (only >= 0)
            break;
    }
    return total;
}

#endif
