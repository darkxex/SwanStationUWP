/*
 * SwanStationUWP - what SwanStationUWP does with the controller while a game runs: stick dead
 * zone and response, gas and brake on the triggers in racing games, and the NeGcon.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SwanStationPS5_CONTROLS_H
#define SwanStationPS5_CONTROLS_H

#include "library.h"
#include "SwanStationPS5.h"

/* Other controllers: 0 pads, 1 NeGcon (racing games that take one); from DuckStation's database. */
int controls_special_for(const Game *g, const Settings *s);
/* Before the game loads, and when it stops. */
void controls_start(const Game *g, const Settings *s);
void controls_stop(void);
/* Each frame, on the buttons the game will see. */
void controls_apply(PadState pads[SwanStationPS5_MAX_PADS], const Settings *s, float dt);
/* A setting that picks the controller changed while a game runs. */
void controls_reconfigure(const Game *g, const Settings *s);

#endif
