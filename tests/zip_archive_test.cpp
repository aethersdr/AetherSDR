#include "core/ZipArchive.h"

#include <QByteArray>
#include <QMap>
#include <QString>

#include <iostream>

namespace {

bool expect(bool condition, const char* message)
{
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

quint16 readLe16(const QByteArray& bytes, qsizetype offset)
{
    const auto* p = reinterpret_cast<const uchar*>(bytes.constData() + offset);
    return quint16(p[0]) | (quint16(p[1]) << 8);
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

    const QList<AetherSDR::ZipEntryData> entries = {
        {QStringLiteral("aethersdr.log"), QByteArray("log line\n")},
        {QStringLiteral("system-info.json"), QByteArray("{\"os\":\"test\"}\n")},
        {QStringLiteral("empty.txt"), QByteArray()},
    };

    const QByteArray zip = AetherSDR::writeStoredZip(entries);
    ok &= expect(zip.startsWith("PK"), "stored ZIP has ZIP magic");
    ok &= expect(readLe16(zip, 8) == 0, "stored ZIP local header uses method 0");

    QString error;
    const QMap<QString, QByteArray> roundTrip = AetherSDR::readZipEntries(zip, &error);
    ok &= expect(error.isEmpty(), "stored ZIP round-trip has no read error");
    ok &= expect(roundTrip.size() == entries.size(), "stored ZIP round-trip preserves entry count");
    ok &= expect(roundTrip.value(QStringLiteral("aethersdr.log")) == QByteArray("log line\n"),
                 "stored ZIP round-trip preserves log data");
    ok &= expect(roundTrip.value(QStringLiteral("system-info.json")) == QByteArray("{\"os\":\"test\"}\n"),
                 "stored ZIP round-trip preserves JSON data");
    ok &= expect(roundTrip.contains(QStringLiteral("empty.txt"))
                 && roundTrip.value(QStringLiteral("empty.txt")).isEmpty(),
                 "stored ZIP round-trip preserves empty files");

    QByteArray corrupt = zip;
    const int dataOffset = 30 + QByteArray("aethersdr.log").size();
    corrupt[dataOffset] = corrupt.at(dataOffset) == 'x' ? 'y' : 'x';
    error.clear();
    ok &= expect(AetherSDR::readZipEntries(corrupt, &error).isEmpty()
                 && error == QStringLiteral("ZIP entry checksum mismatch."),
                 "stored ZIP reader rejects corrupt entry data");

    // --- Deflated ZIP (#3218): method 8, smaller, and round-trips via the
    //     existing inflate path (writer/reader cross-check). ---
    QByteArray bigLog;
    for (int i = 0; i < 500; ++i)
        bigLog += "2026-06-21 12:00:00 INF a repetitive, highly compressible log line\n";
    const QList<AetherSDR::ZipEntryData> dEntries = {
        {QStringLiteral("aethersdr.log"), bigLog},
        {QStringLiteral("system-info.json"), QByteArray("{\"os\":\"test\"}\n")},
        {QStringLiteral("empty.txt"), QByteArray()},
    };

    const QByteArray dzip = AetherSDR::writeDeflatedZip(dEntries);
    ok &= expect(dzip.startsWith("PK"), "deflated ZIP has ZIP magic");
    ok &= expect(readLe16(dzip, 8) == 8, "deflated ZIP local header uses method 8");
    ok &= expect(dzip.size() < bigLog.size(),
                 "deflated ZIP is smaller than the raw log it contains");

    QString derror;
    const QMap<QString, QByteArray> dRoundTrip = AetherSDR::readZipEntries(dzip, &derror);
    ok &= expect(derror.isEmpty(), "deflated ZIP round-trip has no read error");
    ok &= expect(dRoundTrip.size() == dEntries.size(),
                 "deflated ZIP round-trip preserves entry count");
    ok &= expect(dRoundTrip.value(QStringLiteral("aethersdr.log")) == bigLog,
                 "deflated ZIP round-trip preserves the (large) log data");
    ok &= expect(dRoundTrip.value(QStringLiteral("system-info.json")) == QByteArray("{\"os\":\"test\"}\n"),
                 "deflated ZIP round-trip preserves JSON data");
    ok &= expect(dRoundTrip.contains(QStringLiteral("empty.txt"))
                 && dRoundTrip.value(QStringLiteral("empty.txt")).isEmpty(),
                 "deflated ZIP round-trip preserves empty files");

    {
        QByteArray oversized = AetherSDR::writeDeflatedZip({
            {QStringLiteral("small.txt"), QByteArray("small")},
        });
        const qsizetype central = oversized.indexOf(QByteArray::fromHex("504b0102"));
        ok &= expect(central >= 0, "oversized-entry fixture has central directory");
        if (central >= 0) {
            writeLe32(oversized, central + 24, 128U * 1024U * 1024U);
            AetherSDR::ZipReadLimits limits;
            limits.maxEntryUncompressedBytes = 1024;
            limits.maxTotalUncompressedBytes = 2048;
            QString limitError;
            ok &= expect(AetherSDR::readZipEntries(oversized, &limitError, limits).isEmpty()
                         && limitError.contains(QStringLiteral("safety limit")),
                         "reader rejects declared expansion before allocating it");
        }
    }

    {
        const QByteArray duplicates = AetherSDR::writeStoredZip({
            {QStringLiteral("same.txt"), QByteArray("first")},
            {QStringLiteral("same.txt"), QByteArray("second")},
        });
        QString duplicateError;
        ok &= expect(AetherSDR::readZipEntries(duplicates, &duplicateError).isEmpty()
                     && duplicateError.contains(QStringLiteral("duplicate")),
                     "reader rejects duplicate entry names");
    }

    {
        AetherSDR::ZipReadLimits limits;
        limits.maxEntries = 1;
        QString countError;
        ok &= expect(AetherSDR::readZipEntries(zip, &countError, limits).isEmpty()
                     && countError.contains(QStringLiteral("too many entries")),
                     "reader enforces the entry-count limit");
    }

    return ok ? 0 : 1;
}
