#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

namespace AetherSDR {

// What a firmware publisher tells AetherSDR about installing its firmware on
// an ESP32-family device beside whatever else the device's flash holds. It
// is published with the images, so everything specific to a device lives
// there and AetherSDR's installer stays generic. Format 1, as JSON:
//
//   {
//     "format": 1,
//     "product": "Example", "version": "1.2.3",
//     "device": "Example Box", "chip": "ESP32-S3", "flashSize": "0x1000000",
//     "protect": [ { "name": "other app", "offset": "0x10000", "size": "0x300000" } ],
//     "require": [ { "name": "partition rows", "offset": "0x8000", "size": "0xa0",
//                    "sha256": "..." } ],
//     "boot": { "otadata": "0xe000", "slots": ["0x10000", "0x410000", "0x710000"],
//               "ownSlots": [1, 2] },
//     "images": [ { "file": "app.bin", "offset": "0x410000", "size": 1234,
//                   "sha256": "..." } ]
//   }
//
// protect: regions never written, whatever else the manifest says (another
// firmware, its settings). require: bytes that must already be on the device
// and that the install must leave as they are. boot: ESP-IDF's boot selection
// (otadata) and the app slots it chooses among, in partition-table order;
// ownSlots are the slots this firmware runs from. images: written in order
// on install; each is checked against its size and SHA-256 first.
struct EspFirmwareManifest {
    struct Region {
        QString name;
        quint32 offset{0};
        quint32 size{0};
        QByteArray sha256;   // require only
    };
    struct Image {
        QString file;        // relative to the manifest
        quint32 offset{0};
        quint32 size{0};
        QByteArray sha256;
    };

    QString product;
    QString version;
    QString device;   // the hardware it is for, as shown (optional)
    QString chip;
    quint32 flashSize{0};
    QList<Region> protect;
    QList<Region> require;
    quint32 otadata{0};
    QList<quint32> appSlots;   // "boot.slots"
    QList<int> ownSlots;
    QList<Image> images;

    // Parses and checks a manifest: every image, the boot selection and the
    // own slots stay clear of protected regions (by whole sectors, as flash is
    // erased), everything fits in the flash, and the plan holds together
    // (checkPlan). On failure returns false and says why.
    static bool parse(const QByteArray& json, EspFirmwareManifest* out, QString* problem);
    // The writes a manifest plans, as a whole: no two images, nor an image and
    // the boot selection, share a sector, and no required region lies under
    // the boot selection (which every install and start rewrites). parse()
    // checks it; the installer checks again before writing anything.
    static bool checkPlan(const EspFirmwareManifest& m, QString* problem);
};

} // namespace AetherSDR
