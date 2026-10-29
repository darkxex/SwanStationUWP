/*
 * SwanStation - games on NTFS disks (the frontend's side of src/ntfs/ntfsfs.cpp).
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The console's kernel does not mount NTFS, so the app reads the disk itself (ntfs.c, bdev.c, part.c: the reader of
 * abcde6543/Xbox-NTFS-BROWSER, GPL-3.0-or-later, without its writing part). Each volume is served to the whole process
 * under /ntfs/<label>/<path> by passing the C library's file calls through nfs.cpp. Read-only.
 */
#ifndef SwanStationPS5_NTFSFS_H
#define SwanStationPS5_NTFSFS_H

#include <stdbool.h>
#include <stddef.h>

#define NTFSFS_MAX_VOLUMES 8
#define NTFSFS_PATH_MAX 512

#ifdef __cplusplus
extern "C"
{
#endif

    /* Looks for NTFS volumes on the disks that are plugged in (/dev/da*) and mounts the new ones. Blocks for as long as
     * the disks take to answer. Returns how many are mounted. */
    int ntfsfs_scan(void);

    /* The mounted volumes' roots (/ntfs/<label>), up to max. Returns how many. */
    int ntfsfs_mount_points(char out[][NTFSFS_PATH_MAX], int max);

    /* True once when a read from a disk failed (it was unplugged): the game that was reading it cannot go on. */
    bool ntfsfs_connection_lost(void);

    /* A volume that was not cleanly unmounted by Windows (Fast Startup): its latest changes may not be seen. */
    int ntfsfs_dirty_count(void);

#ifdef __cplusplus
}

#include <cstdint>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <vector>

struct NtfsFile;

bool ntfsfs_is_path(const char *path);
int ntfsfs_stat(const char *path, struct stat *st);
NtfsFile *ntfsfs_open(const char *path, uint64_t *size, int *error);
size_t ntfsfs_pread(NtfsFile *f, void *dst, size_t n, uint64_t pos);
void ntfsfs_close(NtfsFile *f);
int ntfsfs_list(const char *path, std::vector<std::pair<std::string, bool>> &entries);
#endif

#endif
