#pragma once

#include <QByteArray>
#include <QFile>
#include <QList>
#include <QString>
#include <QStringList>

namespace MasterSDR {

// Read-only reader for the Microsoft OLE Compound File Binary (CFB)
// format, also known as Compound Document, Structured Storage, or
// "the inside of a .doc/.xls/.msi". Specification: [MS-CFB].
//
// Scope: this implementation supports only the subset we need to
// extract `cab*.cab` streams from FlexRadio's WiX MSI installers:
//
//   • Read-only; no write or modify operations.
//   • Stream lookup by name (UTF-16LE → UTF-8 conversion).
//   • Streams of any size (regular FAT chain).
//   • Mini-stream (small file < 4096 bytes) read-out included for
//     completeness but not exercised by our cab use case.
//
// Out of scope: storage hierarchies (we treat the directory as a
// flat list, ignoring storage parent/child relationships), property
// sets, transactioning, encryption.
class OleCompoundFile {
public:
    OleCompoundFile() = default;
    ~OleCompoundFile() = default;

    // Open the file and parse header + FAT + directory.  Returns false
    // on any failure (not a CFB, truncated, version mismatch, etc.).
    bool open(const QString& path);

    // Names of every stream entry in the directory.  Stream entries
    // only — storage entries (directories) and the root entry are
    // excluded.
    QStringList streamNames() const;

    // Read a stream by exact name match (case-sensitive). Returns
    // empty QByteArray if not found or unreadable.
    QByteArray readStream(const QString& name) const;

    // Convenience: every stream whose name starts with `prefix` and
    // ends with `suffix`.  Returned in directory order.  This is
    // enough for our `cab*.cab` use case without bringing in a real
    // wildcard matcher.
    QList<QPair<QString, QByteArray>> readStreamsByPrefixSuffix(
        const QString& prefix, const QString& suffix) const;

    // MSI-aware variants. Microsoft Installer (.msi) files store
    // table and stream names using a 5-bit packed encoding that lives
    // inside Unicode code points 0x3800-0x4840. The plain accessors
    // above return the raw encoded names; the variants below return
    // the decoded human-readable form.
    QStringList msiStreamNames() const;
    QByteArray  readMsiStream(const QString& decodedName) const;
    QList<QPair<QString, QByteArray>> readMsiStreamsByPrefixSuffix(
        const QString& prefix, const QString& suffix) const;

    // Decode a single MSI-encoded name.  Exposed publicly because it
    // is sometimes useful to a caller that wants to interpret a name
    // it already has (e.g. from a directory listing).
    static QString decodeMsiName(const QString& encoded);

    // Last error message, for diagnostics.
    QString lastError() const { return m_lastError; }

private:
    struct Header {
        quint16 sectorShift{};      // 2^N bytes per sector (typically 9 or 12)
        quint16 miniSectorShift{};  // 2^N bytes per mini-sector (typically 6)
        quint32 numDirectorySectors{};
        quint32 numFatSectors{};
        quint32 firstDirSector{};
        quint32 firstMiniFatSector{};
        quint32 numMiniFatSectors{};
        quint32 firstDifatSector{};
        quint32 numDifatSectors{};
        quint32 miniStreamCutoff{};
    };

    // Directory entry types (offset 66 in each 128-byte entry).
    enum class EntryType : quint8 {
        Unknown    = 0,
        Storage    = 1,
        Stream     = 2,
        RootEntry  = 5,
    };

    struct DirEntry {
        QString    name;            // UTF-8 form of the UTF-16LE name
        EntryType  type{EntryType::Unknown};
        quint32    startSector{};   // First sector of the data chain
        quint64    streamSize{};    // 64-bit on CFB v4; 32-bit on v3
    };

    // FAT chain terminator constants (per [MS-CFB]).
    static constexpr quint32 kFatEndOfChain = 0xFFFFFFFE;
    static constexpr quint32 kFatFreeSector = 0xFFFFFFFF;
    static constexpr quint32 kFatFatSector  = 0xFFFFFFFD;
    static constexpr quint32 kFatDifSector  = 0xFFFFFFFC;

    bool readHeader();
    bool readDifat();          // Assemble full FAT-sector index list
    bool readFat();            // Read every FAT sector into m_fat
    bool readMiniFat();        // Read mini-FAT sectors into m_miniFat
    bool readDirectory();      // Walk directory chain; populate m_entries
    bool readMiniStream();     // Cache the root entry's mini stream

    QByteArray readSector(quint32 sectorIndex) const;
    QByteArray readChain(quint32 startSector, quint64 totalSize) const;
    QByteArray readMiniChain(quint32 startSector, quint64 totalSize) const;

    quint32 sectorSize() const { return 1u << m_header.sectorShift; }
    quint32 miniSectorSize() const { return 1u << m_header.miniSectorShift; }
    qint64  sectorOffset(quint32 sectorIndex) const {
        // CFB stream data starts at sector 1 (header occupies sector 0
        // for v4 with 4 KB sectors; for v3 with 512 B sectors, the header
        // is exactly 512 B and sector 0 is the first data sector — so the
        // formula is "header bytes + sectorIndex * sectorSize", where
        // header bytes is just one sector.)
        return static_cast<qint64>(sectorIndex + 1) * sectorSize();
    }

    mutable QFile     m_file;
    Header            m_header{};
    QList<quint32>    m_difat;       // Concatenated FAT-sector index list
    QList<quint32>    m_fat;         // Per-sector chain (sector → next sector)
    QList<quint32>    m_miniFat;     // Per-mini-sector chain
    QByteArray        m_miniStream;  // Concatenated mini-stream data
    QList<DirEntry>   m_entries;
    mutable QString   m_lastError;
};

} // namespace MasterSDR
