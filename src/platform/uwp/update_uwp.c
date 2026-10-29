/*
 * SwanStationUWP - there is no self-update: a sideloaded UWP package is replaced by installing a newer one (Device Portal
 * or tools\deploy-xbox.ps1). The settings page keeps its rows; they report "newest".
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "../../update.h"

static int state = UPDATE_IDLE;

void update_check(void) { state = UPDATE_NONE; }
void update_install(void) { state = UPDATE_FAILED; }
int update_state(void) { return state; }
const char *update_version(void) { return ""; }
const char *update_message(void) { return "Install a newer package with the Device Portal."; }
