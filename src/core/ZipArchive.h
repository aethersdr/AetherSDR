#pragma once

#include <QByteArray>
#include <QList>
#include <QMap>
#include <QString>
#include <QtGlobal>

namespace AetherSDR {

struct ZipEntryData {
    QString name;
    QByteArray data;
};

struct ZipReadLimits {
    qsizetype maxEntries{4096};
    quint64 maxEntryUncompressedBytes{256ULL * 1024ULL * 1024ULL};
    quint64 maxTotalUncompressedBytes{512ULL * 1024ULL * 1024ULL};
};

QMap<QString, QByteArray> readZipEntries(const QByteArray& zip, QString* error);
QMap<QString, QByteArray> readZipEntries(const QByteArray& zip,
                                         QString* error,
                                         const ZipReadLimits& limits);
// Stored (uncompressed, method 0). Fine for small payloads.
QByteArray writeStoredZip(const QList<ZipEntryData>& entries);
// Raw-deflate (method 8) — same bundled zlib as the read path; 5–10x smaller
// for log-heavy archives like support bundles (#3218).
QByteArray writeDeflatedZip(const QList<ZipEntryData>& entries);

} // namespace AetherSDR
