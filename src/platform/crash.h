/*
 * SwanStationUWP - the Xbox build wrote a crash report into the log from a signal handler; here a crash is caught by the
 * system (Device Portal -> Crash dumps), so these only keep the call sites.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SwanStationXbox_CRASH_H
#define SwanStationXbox_CRASH_H
#define ps5_crash_install(path) ((void)(path))
#define ps5_crash_step(step) ((void)(step))
#endif
