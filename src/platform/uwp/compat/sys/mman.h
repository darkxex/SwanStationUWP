/*
 * SwanStationUWP - anonymous mmap/munmap on VirtualAlloc (what discimport.c uses for its read-ahead buffer).
 * Only MAP_ANON | MAP_PRIVATE regions of plain read/write memory are supported.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <windows.h>
#include <stddef.h>

#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_PRIVATE 1
#define MAP_ANON 2
#define MAP_ANONYMOUS MAP_ANON
#define MAP_FAILED ((void *)-1)

static __inline void *mmap(void *addr, size_t length, int prot, int flags, int fd, long long offset)
{
    (void)addr, (void)prot, (void)fd, (void)offset;
    if (!(flags & MAP_ANON))
        return MAP_FAILED;
    void *p = VirtualAlloc(NULL, length, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    return p ? p : MAP_FAILED;
}

static __inline int munmap(void *addr, size_t length)
{
    (void)length;
    return VirtualFree(addr, 0, MEM_RELEASE) ? 0 : -1;
}
