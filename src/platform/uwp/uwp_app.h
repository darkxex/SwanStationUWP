/*
 * SwanStationUWP - the UWP application shell (CoreApplication view), seen from the platform layer.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SwanStationXbox_UWP_APP_H
#define SwanStationXbox_UWP_APP_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The frontend's main (src/main.c), run on the UI thread: the Vulkan driver (Mesa's Dozen) reads the CoreWindow of the
 * thread that calls it, so the swap chain and the main loop have to live on that thread. */
int swanstation_main(int argc, char **argv);

/* Runs the events waiting for the window (activation, close, controller changes). */
void uwp_pump_events(void);
/* The window was closed or the system asked the app to end. */
bool uwp_quit_requested(void);
/* The app is going to the background (the console's Home screen): save what is open. Called on the UI thread. */
void uwp_set_suspend_handler(void (*handler)(void));
/* The folder the package was installed in, and the app's own writable folder (UTF-8, no trailing slash). */
const char *uwp_install_folder(void);
const char *uwp_local_folder(void);
/* Asks the TV for a mode (HDMI display mode switch); the raw size of the current one in width/height, refresh in hz.
 * Returns false if the console refused or the mode does not exist. */
bool uwp_request_display_mode(int width, int height, double hz);
/* The window changed size since the last call (a PC window resized, the TV's mode changed): true once per change. */
bool uwp_take_window_resized(void);
/* Opens Windows' Settings page that lets the app read every file (a PC); false where it does not exist (a console). */
bool uwp_open_file_access_settings(void);
/* The window's size in pixels (its size in view units times the screen's scale); false while there is no window. */
bool uwp_window_pixels(int *width, int *height);
bool uwp_current_display_mode(int *width, int *height, double *hz);
/* What the system says of the window (bounds, visible bounds, scale, full screen, TV mode), for the log. */
void uwp_describe_window(char *out, size_t size);
/* Asks again for the whole screen (bounds of the core window, full screen): RetroArch-UWP reacts after the loop starts. */
void uwp_reassert_fullscreen(void);

#ifdef __cplusplus
}
#endif
#endif
