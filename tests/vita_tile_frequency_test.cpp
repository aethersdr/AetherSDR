// Unit tests for VITA-49 waterfall-tile frequency decoding.
//
// Regression coverage for the 1 GHz ceiling that blacked out the transverter
// waterfall above 1 GHz (#3449, #1843, #1928, #2835). The decoder must pick the
// correct encoding (VitaFrequency vs plain Hz) from the raw integer magnitude
// with no upper frequency limit.

#include "core/VitaTileFrequency.h"

#include <QtGlobal>

#include <cmath>
#include <cstdio>

using AetherSDR::Vita::decodeTileFrequencyMhz;

namespace {

int g_failures = 0;

void expectNear(const char* what, double got, double want, double tolMhz)
{
    if (std::abs(got - want) > tolMhz) {
        std::fprintf(stderr, "FAIL: %s — got %.9f MHz, want %.9f MHz (tol %.9f)\n",
                     what, got, want, tolMhz);
        ++g_failures;
    } else {
        std::fprintf(stderr, "ok:   %s = %.6f MHz\n", what, got);
    }
}

// VitaFrequency raw value (Hz × 2^20) for a frequency in MHz.
qint64 vitaRaw(double mhz)
{
    return static_cast<qint64>(std::llround(mhz * 1e6 * 1048576.0));
}

// Plain-Hz raw value for a frequency in MHz.
qint64 hzRaw(double mhz)
{
    return static_cast<qint64>(std::llround(mhz * 1e6));
}

} // namespace

int main()
{
    // ── VitaFrequency encoding ───────────────────────────────────────────────
    // HF tile (well below 1 GHz) must decode correctly.
    {
        const auto f = decodeTileFrequencyMhz(vitaRaw(14.0), vitaRaw(0.001));
        expectNear("vita 14 MHz low", f.lowMhz, 14.0, 1e-6);
        expectNear("vita 14 MHz binBw", f.binBwMhz, 0.001, 1e-9);
    }

    // IF-domain transverter tile (~28 MHz) — the classic XVTR case.
    {
        const auto f = decodeTileFrequencyMhz(vitaRaw(28.0), vitaRaw(0.002));
        expectNear("vita 28 MHz IF low", f.lowMhz, 28.0, 1e-6);
    }

    // THE regression: a real ~1 GHz RF-domain tile must NOT be re-read as plain
    // Hz. The reporter in #3449 saw the break between 1000.015 and 1000.016 MHz;
    // both must decode to ~1000 MHz, not ~1.05e9 MHz.
    {
        const auto ok   = decodeTileFrequencyMhz(vitaRaw(1000.015), vitaRaw(0.003));
        const auto fail = decodeTileFrequencyMhz(vitaRaw(1000.016), vitaRaw(0.003));
        expectNear("vita 1000.015 MHz low", ok.lowMhz, 1000.015, 1e-3);
        expectNear("vita 1000.016 MHz low", fail.lowMhz, 1000.016, 1e-3);
    }

    // 1296 MHz (23 cm), 2304 MHz (2.3 GHz terrestrial), 2400 MHz (sat).
    {
        expectNear("vita 1296 MHz", decodeTileFrequencyMhz(vitaRaw(1296.0), 0).lowMhz,
                   1296.0, 1e-2);
        expectNear("vita 2304 MHz", decodeTileFrequencyMhz(vitaRaw(2304.1), 0).lowMhz,
                   2304.1, 1e-2);
        expectNear("vita 2400 MHz", decodeTileFrequencyMhz(vitaRaw(2400.0), 0).lowMhz,
                   2400.0, 1e-2);
    }

    // 2200 m (135 kHz) — lowest band; VitaFrequency raw still lands above the
    // disambiguation threshold, so it must decode as VitaFrequency.
    {
        const auto f = decodeTileFrequencyMhz(vitaRaw(0.1357), vitaRaw(0.00001));
        expectNear("vita 2200m low", f.lowMhz, 0.1357, 1e-4);
    }

    // ── Plain-Hz encoding ────────────────────────────────────────────────────
    // Radios/firmware that send plain Hz must still decode correctly across the
    // same range (the old code claimed to support this but had latent gaps).
    {
        expectNear("hz 14 MHz", decodeTileFrequencyMhz(hzRaw(14.0), hzRaw(0.001)).lowMhz,
                   14.0, 1e-6);
        expectNear("hz 1296 MHz", decodeTileFrequencyMhz(hzRaw(1296.0), 0).lowMhz,
                   1296.0, 1e-6);
        expectNear("hz 2400 MHz", decodeTileFrequencyMhz(hzRaw(2400.0), 0).lowMhz,
                   2400.0, 1e-6);
        const auto bw = decodeTileFrequencyMhz(hzRaw(2400.0), hzRaw(0.005));
        expectNear("hz binBw shares encoding", bw.binBwMhz, 0.005, 1e-9);
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "\nAll vita_tile_frequency_test checks passed.\n");
        return 0;
    }
    std::fprintf(stderr, "\n%d vita_tile_frequency_test check(s) FAILED.\n", g_failures);
    return 1;
}
