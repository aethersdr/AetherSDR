#pragma once

#include "EspFirmwareManifest.h"
#include "EspFlashTarget.h"

#include <QByteArray>
#include <QHash>
#include <QString>

#include <functional>

namespace AetherSDR {

// Installs firmware on an ESP32-family device in download mode, as its
// manifest (EspFirmwareManifest) describes, and nothing more: it refuses to
// write a protected region whatever it is asked, and checks that the device
// is the one the manifest is for before writing anything. Every call blocks;
// run it on a worker thread.
class EspFirmwareInstaller {
public:
    // step: a short description for display; done/total: bytes in this step.
    using Report = std::function<bool(const QString& step, qint64 done, qint64 total)>;

    EspFirmwareInstaller(EspFlashTarget* target, Report report);

    // Whether the device is the chip and flash the manifest is for and holds
    // its required regions. On failure says why.
    bool check(const EspFirmwareManifest& manifest, QString* problem);

    // The whole flash, for a backup.
    bool backup(QByteArray* flash, QString* problem);

    // Checks the device, then writes the manifest's images (files: name ->
    // contents, each checked against the manifest) and selects the first of
    // its own slots to start. Sectors already holding the right bytes are
    // not rewritten.
    bool install(const EspFirmwareManifest& manifest, const QHash<QString, QByteArray>& files,
                 QString* problem);

    // Checks the device, then selects the firmware's current slot to start:
    // the newest boot selection naming one of its own slots, else the first.
    // Writes only the boot selection.
    bool start(const EspFirmwareManifest& manifest, QString* problem);

    // Writes a whole-flash backup back, sector groups that differ only.
    bool restore(const QByteArray& flash, QString* problem);

    // ESP-IDF's boot selection (otadata): two entries, one per sector; the
    // bootloader starts slot (seq - 1) % slotCount of the valid entry with the
    // highest sequence number, or slot 0 when neither is valid.
    struct OtaEntry {
        quint32 seq{0};
        bool valid{false};
    };
    static QList<OtaEntry> readOtadata(const QByteArray& otadata);
    // otadata changed so that it starts slot, writing over the older entry.
    static QByteArray selectSlot(const QByteArray& otadata, int slot, int slotCount);
    // The slot of the newest valid entry naming one of candidates, or -1.
    static int newestOf(const QByteArray& otadata, const QList<int>& candidates, int slotCount);

private:
    bool step(const QString& name, qint64 done, qint64 total);
    bool writeRegion(quint32 offset, const QByteArray& data, const QString& name, QString* problem);
    bool writeOtadata(const EspFirmwareManifest& manifest, int slot, QString* problem);

    EspFlashTarget* m_target;
    Report m_report;
    QList<EspFirmwareManifest::Region> m_protect;   // of the manifest being acted on
};

} // namespace AetherSDR
