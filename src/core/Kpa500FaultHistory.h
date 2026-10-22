#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>
#include <QtTypes>

namespace AetherSDR::Kpa500 {

struct FaultEntry {
    int slot = 0;
    quint32 uptimeSeconds = 0;
    int code = 0;
    QString name;
    QString band;
    quint16 data = 0;
    QString description;
};

// Decode the complete 1 KiB EEPROM snapshot, newest entry first. Invalid
// snapshots return no entries and set error; an empty valid ring clears error.
QVector<FaultEntry> decodeFaultHistory(const QByteArray& image, QString* error = nullptr);

} // namespace AetherSDR::Kpa500
