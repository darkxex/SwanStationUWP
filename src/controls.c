/*
 * SwanStationUWP - what SwanStationUWP does with the controller while a game runs: the sticks' dead zone and response,
 * gas and brake on the triggers in racing games, and the NeGcon.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "controls.h"

#include "core/host.h"
#include "gamedb.h"
#include "platform/platform.h"

#include <math.h>
#include <string.h>

/* ---------------------------------------------------------------- state */

static struct
{
    int special; /* 1 NeGcon (analog gas and brake) */
} C;

/* Other controllers: 1 NeGcon, for the racing games that take one (DuckStation's database) */
int controls_special_for(const Game *g, const Settings *s)
{
    const GameInfo *info = gamedb_get(g->serial);
    if (info && s->negcon && (info->flags & GDB_NEGCON))
        return 1;
    return 0;
}

void controls_start(const Game *g, const Settings *s)
{
    memset(&C, 0, sizeof(C));
    C.special = controls_special_for(g, s);
}

void controls_stop(void)
{
    memset(&C, 0, sizeof(C));
}

void controls_reconfigure(const Game *g, const Settings *s)
{
    const int special = controls_special_for(g, s);
    controls_stop();
    controls_start(g, s);
    host_set_special(special);
    host_apply_devices(s);
}

/* ---------------------------------------------------------------- sticks */

static int16_t clamp16(float v)
{
    return (int16_t)(v > 32767.0f ? 32767.0f : v < -32767.0f ? -32767.0f : v);
}

static void shape_stick(int16_t *x, int16_t *y, int deadzone, int response)
{
    if (!deadzone && !response)
        return;
    float fx = *x / 32767.0f, fy = *y / 32767.0f, m = sqrtf(fx * fx + fy * fy);
    float dz = deadzone * 0.05f;
    if (m <= dz || m < 0.0001f)
    {
        *x = *y = 0;
        return;
    }
    float k = (m - dz) / (1.0f - dz);
    if (k > 1.0f)
        k = 1.0f;
    if (response == 1)
        k = powf(k, 1.8f); /* precise: small moves stay small */
    else if (response == 2)
        k = powf(k, 0.6f); /* quick: a little tilt goes far */
    float scale = k / m * 32767.0f;
    *x = clamp16(fx * scale);
    *y = clamp16(fy * scale);
}

/* ---------------------------------------------------------------- each frame */

void controls_apply(PadState pads[SwanStationPS5_MAX_PADS], const Settings *s, float dt)
{
    (void)dt;
    for (int i = 0; i < SwanStationPS5_MAX_PADS; ++i)
    {
        PadState *p = &pads[i];
        if (!p->connected)
            continue;
        shape_stick(&p->lx, &p->ly, s->stick_deadzone, s->stick_response);
        shape_stick(&p->rx, &p->ry, s->stick_deadzone, s->stick_response);
        /* in every game when it's on; with a NeGcon the triggers are analog already: no digital remap */
        if (s->racing_triggers && C.special != 1)
        {
            /* a swap: R2 presses Cross and Cross presses R2, L2 and Square the same */
            uint32_t b = p->buttons, swapped = b & ~(BIT(BTN_R2) | BIT(BTN_CROSS) | BIT(BTN_L2) | BIT(BTN_SQUARE));
            swapped |= (b & BIT(BTN_R2) ? BIT(BTN_CROSS) : 0) | (b & BIT(BTN_CROSS) ? BIT(BTN_R2) : 0);
            swapped |= (b & BIT(BTN_L2) ? BIT(BTN_SQUARE) : 0) | (b & BIT(BTN_SQUARE) ? BIT(BTN_L2) : 0);
            p->buttons = swapped;
        }
    }
}
