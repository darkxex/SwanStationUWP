/* SwanStationUWP - the few unistd.h calls the frontend makes, on the CRT's io.h. SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SwanStationXbox_UNISTD_H
#define SwanStationXbox_UNISTD_H
#include <io.h>
#include <direct.h>
#include <process.h>
#include <stdint.h>
#include <windows.h>
#include "uwp_compat.h"

#define F_OK 0
#define W_OK 2
#define R_OK 4
/* access, unlink, rmdir, getpid, close, read and write are the CRT's own (io.h, process.h) */
#define lseek(fd, off, whence) _lseeki64((fd), (off), (whence))
#define ftruncate(fd, len) _chsize_s((fd), (len))
#define fsync(fd) _commit(fd)
#define usleep(us) Sleep((us) / 1000)
#define sleep(s) Sleep((s) * 1000)
#endif
