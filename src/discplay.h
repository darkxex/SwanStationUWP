/*
 * SwanStation - a disc played from the drive: its sectors are read into memory in the background and the core
 * sees them as /discplay/<name>.cue and /discplay/<name>.bin (served by src/nfs/nfs.cpp). See discimport.h for the
 * side of the screens.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SwanStationPS5_DISCPLAY_H
#define SwanStationPS5_DISCPLAY_H

#include <stddef.h>
#include <stdint.h>

#define DISCPLAY_DIR "/discplay"

#ifdef __cplusplus
extern "C"
{
#endif

    /* Which file of the disc `name` (the part after /discplay/) is: 0 none, 1 the .cue, 2 the .bin. *size gets its size. */
    int discplay_file(const char *name, uint64_t *size);
    /* Reads from the .cue (kind 1) or the .bin (kind 2). A sector not read yet is waited for and asked of the drive
     * first. Fewer bytes than asked for when the end is reached or the disc was lost. */
    size_t discplay_read(int kind, uint64_t offset, void *dst, size_t n);

#ifdef __cplusplus
}
#endif

#endif
