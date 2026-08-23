// Contract tests for the startup hardware inventory (#4986).
//
// The detection half (cpuid, RAM syscalls) is measured on whatever machine
// runs the suite, so these tests pin the *contracts* that don't depend on the
// host: the baseline-comparison logic that decides the "this binary cannot
// run on this CPU" warning (the #4509 class), and the self-consistency of
// detection on the running host (present features never contradict the
// feature flags). The host is deliberately NOT checked against the compiled
// baseline: this test links no ggml objects and carries no ISA flags, so it
// runs fine on a below-baseline CPU where that check would fail for the lab,
// not the code.

#include "core/SystemInventory.h"

#include <QtGlobal>
#include <cstdio>
#include <cstdlib>

using AetherSDR::SystemInventory;

static int g_failures = 0;

#define CHECK(cond, what)                                                     \
    do {                                                                      \
        if (cond) {                                                           \
            std::printf("[ok] %s\n", what);                                   \
        } else {                                                              \
            std::printf("[FAIL] %s (line %d)\n", what, __LINE__);             \
            ++g_failures;                                                     \
        }                                                                     \
    } while (false)

static SystemInventory::CpuInfo phenomLikeCpu()
{
    // The #4509 machine class: x86-64 with SSE4.2-era features and no AVX of
    // any kind (AMD Phenom II).
    SystemInventory::CpuInfo cpu;
    cpu.arch = QStringLiteral("x86_64");
    cpu.brand = QStringLiteral("AMD Phenom(tm) II X6 1055T Processor");
    cpu.x86 = true;
    cpu.sse42 = false;  // Phenom II is SSE4a, not SSE4.2
    return cpu;
}

static SystemInventory::CpuInfo modernCpu()
{
    SystemInventory::CpuInfo cpu;
    cpu.arch = QStringLiteral("x86_64");
    cpu.brand = QStringLiteral("Test Modern CPU");
    cpu.x86 = true;
    cpu.sse42 = cpu.avx = cpu.avx2 = cpu.fma = cpu.f16c = cpu.bmi2 = true;
    return cpu;
}

int main()
{
    const QString fullBaseline = QStringLiteral("SSE42,AVX,AVX2,FMA,F16C,BMI2");

    // A CPU with everything satisfies the full baseline.
    CHECK(SystemInventory::missingCpuFeatures(modernCpu(), fullBaseline).isEmpty(),
          "modern CPU satisfies the full baseline");

    // The #4509 machine reports every baseline feature missing — this list
    // becoming non-empty is exactly what arms the startup warning.
    {
        const QStringList missing =
            SystemInventory::missingCpuFeatures(phenomLikeCpu(), fullBaseline);
        CHECK(missing.size() == 6, "no-AVX CPU misses all six baseline features");
        CHECK(missing.contains(QStringLiteral("AVX2")),
              "AVX2 is named among the missing features");
    }

    // Partial coverage: only the genuinely absent features are named, in
    // baseline order.
    {
        SystemInventory::CpuInfo cpu = modernCpu();
        cpu.avx2 = false;
        cpu.bmi2 = false;
        const QStringList missing =
            SystemInventory::missingCpuFeatures(cpu, fullBaseline);
        CHECK(missing == QStringList({QStringLiteral("AVX2"), QStringLiteral("BMI2")}),
              "only absent features are reported, in baseline order");
    }

    // Empty/unknown baseline (system-libwhisper builds) never warns.
    CHECK(SystemInventory::missingCpuFeatures(phenomLikeCpu(), QString()).isEmpty(),
          "empty baseline yields no missing features");
    CHECK(SystemInventory::missingCpuFeatures(phenomLikeCpu(), QStringLiteral("  ")).isEmpty(),
          "blank baseline yields no missing features");

    // Non-x86 hosts are out of scope for the x86 token scheme.
    {
        SystemInventory::CpuInfo arm;
        arm.arch = QStringLiteral("arm64");
        arm.x86 = false;
        CHECK(SystemInventory::missingCpuFeatures(arm, fullBaseline).isEmpty(),
              "non-x86 CPU is exempt from the x86 baseline");
    }

    // Tokens this code cannot vouch for must read as missing, not satisfied —
    // a future baseline (e.g. AVX512F) must not silently pass.
    CHECK(SystemInventory::missingCpuFeatures(modernCpu(), QStringLiteral("AVX512F"))
              == QStringList({QStringLiteral("AVX512F")}),
          "unknown baseline token reads as missing");

    // Tolerant parsing: whitespace and case from a hand-edited define.
    CHECK(SystemInventory::missingCpuFeatures(phenomLikeCpu(),
                                              QStringLiteral(" avx2 , fma "))
              == QStringList({QStringLiteral("AVX2"), QStringLiteral("FMA")}),
          "baseline tokens are trimmed and case-folded");

    // presentFeatures mirrors the flags exactly.
    {
        SystemInventory::CpuInfo cpu = phenomLikeCpu();
        cpu.sse42 = true;
        CHECK(SystemInventory::presentFeatures(cpu)
                  == QStringList({QStringLiteral("SSE42")}),
              "presentFeatures lists exactly the set flags");
        CHECK(SystemInventory::presentFeatures(modernCpu()).size() == 6,
              "presentFeatures lists all six when all are set");
    }

    // Host self-consistency of detection (not of the baseline — see header).
    {
        const SystemInventory::CpuInfo host = SystemInventory::detectCpu();
        CHECK(!host.arch.isEmpty(), "host arch detected");
        // presentFeatures agrees with the individual flags on the real host.
        const QStringList present = SystemInventory::presentFeatures(host);
        CHECK(present.contains(QStringLiteral("AVX2")) == host.avx2,
              "presentFeatures agrees with the host avx2 flag");
    }

    // RAM detection returns something plausible on every supported platform:
    // nonzero, and at least 256 MB (no machine that runs Qt 6 has less).
    {
        const quint64 ram = SystemInventory::totalRamBytes();
        CHECK(ram >= 256ull * 1024 * 1024, "total RAM detected and plausible");
    }

    if (g_failures != 0) {
        std::printf("%d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("all ok\n");
    return EXIT_SUCCESS;
}
