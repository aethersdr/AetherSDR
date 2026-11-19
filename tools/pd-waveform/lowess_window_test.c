/* Differential LOWESS check: standalone window search versus original WDSP. */
#include "extrapolate.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Compile the unmodified arithmetic and original window selection under
 * private test names. The ordinary symbols link the standalone fast path. */
#undef AETHER_PD_LOWESS_WINDOW_SEARCH
#define build_lowess reference_build_lowess
#define teardown_lowess reference_teardown_lowess
#define build_extrap0 reference_build_extrap0
#define build_extrap1 reference_build_extrap1
#define teardown_extrap0 reference_teardown_extrap0
#define teardown_extrap1 reference_teardown_extrap1
#define extrapolate_y_at_0 reference_extrapolate_y_at_0
#define extrapolate_y_at_1 reference_extrapolate_y_at_1
#include "../../../third_party/wdsp/upstream/extrapolate.c"
#undef build_lowess
#undef teardown_lowess
#undef build_extrap0
#undef build_extrap1
#undef teardown_extrap0
#undef teardown_extrap1
#undef extrapolate_y_at_0
#undef extrapolate_y_at_1

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>

static void identical(ExtrapolationResult expected, ExtrapolationResult actual)
{
    assert(expected.confidence == actual.confidence);
    assert(memcmp(&expected.y_at_1, &actual.y_at_1, sizeof(double)) == 0);
    assert(memcmp(&expected.y_linear, &actual.y_linear, sizeof(double)) == 0);
    assert(memcmp(&expected.y_quadratic, &actual.y_quadratic, sizeof(double)) == 0);
    assert(memcmp(&expected.y_cubic, &actual.y_cubic, sizeof(double)) == 0);
}

int main(void)
{
    (void)wls_degree1;
    enum { kCapacity = 4096 };
    double x[kCapacity], y[kCapacity];
    const int sizes[] = {3, 4, 5, 16, 65, 128, 257, 1024, kCapacity};
    EXTRAP0 originalLow = reference_build_extrap0(kCapacity);
    EXTRAP1 originalHigh = reference_build_extrap1(kCapacity);
    EXTRAP0 fastLow = build_extrap0(kCapacity);
    EXTRAP1 fastHigh = build_extrap1(kCapacity);
    assert(originalLow && originalHigh && fastLow && fastHigh);
    uint32_t seed = 12345;
    unsigned comparisons = 0;
    for (int pattern = 0; pattern < 6; ++pattern) {
        for (unsigned size = 0; size < sizeof(sizes)/sizeof(sizes[0]); ++size) {
            int n = sizes[size];
            for (int i = 0; i < n; ++i) {
                seed = seed * 1664525u + 1013904223u;
                double random = (double)(seed >> 8)/16777216.0;
                switch (pattern) {
                case 0: x[i] = (double)i/(n-1); break;
                case 1: x[i] = random; break;
                case 2: x[i] = (double)(seed%8)/7; break;
                case 3: x[i] = 0.5; break;
                case 4: x[i] = nextafter((double)(seed%4)/3, INFINITY); break;
                default: x[i] = 1.2*random-0.1; break;
                }
                y[i] = 0.8*x[i]+0.1*x[i]*x[i]+0.002*sin(33*i);
                if (pattern == 1 && i%37 == 0) { y[i] += 0.2; }
            }
            identical(reference_extrapolate_y_at_1(originalHigh,x,y,n),
                      extrapolate_y_at_1(fastHigh,x,y,n));
            identical(reference_extrapolate_y_at_0(originalLow,x,y,n,0.0,0.15),
                      extrapolate_y_at_0(fastLow,x,y,n,0.0,0.15));
            comparisons += 2;
        }
    }
    teardown_extrap0(fastLow); teardown_extrap1(fastHigh);
    reference_teardown_extrap0(originalLow); reference_teardown_extrap1(originalHigh);
    printf("LOWESS: %u bit-identical endpoint/confidence comparisons\n", comparisons);
    return 0;
}
