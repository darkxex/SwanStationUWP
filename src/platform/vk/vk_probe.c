/*
 * SwanStation Xbox - the old bring-up probe of the Xbox build is gone: the presenter's own start (vkp_open) logs the
 * GPU, the driver and the window it found. Kept so main.c's call stays.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "vk_probe.h"

void vk_probe(const char *root)
{
    (void)root;
}
