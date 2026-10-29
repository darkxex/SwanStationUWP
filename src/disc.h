/*
 * SwanStation - reads a game's serial (SLUS-01041 ...) from its disc image.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SwanStationPS5_DISC_H
#define SwanStationPS5_DISC_H

#include "SwanStationPS5.h"

/* Fills `serial` ("SLUS-01041") from SYSTEM.CNF (.cue/.bin/.iso/.img/.m3u) or
 * PARAM.SFO (.pbp). Not CHD, which is compressed: the library asks the core for those. */
bool disc_read_serial(const char *path, char *serial, size_t size);

/* The same, from any source of 2048-byte sectors (a disc in a drive): read() fills `length` bytes of user data
 * from sector `lba`. */
typedef bool (*DiscReadSectors)(void *ctx, uint32_t lba, uint8_t *out, size_t length);
bool disc_serial_from_reader(DiscReadSectors read, void *ctx, char *serial, size_t size);

/* Normalises "SLUS_010.41", "SLUS01041", "slus-01041" to "SLUS-01041". */
bool disc_format_serial(const char *raw, char *serial, size_t size);

#endif
