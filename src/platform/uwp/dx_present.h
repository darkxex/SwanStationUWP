/*
 * SwanStationUWP - the screen through Direct3D 11 (flip-model swap chain on the CoreWindow).
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * One D3D11 device serves the whole app: the interface is drawn with it, and the core (SwanStation's D3D11 renderer) is
 * handed the same device and immediate context through libretro's D3D11 interface. The core leaves its finished picture
 * bound to the pixel shader's slot 0; dxp_take_core_frame() keeps that view and the next present draws it.
 */
#ifndef SwanStationUWP_DX_PRESENT_H
#define SwanStationUWP_DX_PRESENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool dxp_open(int canvas_w, int canvas_h, char *error, size_t size);
void dxp_close(void);
/* "1920x1080 @ 60 Hz" once open */
const char *dxp_describe(void);
double dxp_display_hz(void); /* the screen's refresh rate (0 when unknown) */
void dxp_want_size(int width, int height);   /* before dxp_open: the screen's mode (kept for the window) */
void dxp_want_extent(int width, int height); /* before dxp_open: the window's size in pixels, for the swap chain */
void dxp_want_high_refresh(bool on);
void dxp_force_resize(void); /* the next dxp_resize rebuilds the buffers even if the size is the same */
bool dxp_resize(int width, int height, char *error, size_t size); /* the window changed size: the swap chain follows */
bool dxp_set_mode(int width, int height, char *error, size_t size);

/* The interface drawn by the GPU. Coordinates are canvas pixels. */
typedef struct
{
    float x, y, u, v;
    uint32_t argb; /* tint, multiplied with the texture */
} DxpUiVertex;
bool dxp_ui_open(char *error, size_t size);
bool dxp_ui_active(void);
int dxp_ui_texture_create(const uint8_t *rgba, int w, int h, bool smooth, bool mips, bool copy); /* -1: failed */
void dxp_ui_texture_free(int id);
void dxp_ui_texture_update(int id, const uint8_t *rgba);
void dxp_ui_blend(bool on);                   /* the next draws are blended (default) or copied, ignoring the texture's alpha */
void dxp_ui_game_here(void);                  /* the game's picture goes here in the list */
void dxp_ui_begin(uint32_t clear_argb);       /* a new frame's list; the colour the screen is cleared to (alpha 0: black) */
void dxp_ui_clip(int x, int y, int w, int h); /* w <= 0: none */
void dxp_ui_draw(int id, const DxpUiVertex *v, int count); /* triangles; id < 0: no texture (the tint alone) */
void dxp_present_ui(void);                    /* the frame: the list, then the swap chain's present */

/* The game picture. */
bool dxp_game_image_ready(void);
void dxp_set_game_nearest(bool nearest);
void dxp_show_game(float x, float y, float w, float h, float crop, int shader, int tex_w, int tex_h, int lines);
/* The picture scaled to w x h RGBA; waits for the GPU. */
bool dxp_capture_game(uint8_t *rgba, int w, int h, int src_w, int src_h);

/* What stays of the Vulkan presenter's interface: nothing to do on this one yet (colour, FSR and frame generation come
 * back with the shaders). */
void dxp_set_colour(float brightness, float saturation, float warmth, float sharpen);
void dxp_set_fsr(bool on);
void dxp_set_framegen(int generated, double core_hz, double speed, bool nominal);
int dxp_framegen_times(void);
double dxp_framegen_hz(void);
void dxp_frame_repeated(void);

/* The core's side (host.c). dxp_core_interface() is a retro_hw_render_interface_d3d11 (device, immediate context). */
const void *dxp_core_interface(void);
/* The core presented a frame (video_refresh with RETRO_HW_FRAME_BUFFER_VALID): keeps the picture it left bound. */
void dxp_take_core_frame(unsigned width, unsigned height);
void dxp_drop_core_frame(void);

#ifdef __cplusplus
}
#endif
#endif
