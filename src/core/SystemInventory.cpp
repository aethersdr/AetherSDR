#include "SystemInventory.h"

#include <QLoggingCategory>
#include <QSysInfo>

#include <cstring>

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX  // guard: may already be defined by the build system (#4031)
#endif
#include <windows.h>
#elif defined(Q_OS_MACOS)
#include <sys/sysctl.h>
#include <sys/types.h>
#elif defined(Q_OS_LINUX)
#include <sys/sysinfo.h>
#endif

#if defined(Q_PROCESSOR_X86)
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif

// Declared QtWarningMsg like the other curated categories: the info tier is
// raised by LogManager::applyFilterRules() once the category is enabled
// (default-on via loadSettings), and the baseline-mismatch warning below
// passes regardless of the toggle.
Q_LOGGING_CATEGORY(lcSysInfo, "aether.sysinfo", QtWarningMsg)

namespace AetherSDR {

namespace {

#if defined(Q_PROCESSOR_X86)
void cpuidCount(quint32 leaf, quint32 subleaf, quint32 out[4])
{
#if defined(_MSC_VER)
    int regs[4] = {};
    __cpuidex(regs, static_cast<int>(leaf), static_cast<int>(subleaf));
    for (int i = 0; i < 4; ++i)
        out[i] = static_cast<quint32>(regs[i]);
#else
    unsigned int a = 0, b = 0, c = 0, d = 0;
    if (!__get_cpuid_count(leaf, subleaf, &a, &b, &c, &d)) {
        out[0] = out[1] = out[2] = out[3] = 0;
        return;
    }
    out[0] = a;
    out[1] = b;
    out[2] = c;
    out[3] = d;
#endif
}

QString cpuBrandString()
{
    // Leaves 0x80000002-4 spell the 48-byte brand string; guard on the
    // extended-leaf ceiling first (pre-2001 parts lack them).
    quint32 regs[4] = {};
    cpuidCount(0x80000000u, 0, regs);
    if (regs[0] < 0x80000004u)
        return {};
    char brand[49] = {};
    for (quint32 leaf = 0; leaf < 3; ++leaf) {
        cpuidCount(0x80000002u + leaf, 0, regs);
        memcpy(brand + leaf * 16, regs, 16);
    }
    return QString::fromLatin1(brand).simplified();
}
#endif // Q_PROCESSOR_X86

} // namespace

SystemInventory::CpuInfo SystemInventory::detectCpu()
{
    CpuInfo info;
    info.arch = QSysInfo::currentCpuArchitecture();
#if defined(Q_PROCESSOR_X86)
    info.x86 = true;
    info.brand = cpuBrandString();
    quint32 regs[4] = {};
    // Leaf 1 ECX: SSE4.2 bit 20, FMA bit 12, AVX bit 28, F16C bit 29. These
    // are CPU capability bits; OS XSAVE state is assumed (universal on the
    // OS versions Qt 6 supports).
    cpuidCount(1, 0, regs);
    info.sse42 = (regs[2] >> 20) & 1u;
    info.fma   = (regs[2] >> 12) & 1u;
    info.avx   = (regs[2] >> 28) & 1u;
    info.f16c  = (regs[2] >> 29) & 1u;
    // Leaf 7.0 EBX: AVX2 bit 5, BMI2 bit 8 (leaf 7 absent → zeros).
    cpuidCount(7, 0, regs);
    info.avx2 = (regs[1] >> 5) & 1u;
    info.bmi2 = (regs[1] >> 8) & 1u;
#endif
    return info;
}

quint64 SystemInventory::totalRamBytes()
{
#if defined(Q_OS_WIN)
    MEMORYSTATUSEX status;
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status))
        return status.ullTotalPhys;
    return 0;
#elif defined(Q_OS_MACOS)
    quint64 bytes = 0;
    size_t len = sizeof(bytes);
    if (sysctlbyname("hw.memsize", &bytes, &len, nullptr, 0) == 0)
        return bytes;
    return 0;
#elif defined(Q_OS_LINUX)
    struct sysinfo info = {};
    if (sysinfo(&info) == 0)
        return quint64(info.totalram) * info.mem_unit;
    return 0;
#else
    return 0;
#endif
}

QStringList SystemInventory::presentFeatures(const CpuInfo& cpu)
{
    if (!cpu.x86)
        return {};
    QStringList features;
    if (cpu.sse42) features << QStringLiteral("SSE42");
    if (cpu.avx)   features << QStringLiteral("AVX");
    if (cpu.avx2)  features << QStringLiteral("AVX2");
    if (cpu.fma)   features << QStringLiteral("FMA");
    if (cpu.f16c)  features << QStringLiteral("F16C");
    if (cpu.bmi2)  features << QStringLiteral("BMI2");
    return features;
}

QStringList SystemInventory::missingCpuFeatures(const CpuInfo& cpu, const QString& baseline)
{
    if (!cpu.x86 || baseline.trimmed().isEmpty())
        return {};
    const QStringList have = presentFeatures(cpu);
    QStringList missing;
    const QStringList required = baseline.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString& tokenRaw : required) {
        const QString token = tokenRaw.trimmed().toUpper();
        if (token.isEmpty())
            continue;
        if (!have.contains(token))
            missing << token;
    }
    return missing;
}

QString SystemInventory::compiledGgmlBaseline()
{
#ifdef AETHER_GGML_CPU_BASELINE
    return QStringLiteral(AETHER_GGML_CPU_BASELINE);
#else
    return {};
#endif
}

void SystemInventory::logSystemInventory()
{
    const CpuInfo cpu = detectCpu();
    const quint64 ramMb = totalRamBytes() / (1024 * 1024);

    qCInfo(lcSysInfo).noquote()
        << "OS:" << QSysInfo::prettyProductName()
        << "kernel" << QSysInfo::kernelVersion()
        << "arch" << cpu.arch;
    qCInfo(lcSysInfo).noquote()
        << "CPU:" << (cpu.brand.isEmpty() ? cpu.arch : cpu.brand)
        << (cpu.x86 ? QStringLiteral("features: %1").arg(
                          presentFeatures(cpu).join(QLatin1Char(' ')))
                    : QString());
    qCInfo(lcSysInfo).noquote() << "RAM:" << ramMb << "MB";

    const QString baseline = compiledGgmlBaseline();
    qCInfo(lcSysInfo).noquote()
        << "Speech engine CPU baseline:"
        << (baseline.isEmpty() ? QStringLiteral("unknown (system libwhisper)")
                               : baseline);

    const QStringList missing = missingCpuFeatures(cpu, baseline);
    if (!missing.isEmpty()) {
        // Warning tier deliberately: "this binary's speech engine cannot run
        // on this CPU" must reach the log even with the category toggled off.
        qCWarning(lcSysInfo).noquote()
            << "CPU lacks features the bundled speech engine requires:"
            << missing.join(QLatin1Char(' '))
            << "- enabling Copy Assist (ASR) would fault on this machine (#4986)";
    }
}

QString SystemInventory::cpuSummary()
{
    const CpuInfo cpu = detectCpu();
    const QString name = cpu.brand.isEmpty() ? cpu.arch : cpu.brand;
    if (!cpu.x86)
        return name;
    const QStringList features = presentFeatures(cpu);
    return QStringLiteral("%1 (%2; %3)")
        .arg(name, cpu.arch,
             features.isEmpty() ? QStringLiteral("no SIMD features detected")
                                : features.join(QLatin1Char(' ')));
}

QString SystemInventory::ramSummary()
{
    return QStringLiteral("%1 MB").arg(totalRamBytes() / (1024 * 1024));
}

} // namespace AetherSDR
