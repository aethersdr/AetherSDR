#pragma once

#include <QString>
#include <QStringList>

namespace AetherSDR {

// Startup hardware/capability inventory (#4986). Collects the facts that
// decide hardware-dependent crash reports — CPU model + SIMD features, total
// RAM — and compares the CPU against the ISA baseline the vendored ggml was
// compiled with, so "this binary cannot run on this CPU" appears in the log
// instead of only as an illegal-instruction fault (#4509).
//
// Deliberately free of ggml/whisper includes: on a CPU below the baseline,
// executing any ggml code IS the crash, so the inventory must be able to run
// and log before that code is ever entered.
class SystemInventory {
public:
    struct CpuInfo {
        QString arch;   // QSysInfo::currentCpuArchitecture()
        QString brand;  // cpuid brand string; empty on non-x86
        bool x86 = false;  // feature flags below are meaningful only when true
        bool sse42 = false;
        bool avx = false;
        bool avx2 = false;
        bool fma = false;
        bool f16c = false;
        bool bmi2 = false;
    };

    // cpuid-based detection; on non-x86 hosts returns arch with x86=false and
    // all feature flags false.
    static CpuInfo detectCpu();

    static quint64 totalRamBytes();

    // Feature tokens `cpu` provides, in ggml's option spelling
    // (SSE42/AVX/AVX2/FMA/F16C/BMI2). Empty on non-x86.
    static QStringList presentFeatures(const CpuInfo& cpu);

    // Tokens named in `baseline` (comma-separated, as CMake bakes into
    // AETHER_GGML_CPU_BASELINE) that `cpu` does not provide. Empty when the
    // baseline is empty/unknown or when `cpu` is not x86 (non-x86 baselines
    // are not expressed in this scheme). Unknown tokens are reported missing:
    // a baseline this code cannot vouch for must not read as satisfied.
    static QStringList missingCpuFeatures(const CpuInfo& cpu, const QString& baseline);

    // The baseline compiled into this binary, or empty when unknown
    // (system-libwhisper builds).
    static QString compiledGgmlBaseline();

    // Emit the one-per-launch inventory block under aether.sysinfo, plus a
    // warning naming any baseline features the CPU lacks. Caller flushes the
    // log afterwards so the block is on disk before startup continues.
    static void logSystemInventory();

    // One-line summaries for SupportBundle / IssueReport.
    static QString cpuSummary();  // "<brand> (<arch>; SSE42 AVX AVX2 ...)"
    static QString ramSummary();  // "<N> MB"
};

} // namespace AetherSDR
