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

    return ok ? 0 : 1;
}
