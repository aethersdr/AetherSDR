#include "AetherGpsPackage.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QObject>
#include <QSaveFile>

namespace AetherSDR::aethergps {

namespace {

bool matchesPin(const QByteArray& bytes, const AetherGpsPackage::Pin& pin)
{
    return bytes.size() == pin.size
        && QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex() == QByteArray(pin.sha256);
}

}  // namespace

QString writeBundledPackage(const AetherGpsPackage::Pin& pin, const QString& dir, QString* error)
{
    const QString target = QDir(dir).filePath(QString::fromLatin1(pin.fileName));
    QFile existing(target);
    if (existing.open(QIODevice::ReadOnly) && existing.size() == pin.size
        && matchesPin(existing.readAll(), pin)) {
        return target;
    }
    existing.close();
    QFile bundled(QString::fromLatin1(pin.resource));
    const QByteArray bytes = bundled.open(QIODevice::ReadOnly) ? bundled.read(pin.size + 1) : QByteArray();
    if (!matchesPin(bytes, pin)) {
        if (error) {
            *error = QObject::tr("This AetherSDR's AetherGPS package is damaged, so nothing was installed.");
        }
        return {};
    }
    QSaveFile out(target);
    if (!QDir().mkpath(dir) || !out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size()
        || !out.commit()) {
        if (error) {
            *error = QObject::tr("Can't write the AetherGPS package to %1.").arg(QDir::toNativeSeparators(target));
        }
        return {};
    }
    return target;
}

}  // namespace AetherSDR::aethergps
