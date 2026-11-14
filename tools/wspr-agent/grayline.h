/* WSPR band hopping's time-of-day periods. Part of AetherSDR,
 * GPL-3.0-or-later. */
#ifndef AETHER_WSPR_GRAYLINE_H
#define AETHER_WSPR_GRAYLINE_H

/* The values WSJT-X's grayline routine returns (isun). */
enum { WSPR_SUNRISE = 0, WSPR_DAY = 1, WSPR_SUNSET = 2, WSPR_NIGHT = 3 };

/* The period at UTC date year-month-day, hour uth (fractional) for a 4- or
 * 6-character grid, with gray lines duration_min long. */
int wspr_grayline(int year, int month, int day, float uth, const char *grid, int duration_min);

#endif
