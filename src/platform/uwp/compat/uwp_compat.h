/*
 * SwanStationUWP - force-included in every C and C++ source of the UWP build (/FI): the GNU spellings the frontend
 * uses, on MSVC. No behaviour here, only names.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SwanStationXbox_COMPAT_H
#define SwanStationXbox_COMPAT_H

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS 1
#endif
#ifndef _CRT_NONSTDC_NO_DEPRECATE
#define _CRT_NONSTDC_NO_DEPRECATE 1
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <io.h>
#include <math.h>
#include <time.h>
#include <intrin.h>
#include <direct.h>
#include <sys/types.h>
#include <sys/stat.h>

#if defined(_MSC_VER) && !defined(__clang__)
#define __attribute__(x)
#define __builtin_popcount(x) ((int)__popcnt((unsigned)(x)))
#define __builtin_popcountll(x) ((int)__popcnt64((unsigned __int64)(x)))
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#define strtok_r strtok_s
typedef intptr_t ssize_t;
#endif

#ifdef __cplusplus
extern "C" {
#endif
/* POSIX rename replaces the target: the frontend saves everything by writing a temp file and renaming it over the
 * old one, which the CRT's rename refuses to do. (Defined after the system headers, which declare rename.) */
int uwp_rename(const char *from, const char *to);
FILE *uwp_fopen(const char *path, const char *mode);
/* stat on the *FromApp calls: the CRT's stat only sees the app's own folders, not a drive the user gave access to. */
int uwp_stat(const char *path, struct stat *st);
/* mkdir on CreateDirectoryFromApp (as RetroArch's UWP build): 0, or -1 with errno EEXIST, ENOENT or EACCES. */
int uwp_mkdir(const char *path);
/* The Windows error of the last uwp_fopen or uwp_mkdir that failed. */
unsigned uwp_last_error(void);
#ifdef __cplusplus
}
#endif
#define rename(from, to) uwp_rename((from), (to))
#define fopen(path, mode) uwp_fopen((path), (mode))
#define stat(path, st) uwp_stat((path), (st))
#define mkdir(path, mode) uwp_mkdir((path))
#ifndef S_ISDIR
#define S_ISDIR(m) (((m)&_S_IFMT) == _S_IFDIR)
#define S_ISREG(m) (((m)&_S_IFMT) == _S_IFREG)
#endif

#endif
