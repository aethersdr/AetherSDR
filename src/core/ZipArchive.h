#pragma once

#include <QByteArray>
#include <QList>
#include <QMap>
#include <QString>

namespace AetherSDR {

struct ZipEntryData {
    QString name;
    QByteArray data;
};

QMap<QString, QByteArray> readZipEntries(const QByteArray& zip, QString* error);
QByteArray writeStoredZip(const QList<ZipEntryData>& entries);

} // namespace AetherSDR
