#pragma once

#include <QByteArray>
#include <QList>
#include <QPair>
#include <QString>

namespace AetherSDR {

// Extract files from a Microsoft CAB archive that lives entirely in
// memory. Used by the firmware-update path to unpack `cab*.cab` streams
// pulled from a WiX MSI.
//
// libmspack does the actual CAB+LZX/MSZIP/Quantum decompression. We
// supply a custom mspack_system that reads the cabinet bytes from a
// QByteArray and writes the decompressed payload to another QByteArray —
// no temp files involved.
class CabExtractor {
public:
    // Extract every file in `cabBytes` into `out` as (filename, bytes)
    // pairs, in cabinet order, capped at 500 MiB total. Returns true on success.
    bool extractAll(const QByteArray& cabBytes,
                    QList<QPair<QString, QByteArray>>& out);

    // Extract exactly one MSI File key, bounded by its recorded FileSize.
    bool extractFile(const QByteArray& cabBytes, const QString& memberName,
                     qint64 expectedSize, QByteArray& out);

    // Convenience for archive inspection: return the first file whose
    // payload begins with `magic`. Firmware selection uses extractFile().
    bool extractFirstMatchingMagic(const QByteArray& cabBytes,
                                   const QByteArray& magic,
                                   QByteArray& out);

    QString lastError() const { return m_lastError; }

private:
    QString m_lastError;
};

} // namespace AetherSDR
