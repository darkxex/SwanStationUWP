/*
 * SwanStation - games on NFS servers (the frontend's side of src/nfs/nfs.cpp).
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The console's kernel has no NFS client, so the app is its own (libnfs, third_party/libnfs): a share is served
 * to the whole process under /nfs/<host>/<path>, by passing the C library's file calls through nfs.cpp. The
 * frontend scans that folder and the core reads the discs from it as from any other. Read-only.
 *
 * A share is an nfs:// address, several separated by ';' or new lines:
 *   nfs://192.168.1.85/psx             NFS v3 (the server's portmapper), else v4
 *   nfs://192.168.1.10/srv/games?version=4     only v4
 *   nfs://nas/psx?nfsport=2049&mountport=1058  fixed ports, no portmapper; also uid=, gid=
 * The folder may be inside what the server exports.
 */
#ifndef SwanStationPS5_NFS_H
#define SwanStationPS5_NFS_H

#include <stdbool.h>
#include <stddef.h>

#define NFS_MAX_SHARES 8

    enum
    {
        NFS_FAIL_NONE = 0,
        NFS_FAIL_NO_ANSWER,    /* the server did not answer in time */
        NFS_FAIL_REFUSED,      /* it answered and refused the mount, without saying why */
        NFS_FAIL_NOT_EXPORTED, /* that folder is not exported; detail: what the server does export */
        NFS_FAIL_NO_FOLDER,    /* mounted, but the folder is not there */
        NFS_FAIL_CONNECT,      /* the connection was refused or could not be made */
        NFS_FAIL_CLOSED,       /* the server closed the connection */
        NFS_FAIL_NAME,         /* the server's name could not be found */
        NFS_FAIL_DENIED,       /* the server denies this console */
        NFS_FAIL_OTHER         /* detail: what libnfs said */
    };
#define NFS_PATH_MAX 512

#ifdef __cplusplus
extern "C"
{
#endif

    /* Replaces the shares with those of the list. Addresses that don't parse are left out; `error` (when given)
     * gets the first one as "<address>\t<reason>", the reason being a sentence of the interface (to translate).
     * True when every one parsed. Never touches the network. */
    bool nfs_set_shares(const char *list, char *error, size_t error_size);

    /* Mounts every share not mounted yet, all at once, each giving up after about timeout_ms. Blocks. Returns
     * how many shares are mounted. */
    int nfs_mount_all(int timeout_ms);

    /* The mounted shares' folders (/nfs/<host>/<path>), up to max. Returns how many. */
    int nfs_mount_points(char out[][NFS_PATH_MAX], int max);

    /* nfs_mount_all on its own thread, so the app does not wait for the servers (at start). Nothing happens while
     * one is already running. */
    void nfs_mount_begin(int timeout_ms);
    /* True while the mount begun by nfs_mount_begin has not finished. */
    bool nfs_mount_running(void);
    /* True once, when the mount begun by nfs_mount_begin has finished. */
    bool nfs_mount_finished(void);

    /* Why the first share that is not mounted is not: a NFS_FAIL_* code (NFS_FAIL_NONE when all are), and a detail. */
    int nfs_first_failure(char *detail, size_t size);

    /* True once when a read from a share failed (the server went away): the game that was reading it cannot go on. */
    bool nfs_connection_lost(void);
    /* How long a read may take, in seconds, before it is cut and tried again on a new connection. */
    void nfs_set_read_timeout(int seconds);
    /* True while a read that failed is being tried again; the game waits. */
    bool nfs_reconnecting(void);

    /* How many of the shares set are not mounted. */
    int nfs_unmounted(void);

    /* Where the .sbi files of the games on a share are kept: the share is read only and the core looks for the .sbi
     * beside the disc image, so an .sbi under /nfs that the server does not have is read from and written to a file of
     * this folder instead. */
    void nfs_set_sbi_dir(const char *dir);

    /* One line per share with its state, for the log and the notices. */
    void nfs_describe(char *out, size_t size);

#ifdef __cplusplus
}
#endif

#endif
