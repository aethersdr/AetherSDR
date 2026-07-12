#include "core/LegacyWaveformPackage.h"
#include "core/ZipArchive.h"

#include <QByteArray>
#include <QList>
#include <QString>

#include <iostream>

namespace {

bool expect(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

QByteArray validPackage()
{
    const QByteArray cfg =
        "[header]\n"
        "Name: ExampleLegacy\n"
        "Version: \"1.2.3\"\n"
        "Executable: \"examplewaveform\"\n"
        "\n"
        "[setup]\n"
        "waveform create name=ExampleLegacy mode=TEST underlying_mode=DFM version=1.2.3\n";
    return AetherSDR::writeDeflatedZip({
        {QStringLiteral("ExampleLegacy/ExampleLegacy.cfg"), cfg},
        {QStringLiteral("ExampleLegacy/examplewaveform"), QByteArray("ELF test payload")},
    });
}

void writeLe32(QByteArray& bytes, qsizetype offset, quint32 value)
{
    bytes[offset] = static_cast<char>(value & 0xffU);
    bytes[offset + 1] = static_cast<char>((value >> 8U) & 0xffU);
    bytes[offset + 2] = static_cast<char>((value >> 16U) & 0xffU);
    bytes[offset + 3] = static_cast<char>((value >> 24U) & 0xffU);
}

} // namespace

int main()
{
    bool ok = true;

    {
        const AetherSDR::LegacyWaveformPackageInfo info =
            AetherSDR::inspectLegacyWaveformPackage(validPackage());
        ok &= expect(info.valid, "valid package accepted");
        ok &= expect(info.waveformName == QStringLiteral("ExampleLegacy"),
                     "waveform name parsed");
        ok &= expect(info.version == QStringLiteral("1.2.3"),
                     "version parsed without quotes");
        ok &= expect(info.rootDirectory == QStringLiteral("ExampleLegacy"),
                     "root directory parsed");
        ok &= expect(info.configPath == QStringLiteral("ExampleLegacy/ExampleLegacy.cfg"),
                     "config path parsed");
        ok &= expect(info.executablePath == QStringLiteral("ExampleLegacy/examplewaveform"),
                     "executable path resolved");
    }

    {
        const QByteArray missingExe = AetherSDR::writeStoredZip({
            {QStringLiteral("Probe/Probe.cfg"),
             QByteArray("[header]\nName: Probe\nExecutable: \"probe\"\n")},
        });
        const AetherSDR::LegacyWaveformPackageInfo info =
            AetherSDR::inspectLegacyWaveformPackage(missingExe);
        ok &= expect(!info.valid, "missing executable rejected");
        ok &= expect(info.error.contains(QStringLiteral("executable")),
                     "missing executable error mentions executable");
    }

    {
        const QByteArray unsafePath = AetherSDR::writeStoredZip({
            {QStringLiteral("../Probe/Probe.cfg"),
             QByteArray("[header]\nName: Probe\nExecutable: \"probe\"\n")},
            {QStringLiteral("Probe/probe"), QByteArray("x")},
        });
        const AetherSDR::LegacyWaveformPackageInfo info =
            AetherSDR::inspectLegacyWaveformPackage(unsafePath);
        ok &= expect(!info.valid, "unsafe path rejected");
    }

    {
        const AetherSDR::LegacyWaveformPackageInfo info =
            AetherSDR::inspectLegacyWaveformPackage(QByteArray("not a zip"));
        ok &= expect(!info.valid, "non-zip package rejected");
    }

    {
        QByteArray expansionBomb = validPackage();
        const qsizetype central = expansionBomb.indexOf(QByteArray::fromHex("504b0102"));
        ok &= expect(central >= 0, "legacy expansion fixture has central directory");
        if (central >= 0) {
            writeLe32(expansionBomb, central + 24, 128U * 1024U * 1024U);
            const AetherSDR::LegacyWaveformPackageInfo info =
                AetherSDR::inspectLegacyWaveformPackage(expansionBomb);
            ok &= expect(!info.valid
                         && info.error.contains(QStringLiteral("safety limit")),
                         "legacy inspector rejects excessive declared expansion");
        }
    }

    return ok ? 0 : 1;
}
