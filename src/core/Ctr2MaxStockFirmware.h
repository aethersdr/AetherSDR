#pragma once

#include "EspFirmwareManifest.h"

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>

namespace AetherSDR {

// The CTR2-Max's stock firmware, as Lynovation publishes it on lynovation.com:
// a zip holding one merged flash image (a .bin, with manuals and settings
// files beside it), or that .bin on its own. Lynovation's instructions write
// the whole image at 0x0; AetherSDR installs only its application into the
// unit's app0 and selects it to start, so the unit's bootloader, partition
// table, settings, registration and AetherCTR2 stay as they are.
//
// Lynovation's files are not signed, so a file is taken for what it shows
// itself to be: an intact ESP32-S3 application image (its appended SHA-256
// holds) carrying the CTR2-Max firmware's markers, from a merged image whose
// layout agrees with the unit's where the firmware relies on it. AetherSDR
// never fetches or distributes the file; the operator downloads it.
class Ctr2MaxStockFirmware {
public:
    // ESP-IDF's partition table, where the CTR2-Max has it.
    static constexpr quint32 kTableOffset = 0x8000;
    static constexpr quint32 kTableSize = 0xC00;

    struct Partition {
        QString name;
        quint8 type{0};       // 0 app, 1 data
        quint8 subtype{0};
        quint32 offset{0};
        quint32 size{0};
    };
    // The rows of a partition table, up to its end (or its MD5 row).
    static QList<Partition> readPartitionTable(const QByteArray& table);

    // The length of the ESP32-S3 image at the start of data, its appended
    // SHA-256 included, once that hash is checked; 0, with *problem, if it
    // is not one.
    static qint64 imageLength(const QByteArray& data, QString* problem);

    // A file as downloaded from lynovation.com: the zip, or the .bin in it.
    static bool read(const QByteArray& file, Ctr2MaxStockFirmware* out, QString* problem);

    QString version() const { return m_version; }   // "v1.01.03"; empty if not found
    // Whether version a is newer than b: "v<major>.<minor>.<patch>[letter]"
    // by number, a letter after none; a version not of that shape compares
    // as text, and an empty one is older than any.
    static bool newer(const QString& a, const QString& b);
    QByteArray app() const { return m_app; }
    // The whole flash image as published (the .bin), which read() takes back.
    QByteArray image() const { return m_image; }

    // The install, laid out by the unit's own partition table (kTableSize
    // bytes read at kTableOffset) and flash size: the application into app0,
    // and app0 selected, with every other region protected and the table
    // required to stay as it is. False, with *problem, when the unit's
    // layout does not agree with the file's where the firmware relies on it.
    bool plan(const QByteArray& unitTable, quint32 flashSize, EspFirmwareManifest* manifest,
              QHash<QString, QByteArray>* files, QString* problem) const;

    // Starting firmware already installed, laid out by the unit's table: the
    // stock firmware (app0) or AetherCTR2 (its slots after app0). Writes only
    // the boot selection; everything else is protected.
    static bool planStart(const QByteArray& unitTable, quint32 flashSize, bool aetherCtr2,
                          EspFirmwareManifest* manifest, QString* problem);

private:
    QString m_version;
    QByteArray m_app;
    QByteArray m_image;
    QList<Partition> m_table;   // the file's
};

} // namespace AetherSDR
