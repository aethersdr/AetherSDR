/* The time-of-day period for WSPR band hopping: sunrise grayline, day,
 * sunset grayline or night at a Maidenhead grid. Translated from WSJT-X
 * 3.0.2's lib/grayline.f90, lib/sun.f90 and lib/grid2deg.f90 (Joe Taylor
 * K1JT and others, GPL-3.0-or-later), keeping their single-precision
 * arithmetic so the period changes at the same moment as in WSJT-X. Part of
 * AetherSDR, GPL-3.0-or-later. */
#include "grayline.h"

#include <math.h>

/* Degrees west longitude and north latitude of a grid, as grid2deg: a
 * 4-character grid is the centre of its square ("mm"). */
static void grid2deg(const char *grid0, float *dlong, float *dlat)
{
    char g[7] = "      ";
    for (int i = 0; i < 6 && grid0[i]; i++) g[i] = grid0[i];
    int i5 = (unsigned char)g[4];
    if (g[4] == ' ' || i5 <= 64 || i5 >= 128) g[4] = 'm', g[5] = 'm';
    if (g[0] >= 'a' && g[0] <= 'z') g[0] = (char)(g[0] + 'A' - 'a');
    if (g[1] >= 'a' && g[1] <= 'z') g[1] = (char)(g[1] + 'A' - 'a');
    if (g[4] >= 'A' && g[4] <= 'Z') g[4] = (char)(g[4] - 'A' + 'a');
    if (g[5] >= 'A' && g[5] <= 'Z') g[5] = (char)(g[5] - 'A' + 'a');
    int nlong = 180 - 20 * (g[0] - 'A');
    int n20d = 2 * (g[2] - '0');
    float xminlong = 5 * ((float)(g[4] - 'a') + 0.5f);
    *dlong = (float)(nlong - n20d) - xminlong / 60.0f;
    int nlat = -90 + 10 * (g[1] - 'A') + g[3] - '0';
    float xminlat = 2.5f * ((float)(g[5] - 'a') + 0.5f);
    *dlat = (float)nlat + xminlat / 60.0f;
}

/* Fortran MOD for reals, as gfortran implements it. */
static float fmodr(float a, float p) { return fmodf(a, p); }

/* The sun's elevation in degrees (sun.f90, the parts grayline uses). */
static float sun_elevation(int y, int m, int dd, float ut, float lon, float lat)
{
    const float rad = 57.2957795f;
    float d = (float)(367 * y - 7 * (y + (m + 9) / 12) / 4 + 275 * m / 9 + dd - 730530) + ut / 24.0f;
    float ecl = 23.4393f - 3.563e-7f * d;
    float w = 282.9404f + 4.70935e-5f * d;
    float e = 0.016709f - 1.151e-9f * d;
    float mm = (float)fmod(356.0470 + 0.9856002585 * (double)d + 360000.0, 360.0);
    float ls = fmodr(w + mm + 720.0f, 360.0f);
    float ee = mm + e * rad * sinf(mm / rad) * (1.0f + e * cosf(mm / rad));
    ee = ee - (ee - e * rad * sinf(ee / rad) - mm) / (1.0f - e * cosf(ee / rad));
    float xv = cosf(ee / rad) - e;
    float yv = sqrtf(1.0f - e * e) * sinf(ee / rad);
    float v = rad * atan2f(yv, xv);
    float r = sqrtf(xv * xv + yv * yv);
    float lonsun = fmodr(v + w + 720.0f, 360.0f);
    float xs = r * cosf(lonsun / rad);
    float ys = r * sinf(lonsun / rad);
    float xe = xs;
    float ye = ys * cosf(ecl / rad);
    float ze = ys * sinf(ecl / rad);
    float ra = rad * atan2f(ye, xe);
    float dec = rad * atan2f(ze, sqrtf(xe * xe + ye * ye));
    float gmst0 = (ls + 180.0f) / 15.0f;
    float lst = fmodr(gmst0 + ut + lon / 15.0f + 48.0f, 24.0f);
    float ha = 15.0f * lst - ra;
    float xx = cosf(ha / rad) * cosf(dec / rad);
    float zz = sinf(dec / rad);
    float zhor = xx * cosf(lat / rad) + zz * sinf(lat / rad);
    return rad * asinf(zhor);
}

int wspr_grayline(int year, int month, int day, float uth, const char *grid, int duration_min)
{
    float elon, lat;
    grid2deg(grid, &elon, &lat);
    float lon = -elon;
    float uth0 = uth - 0.5f * (float)duration_min / 60.0f;
    float uth1 = uth + 0.5f * (float)duration_min / 60.0f;
    float el0 = sun_elevation(year, month, day, uth0, lon, lat);
    float el1 = sun_elevation(year, month, day, uth1, lon, lat);
    const float elchk = -0.8333f;
    if (el0 < elchk && el1 >= elchk) return WSPR_SUNRISE;
    if (el0 > elchk && el1 <= elchk) return WSPR_SUNSET;
    if (el1 > elchk) return WSPR_DAY;
    return WSPR_NIGHT;
}
