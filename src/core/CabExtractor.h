#pragma once

#include <QByteArray>
#include <QList>
#include <QPair>
#include <QString>

namespace MasterSDR {

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
    // pairs, in cabinet order. Returns true on success.
    bool extractAll(const QByteArray& cabBytes,
                    QList<QPair<QString, QByteArray>>& out);

    // Convenience: extract every file and return the first whose
    // payload begins with `magic`. Used by the firmware-update path
    // to grab the .ssdr blob (magic="Salted__") and ignore the
    // sibling driver/installer files in the same cab.
    bool extractFirstMatchingMagic(const QByteArray& cabBytes,
                                   const QByteArray& magic,
                                   QByteArray& out);

    QString lastError() const { return m_lastError; }

private:
    QString m_lastError;
};

} // namespace MasterSDR
