/*
 * SwanStation - the choices of the core's three BIOS options (NTSC-J, NTSC-U, PAL).
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Their list is not the table's fixed four: it is Auto (the core picks the folder's BIOS for the region), the OpenBIOS
 * built into the core, and every .bin in the BIOS folder. The list only grows (a file added later is appended), so an
 * option's chosen index stays valid while the app runs; the files hold the value by name.
 * Every other option goes on being the table's.
 */
#ifndef SwanStationPS5_BIOS_CHOICES_H
#define SwanStationPS5_BIOS_CHOICES_H

#include <stdbool.h>

bool ss_opt_is_bios(int opt);
int ss_opt_count(int opt);
const char *ss_opt_value(int opt, int index);      /* what the core is given ("" is Auto) */
const char *const *ss_opt_labels(int opt);         /* what the menu shows */
/* Index of a value, or -1; with `add`, a BIOS name that is not in the list yet is appended to it. */
int ss_opt_find(int opt, const char *value, bool add);
/* Appends the .bin files of the folder not listed yet. */
void bios_choices_scan(const char *dir);

#endif
