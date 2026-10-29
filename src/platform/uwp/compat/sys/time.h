/* SwanStation Xbox: gettimeofday on the Win32 clock. SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <winsock2.h>
#include <time.h>
static __inline int gettimeofday(struct timeval *tv, void *tz)
{
    (void)tz;
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    unsigned long long t = ((unsigned long long)ft.dwHighDateTime << 32 | ft.dwLowDateTime) - 116444736000000000ull;
    tv->tv_sec = (long)(t / 10000000ull);
    tv->tv_usec = (long)(t % 10000000ull / 10);
    return 0;
}
