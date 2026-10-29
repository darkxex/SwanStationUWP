/* SwanStation - time left of a disc installation, shared by the disc daemon and the app.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SwanStationPS5_DISCETA_H
#define SwanStationPS5_DISCETA_H

#include <math.h>
#include <stdbool.h>

/* The time left, from the speed just measured and the shape of a CD. A CD spins at fixed revolutions and its sectors
 * run in a spiral of constant density, so the read speed at any point goes with the radius there, and the radius
 * with how many sectors came before it: r(f)^2 = rin^2 + K f, f the fraction done. With the speed measured over the
 * last stretch (at the stretch's middle), the rest takes (N r_mid / v) * (2 / K) * (r(1) - r(now)).
 * A drive that does not speed up towards the edge gets the plain "same speed from here" estimate instead. */
#define CD_R_IN 24.0f    /* mm, where the data starts */
#define CD_R_OUT 58.0f   /* mm, the largest it gets */
#define CD_CAPACITY 333000.0 /* sectors of a 74 minute disc, at that largest radius */

typedef struct
{
    double prev_f;      /* where the last notification was (fraction), and when */
    double prev_time;
    double prev_rate;   /* the speed of that stretch, sectors/s, and the radius at its middle */
    double prev_rmid;
} Eta;

static inline double cd_radius(double k, double f)
{
    return sqrt((double)CD_R_IN * CD_R_IN + k * f);
}

/* seconds left, or < 0 when there is nothing to base it on */
static inline long eta_left(Eta *e, double f_now, double now, int total)
{
    long left = -1;
    if (total > 0 && f_now > e->prev_f && now > e->prev_time && f_now < 1.0)
    {
        double dt = now - e->prev_time, rate = total * (f_now - e->prev_f) / dt;
        double plain = (1.0 - f_now) * dt / (f_now - e->prev_f); /* the same speed for the rest */
        double k = (total / CD_CAPACITY) * ((double)CD_R_OUT * CD_R_OUT - (double)CD_R_IN * CD_R_IN);
        double rmid = cd_radius(k, (e->prev_f + f_now) * 0.5);
        double modelled = (total * rmid / rate) * (2.0 / k) * (cd_radius(k, 1.0) - cd_radius(k, f_now));
        /* does this drive speed up the way the model says? at least half of the speed-up the model expects between
         * the two stretches (the noise of one stretch is a percent or two, the model's step is a few) */
        bool speeds_up = true;
        if (e->prev_rate > 0.0 && e->prev_rmid > 0.0)
            speeds_up = rate / e->prev_rate >= 1.0 + 0.5 * (rmid / e->prev_rmid - 1.0);
        double best = speeds_up && modelled > 0.0 && modelled < 3.0 * plain ? modelled : plain;
        left = (long)best;
        e->prev_rate = rate;
        e->prev_rmid = rmid;
    }
    e->prev_f = f_now;
    e->prev_time = now;
    return left;
}

#endif
