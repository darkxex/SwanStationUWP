/*
 * SwanStationUWP - the PS5 build's network shares (libnfs) and NTFS disk reader have no counterpart in a UWP
 * package: these keep the frontend's calls in place and report "nothing mounted". Games come from the app's own
 * folder and from drives the user has given the app access to.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "../../nfs/nfs.h"
#include "../../ntfs/ntfsfs.h"

#include <stdio.h>
#include <string.h>

bool nfs_set_shares(const char *list, char *error, size_t error_size)
{
    (void)list;
    if (error && error_size)
        error[0] = '\0';
    return true;
}
int nfs_mount_all(int timeout_ms) { (void)timeout_ms; return 0; }
int nfs_mount_points(char out[][NFS_PATH_MAX], int max) { (void)out, (void)max; return 0; }
void nfs_mount_begin(int timeout_ms) { (void)timeout_ms; }
bool nfs_mount_running(void) { return false; }
bool nfs_mount_finished(void) { return false; }
int nfs_first_failure(char *detail, size_t size)
{
    if (detail && size)
        detail[0] = '\0';
    return NFS_FAIL_NONE;
}
bool nfs_connection_lost(void) { return false; }
void nfs_set_read_timeout(int seconds) { (void)seconds; }
bool nfs_reconnecting(void) { return false; }
int nfs_unmounted(void) { return 0; }
void nfs_set_sbi_dir(const char *dir) { (void)dir; }
void nfs_describe(char *out, size_t size)
{
    if (out && size)
        out[0] = '\0';
}

int ntfsfs_scan(void) { return 0; }
int ntfsfs_mount_points(char out[][NTFSFS_PATH_MAX], int max) { (void)out, (void)max; return 0; }
bool ntfsfs_connection_lost(void) { return false; }
int ntfsfs_dirty_count(void) { return 0; }
