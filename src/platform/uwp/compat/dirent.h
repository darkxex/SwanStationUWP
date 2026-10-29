/*
 * SwanStation Xbox - dirent.h on FindFirstFileExFromAppW, which a UWP app may call on any folder it has been given
 * (its own LocalState, the install folder, and removable drives where the user allowed file access).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SwanStationXbox_DIRENT_H
#define SwanStationXbox_DIRENT_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

#define DT_UNKNOWN 0
#define DT_DIR 4
#define DT_REG 8

struct dirent
{
    unsigned char d_type;
    char d_name[1024];
};
typedef struct DIR DIR;

DIR *opendir(const char *path);
struct dirent *readdir(DIR *dir);
int closedir(DIR *dir);

#ifdef __cplusplus
}
#endif
#endif
