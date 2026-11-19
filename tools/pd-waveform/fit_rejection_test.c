#include "pd_core.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static double uniform(uint32_t* seed)
{
    *seed = *seed * 1664525u + 1013904223u;
    return ((double)(*seed >> 8) + 0.5) / 16777216.0 - 0.5;
}

static void check_fit(double noise, int accepted)
{
    enum { kRate = 24000, kBlock = 128 };
    pd_core* core = pd_core_create(kRate, kBlock, .7);
    assert(core);
    pd_core_set_keyed(core, 1);
    uint32_t seed = 12345;
    long processed = 0;
    struct timespec begin, now;
    assert(!clock_gettime(CLOCK_MONOTONIC, &begin));
    for (;;) {
        double tx[2 * kBlock], rx[2 * kBlock], output[2 * kBlock];
        for (int k = 0; k < kBlock; ++k) {
            double t = (double)processed++ / kRate;
            tx[2 * k] = .35 * (cos(2 * M_PI * 710 * t) + cos(2 * M_PI * 1900 * t));
            tx[2 * k + 1] = .35 * (sin(2 * M_PI * 710 * t) + sin(2 * M_PI * 1900 * t));
            double power = tx[2 * k] * tx[2 * k] + tx[2 * k + 1] * tx[2 * k + 1];
            double gain = 1 - .12 * power, phase = .08 * power;
            rx[2 * k] = gain * (tx[2 * k] * cos(phase) - tx[2 * k + 1] * sin(phase)) + noise * uniform(&seed);
            rx[2 * k + 1] = gain * (tx[2 * k] * sin(phase) + tx[2 * k + 1] * cos(phase)) + noise * uniform(&seed);
        }
        pd_core_calibrate(core, tx, rx, kBlock);
        memcpy(output, tx, sizeof(output));
        pd_core_correct(core, output, kBlock);
        if (pd_core_fits(core) > 0) { break; }
        assert(!clock_gettime(CLOCK_MONOTONIC, &now));
        assert((now.tv_sec - begin.tv_sec) + (now.tv_nsec - begin.tv_nsec) / 1e9 < 20);
        struct timespec wait = {0, 1000000};
        nanosleep(&wait, NULL);
    }
    int flags = pd_core_fit_flags(core);
    printf("noise %.2f: fits %d, flags 0x%x, usable model %d\n",
           noise, pd_core_fits(core), flags, pd_core_current_solution(core));
    fflush(stdout);
    if (accepted) {
        assert(flags == 0 && pd_core_current_solution(core));
    } else {
        assert(!pd_core_current_solution(core));
        // Real WDSP rejects the noisy phase fit's sine/cosine identity.
        // Both reason fields must survive the wrapper's diagnostic packing.
        assert((flags & (0x10 << 16)) && (flags & (0x10 << 22)));
    }
    pd_core_set_keyed(core, 0);
    pd_core_destroy(core);
}

int main(void)
{
    alarm(50);
    check_fit(0, 1);
    check_fit(.03, 0);
    puts("PASS: clean fit is usable; rejected phase fits remain bypassed and expose both rejection reasons");
    return 0;
}
