// The radio health agent (tools/radio-health): the package that ships in
// AetherSDR's resources matches its pin, is a valid legacy waveform and is
// written out only when it does; its JSON line parses, anything else is
// refused, values out of range are dropped, and the summary leaves out what
// the agent couldn't measure. Socket-free; files only in a temporary dir.

#include "core/RadioHealthClient.h"
#include "core/LegacyWaveformPackage.h"
#include "core/RadioHealthPackage.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QLocale>
#include <QTemporaryDir>

#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void check(bool ok, const char* label)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", label);
    if (!ok) {
        ++g_failed;
    }
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QLocale::setDefault(QLocale::c());

    // A real FLEX-8600 reply (2026-11-01).
    const QByteArray flex8600 = R"({"agent":"0.1.0","cpu_idle_pct":90,"cpu_cores":4,)"
        R"("mem_total_kb":1944564,"mem_available_kb":1680432,"mem_estimated":false,)"
        R"("storage":[{"path":"/var/lib/smartsdr/containers","free_bytes":824102912,)"
        R"("total_bytes":1022013440},{"path":"/var/lib/smartsdr/waveforms/AetherHealth",)"
        R"("free_bytes":824102912,"total_bytes":1022013440}]})";
    const auto h = radiohealth::parse(flex8600);
    check(h.has_value(), "a FLEX-8600 reply parses");
    if (h) {
        check(h->agentVersion == QLatin1String("0.1.0") && h->cpuIdlePct == 90
                  && h->cpuCores == 4 && h->memAvailableKb == 1680432 && !h->memEstimated,
              "fields read");
        check(h->storage.size() == 2
                  && h->storage.first().path == QLatin1String("/var/lib/smartsdr/containers")
                  && h->storage.first().freeBytes == 824102912ULL,
              "storage in the agent's order, container store first");
        check(radiohealth::summary(*h)
                  == QStringLiteral("Storage 824 MB free · Memory 1.7 GB free · CPU 90% idle"),
              "summary");
        check(radiohealth::storageSummary(*h) == QStringLiteral("Storage 824 MB free")
                  && radiohealth::resourceSummary(*h)
                         == QStringLiteral("Memory 1.7 GB free · CPU 90% idle"),
              "storage and memory/CPU parts for the one-row bar");
    }

    // A FLEX-6000 (2.6.37: estimated memory) before its second CPU sample.
    const auto six = radiohealth::parse(
        R"({"agent":"0.1.0","cpu_idle_pct":-1,"cpu_cores":1,"mem_total_kb":480512,)"
        R"("mem_available_kb":270000,"mem_estimated":true,)"
        R"("storage":[{"path":"/user/waveforms/AetherHealth","free_bytes":6815744,"total_bytes":19922944}]})");
    check(six && six->cpuIdlePct == -1 && six->memEstimated, "6000 reply parses");
    if (six) {
        check(radiohealth::summary(*six)
                  == QStringLiteral("Storage 7 MB free · Memory about 270.0 MB free"),
              "unknown CPU left out; estimated memory says about");
    }

    check(!radiohealth::parse("not json"), "garbage refused");
    check(!radiohealth::parse(R"({"storage":[]})"), "no agent version refused");
    check(!radiohealth::parse(R"({"agent":"0.1.0"})"), "no storage list refused");
    check(!radiohealth::parse("[1,2]"), "a JSON array refused");

    const auto odd = radiohealth::parse(
        R"({"agent":"0.1.0","cpu_idle_pct":250,"cpu_cores":-3,"storage":[)"
        R"({"path":"a","free_bytes":10,"total_bytes":5},)"
        R"({"path":"b","free_bytes":-1,"total_bytes":5},)"
        R"({"path":"c","free_bytes":1,"total_bytes":2},{"path":"d","free_bytes":1,"total_bytes":2},)"
        R"({"path":"e","free_bytes":1,"total_bytes":2},{"path":"f","free_bytes":1,"total_bytes":2},)"
        R"({"path":"g","free_bytes":1,"total_bytes":2}]})");
    check(odd && odd->cpuIdlePct == -1 && odd->cpuCores == 0, "out-of-range CPU values dropped");
    check(odd && odd->storage.size() == 4 && odd->storage.first().path == QLatin1String("c"),
          "impossible volumes dropped, at most four kept");
    check(odd && odd->memAvailableKb == -1 && radiohealth::summary(*odd).startsWith("Storage"),
          "missing memory left out of the summary");
    const auto noVolumes = radiohealth::parse(R"({"agent":"0.1.0","cpu_idle_pct":5,"storage":[]})");
    check(noVolumes && radiohealth::storageSummary(*noVolumes).isEmpty()
              && radiohealth::summary(*noVolumes) == QStringLiteral("CPU 5% idle"),
          "no readable volume: storage left out");


    check(isRadioHealthWaveform(QStringLiteral("AetherHealth"), false),
          "the agent's legacy row is recognized");
    check(!isRadioHealthWaveform(QStringLiteral("AetherHealth"), true)
              && !isRadioHealthWaveform(QStringLiteral("AetherTailnet"), false),
          "a container of that name, or another waveform, is not");
    // The bundled package.
    {
        QTemporaryDir dir;
        QString error;
        const QString path = radiohealth::writeBundledPackage(dir.path(), &error);
        QFile f(path);
        const QByteArray bytes = f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
        check(!path.isEmpty() && bytes.size() == RadioHealthPackage::kSize
                  && QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()
                         == QByteArray(RadioHealthPackage::kSha256),
              "the bundled package is written and matches its pin");
        f.close();
        const LegacyWaveformPackageInfo info = inspectLegacyWaveformPackageFile(path);
        check(info.valid && info.waveformName == QLatin1String(kRadioHealthWaveformName)
                  && info.version == QLatin1String(RadioHealthPackage::kVersion),
              "it is a valid legacy waveform named AetherHealth, at the pinned version");

        // A damaged copy left on disk is replaced, not installed.
        QFile damaged(path);
        damaged.open(QIODevice::WriteOnly);
        damaged.write("not the package");
        damaged.close();
        const QString again = radiohealth::writeBundledPackage(dir.path(), &error);
        QFile g(again);
        check(again == path && g.open(QIODevice::ReadOnly) && g.size() == RadioHealthPackage::kSize,
              "a damaged copy on disk is rewritten from the bundle");

        // Bytes that don't match the pin are never written.
        QTemporaryDir other;
        QString why;
        const QString none = radiohealth::writeBundledPackage(
            other.path(), &why, QStringLiteral(":/cty.dat"));
        check(none.isEmpty() && !why.isEmpty()
                  && !QFile::exists(QDir(other.path()).filePath(QStringLiteral("AetherHealth.ssdr_waveform"))),
              "a resource that doesn't match the pin is refused and nothing is written");
    }

    std::printf(g_failed ? "\n%d radio-health tests failed.\n" : "\nAll radio-health tests passed.\n",
                g_failed);
    return g_failed ? 1 : 0;
}
