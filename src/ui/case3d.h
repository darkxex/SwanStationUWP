/*
 * SwanStation - a PS1 jewel case in 3D (assets/psx_case_NTSC.obj), drawn with plat_draw_mesh.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SwanStationPS5_CASE3D_H
#define SwanStationPS5_CASE3D_H

#include "../platform/platform.h"

#include <stdbool.h>
#include <stdint.h>

/* Loads the model on first use; false when the file is missing or unreadable (callers fall back to flat covers). */
bool case3d_available(void);

/* Width / height of the case facing the camera. */
float case3d_aspect(void);
/* Thickness / height: the edge showing when the case is turned. */
float case3d_depth(void);

/* The turn (radians, either way) up to which only the case's front is in view: further, its side shows. */
float case3d_max_yaw(void);
/* The tilt (radians, either way: top away or towards the camera) up to which the back is still out of view. */
float case3d_max_pitch(void);

/* The launch animation of the case drawn next (case3d_draw takes it and forgets it): `open` (0..1) swings its lid open on
 * the hinge, `spin` (radians) turns the CD on its tray; `in_tray`: the CD stays flat on the tray, without lifting or growing. */
void case3d_launch(float open, float spin, bool in_tray);

/* Whether the cover gets light (its shading and the glare over the lid's plastic); off: the picture as it is. The rest of
 * the case and the CD keep theirs. Stays until changed. */
void case3d_lighting(bool on);

/* How dark the patch of a case's contact shadow on the floor is (a case with a reflection has it): 1 as it is, less is more
 * transparent, 0 none. Stays until changed. */
void case3d_floor_shadow(float strength);

/* The next case drawn lights its cover sheet like matte paper (plain diffuse light from the light point of the plastic's
 * glare: it darkens as it turns from the light, no shine): for the picture shown for games without a cover. */
void case3d_matte(bool on);

/* One case centred on (cx, cy), `h` pixels tall. `yaw` turns it about the vertical axis (radians, positive takes its
 * right edge away from the camera), `pitch` about the horizontal one (positive takes its top away). `cover` is the front art (NULL: a plain card). `tint` shades it (rgb) and fades
 * it (alpha), like the flat covers' tint. `reflection` adds the mirror image on the floor below. */
void case3d_draw(PlatTexture *cover, float cx, float cy, float h, float yaw, float pitch, uint32_t tint,
                 bool reflection);

#endif
