#include "OleCompoundFile.h"
#include "LogManager.h"

#include <QtEndian>

namespace MasterSDR {

namespace {

// CFB magic at offset 0. [MS-CFB] §2.2.
constexpr quint8 kCfbMagic[8] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};

// Read a little-endian integer from a byte buffer at a given offset.
template <typename T>
T readLE(const QByteArray& buf, qsizetype offset) {
    T value{};
    if (offset + static_cast<qsizetype>(sizeof(T)) > buf.size())
        return value;
    std::memcpy(&value, buf.constData() + offset, sizeof(T));
    return qFromLittleEndian(value);
}

// Decode a UTF-16LE directory-entry name field.  The 64-byte name area
// is followed by a 2-byte "name length in bytes including the trailing
// NUL" field.  Returns the name without the NUL.
QString decodeDirName(const QByteArray& entry) {
    quint16 nameLenBytes = readLE<quint16>(entry, 64);
    if (nameLenBytes < 2 || nameLenBytes > 64)
        return QString();
    // nameLenBytes counts the trailing UTF-16 NUL (2 bytes), so subtract.
    const int u16len = (nameLenBytes - 2) / 2;
    QString out;
    out.reserve(u16len);
    for (int i = 0; i < u16len; ++i) {
        const quint16 ch = readLE<quint16>(entry, i * 2);
        out.append(QChar(ch));
    }
    return out;
}

} // anonymous namespace

bool OleCompoundFile::open(const QString& path)
{
    m_file.setFileName(path);
    if (!m_file.open(QIODevice::ReadOnly)) {
        m_lastError = "open failed: " + m_file.errorString();
        return false;
    }
    if (!readHeader())     return false;
    if (!readDifat())      return false;
    if (!readFat())        return false;
    if (!readMiniFat())    return false;
    if (!readDirectory())  return false;
    if (!readMiniStream()) return false;
    return true;
}

bool OleCompoundFile::readHeader()
{
    m_file.seek(0);
    const QByteArray hdr = m_file.read(512);
    if (hdr.size() != 512) {
        m_lastError = "header truncated";
        return false;
    }
    if (std::memcmp(hdr.constData(), kCfbMagic, 8) != 0) {
        m_lastError = "not a CFB file (magic mismatch)";
        return false;
    }
    const quint16 majorVersion = readLE<quint16>(hdr, 26);
    if (majorVersion != 3 && majorVersion != 4) {
        m_lastError = QString("unsupported CFB major version %1").arg(majorVersion);
        return false;
    }
    m_header.sectorShift          = readLE<quint16>(hdr, 30);
    m_header.miniSectorShift      = readLE<quint16>(hdr, 32);
    m_header.numDirectorySectors  = readLE<quint32>(hdr, 40);
    m_header.numFatSectors        = readLE<quint32>(hdr, 44);
    m_header.firstDirSector       = readLE<quint32>(hdr, 48);
    m_header.miniStreamCutoff     = readLE<quint32>(hdr, 56);
    m_header.firstMiniFatSector   = readLE<quint32>(hdr, 60);
    m_header.numMiniFatSectors    = readLE<quint32>(hdr, 64);
    m_header.firstDifatSector     = readLE<quint32>(hdr, 68);
    m_header.numDifatSectors      = readLE<quint32>(hdr, 72);

    // Sanity bounds — guard against corrupt/malicious headers before
    // we use these values to size other allocations.
    if (m_header.sectorShift < 7 || m_header.sectorShift > 16) {
        m_lastError = QString("implausible sector shift %1").arg(m_header.sectorShift);
        return false;
    }
    if (m_header.numFatSectors > 1u << 24) {
        m_lastError = "implausible FAT-sector count";
        return false;
    }

    // Inline DIFAT: header bytes 76..511, 109 entries of 4 bytes each.
    m_difat.reserve(static_cast<int>(m_header.numFatSectors));
    for (int i = 0; i < 109; ++i) {
        const quint32 fatSector = readLE<quint32>(hdr, 76 + i * 4);
        if (fatSector == kFatFreeSector) break;
        m_difat.append(fatSector);
    }
    return true;
}

bool OleCompoundFile::readDifat()
{
    // If everything fit in the 109 inline slots, we're done.
    if (m_header.numDifatSectors == 0)
        return true;

    quint32 nextDifat = m_header.firstDifatSector;
    quint32 difatHops = 0;
    const quint32 entriesPerSector = (sectorSize() / 4) - 1;  // last slot = next pointer

    while (nextDifat != kFatEndOfChain && nextDifat != kFatFreeSector) {
        if (++difatHops > m_header.numDifatSectors + 4) {
            m_lastError = "DIFAT chain longer than declared";
            return false;
        }
        const QByteArray sec = readSector(nextDifat);
        if (sec.size() != static_cast<int>(sectorSize())) {
            m_lastError = "DIFAT sector read failure";
            return false;
        }
        for (quint32 i = 0; i < entriesPerSector; ++i) {
            const quint32 fatSector = readLE<quint32>(sec, i * 4);
            if (fatSector == kFatFreeSector) continue;
            m_difat.append(fatSector);
        }
        // Last 4 bytes of the DIFAT sector point to the next DIFAT sector.
        nextDifat = readLE<quint32>(sec, sectorSize() - 4);
    }
    return true;
}

bool OleCompoundFile::readFat()
{
    const quint32 entriesPerSector = sectorSize() / 4;
    m_fat.reserve(m_difat.size() * static_cast<int>(entriesPerSector));
    for (quint32 fatSector : m_difat) {
        const QByteArray sec = readSector(fatSector);
        if (sec.size() != static_cast<int>(sectorSize())) {
            m_lastError = "FAT sector read failure";
            return false;
        }
        for (quint32 i = 0; i < entriesPerSector; ++i)
            m_fat.append(readLE<quint32>(sec, i * 4));
    }
    return true;
}

bool OleCompoundFile::readMiniFat()
{
    if (m_header.numMiniFatSectors == 0)
        return true;
    quint32 sector = m_header.firstMiniFatSector;
    const quint32 entriesPerSector = sectorSize() / 4;
    quint32 hops = 0;
    while (sector != kFatEndOfChain && sector != kFatFreeSector) {
        if (++hops > m_header.numMiniFatSectors + 4) {
            m_lastError = "mini-FAT chain longer than declared";
            return false;
        }
        const QByteArray sec = readSector(sector);
        if (sec.size() != static_cast<int>(sectorSize()))
            return false;
        for (quint32 i = 0; i < entriesPerSector; ++i)
            m_miniFat.append(readLE<quint32>(sec, i * 4));
        if (sector >= static_cast<quint32>(m_fat.size())) {
            m_lastError = "mini-FAT sector index out of range";
            return false;
        }
        sector = m_fat.at(sector);
    }
    return true;
}

bool OleCompoundFile::readDirectory()
{
    const QByteArray dirData = readChain(m_header.firstDirSector,
        // Directory size is technically unbounded; walk the FAT chain
        // until end and stop. We pass 0 for "until end of chain".
        0);
    if (dirData.isEmpty()) {
        m_lastError = "empty directory chain";
        return false;
    }
    // Each directory entry is exactly 128 bytes per spec.
    const int numEntries = dirData.size() / 128;
    m_entries.reserve(numEntries);
    for (int i = 0; i < numEntries; ++i) {
        const QByteArray entry = dirData.mid(i * 128, 128);
        DirEntry e;
        e.name = decodeDirName(entry);
        const quint8 typeRaw = static_cast<quint8>(entry.at(66));
        e.type = static_cast<EntryType>(typeRaw);
        e.startSector = readLE<quint32>(entry, 116);
        // CFB v4 has a 64-bit size field (lo 4 bytes at 120, hi 4 bytes at 124).
        // CFB v3 only uses the low 4 bytes.
        const quint32 sizeLo = readLE<quint32>(entry, 120);
        const quint32 sizeHi = readLE<quint32>(entry, 124);
        e.streamSize = (static_cast<quint64>(sizeHi) << 32) | sizeLo;
        m_entries.append(e);
    }
    return true;
}

bool OleCompoundFile::readMiniStream()
{
    // The root entry (entry 0) holds the mini stream's location.
    if (m_entries.isEmpty() || m_entries.first().type != EntryType::RootEntry) {
        // No root or no mini stream — fine, we just won't be able to read
        // tiny streams (which we don't expect to need anyway).
        return true;
    }
    const DirEntry& root = m_entries.first();
    if (root.streamSize == 0)
        return true;
    m_miniStream = readChain(root.startSector, root.streamSize);
    return true;
}

QStringList OleCompoundFile::streamNames() const
{
    QStringList out;
    for (const auto& e : m_entries) {
        if (e.type == EntryType::Stream)
            out.append(e.name);
    }
    return out;
}

QByteArray OleCompoundFile::readStream(const QString& name) const
{
    for (const auto& e : m_entries) {
        if (e.type != EntryType::Stream) continue;
        if (e.name != name) continue;
        if (e.streamSize < m_header.miniStreamCutoff)
            return readMiniChain(e.startSector, e.streamSize);
        return readChain(e.startSector, e.streamSize);
    }
    m_lastError = "stream not found: " + name;
    return QByteArray();
}

QList<QPair<QString, QByteArray>> OleCompoundFile::readStreamsByPrefixSuffix(
    const QString& prefix, const QString& suffix) const
{
    QList<QPair<QString, QByteArray>> out;
    for (const auto& e : m_entries) {
        if (e.type != EntryType::Stream) continue;
        if (!e.name.startsWith(prefix)) continue;
        if (!e.name.endsWith(suffix)) continue;
        QByteArray data = (e.streamSize < m_header.miniStreamCutoff)
            ? readMiniChain(e.startSector, e.streamSize)
            : readChain(e.startSector, e.streamSize);
        if (!data.isEmpty())
            out.append({e.name, std::move(data)});
    }
    return out;
}

// ─── MSI-aware accessors ─────────────────────────────────────────────

QString OleCompoundFile::decodeMsiName(const QString& encoded)
{
    // 64-entry MSI alphabet (digits, upper, lower, '.', '_').
    static const char kTable[65] =
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz._";

    QString out;
    out.reserve(encoded.size() * 2);
    for (QChar qc : encoded) {
        const ushort cp = qc.unicode();
        if (cp >= 0x3800 && cp < 0x4840) {
            const ushort v = cp - 0x3800;
            out.append(QChar::fromLatin1(kTable[v & 0x3F]));
            const ushort hi = (v >> 6) & 0x3F;
            if (hi != 0)
                out.append(QChar::fromLatin1(kTable[hi]));
        } else if (cp == 0x4840) {
            // MSI stream-name marker (vs. table-name) — drop silently.
            continue;
        } else {
            // ASCII control chars (e.g. \005SummaryInformation) and
            // anything outside the MSI compression range pass through.
            out.append(qc);
        }
    }
    return out;
}

QStringList OleCompoundFile::msiStreamNames() const
{
    QStringList out;
    for (const auto& e : m_entries) {
        if (e.type == EntryType::Stream)
            out.append(decodeMsiName(e.name));
    }
    return out;
}

QByteArray OleCompoundFile::readMsiStream(const QString& decodedName) const
{
    for (const auto& e : m_entries) {
        if (e.type != EntryType::Stream) continue;
        if (decodeMsiName(e.name) != decodedName) continue;
        if (e.streamSize < m_header.miniStreamCutoff)
            return readMiniChain(e.startSector, e.streamSize);
        return readChain(e.startSector, e.streamSize);
    }
    m_lastError = "MSI stream not found: " + decodedName;
    return QByteArray();
}

QList<QPair<QString, QByteArray>> OleCompoundFile::readMsiStreamsByPrefixSuffix(
    const QString& prefix, const QString& suffix) const
{
    QList<QPair<QString, QByteArray>> out;
    for (const auto& e : m_entries) {
        if (e.type != EntryType::Stream) continue;
        const QString decoded = decodeMsiName(e.name);
        if (!decoded.startsWith(prefix)) continue;
        if (!decoded.endsWith(suffix)) continue;
        QByteArray data = (e.streamSize < m_header.miniStreamCutoff)
            ? readMiniChain(e.startSector, e.streamSize)
            : readChain(e.startSector, e.streamSize);
        if (!data.isEmpty())
            out.append({decoded, std::move(data)});
    }
    return out;
}

QByteArray OleCompoundFile::readSector(quint32 sectorIndex) const
{
    const qint64 offset = sectorOffset(sectorIndex);
    if (!m_file.seek(offset))
        return QByteArray();
    return m_file.read(sectorSize());
}

QByteArray OleCompoundFile::readChain(quint32 startSector, quint64 totalSize) const
{
    QByteArray out;
    out.reserve(static_cast<int>(totalSize > 0 ? totalSize : sectorSize()));
    quint32 sector = startSector;
    quint32 hops = 0;
    const quint32 maxHops = static_cast<quint32>(m_fat.size()) + 4;
    while (sector != kFatEndOfChain && sector != kFatFreeSector) {
        if (++hops > maxHops) {
            m_lastError = "FAT chain loop or runaway";
            return QByteArray();
        }
        if (sector >= static_cast<quint32>(m_fat.size())) {
            m_lastError = "FAT sector index out of range";
            return QByteArray();
        }
        out.append(readSector(sector));
        sector = m_fat.at(sector);
    }
    if (totalSize > 0 && static_cast<quint64>(out.size()) > totalSize)
        out.truncate(static_cast<int>(totalSize));
    return out;
}

QByteArray OleCompoundFile::readMiniChain(quint32 startSector, quint64 totalSize) const
{
    QByteArray out;
    out.reserve(static_cast<int>(totalSize));
    quint32 sector = startSector;
    quint32 hops = 0;
    const quint32 maxHops = static_cast<quint32>(m_miniFat.size()) + 4;
    const quint32 mss = miniSectorSize();
    while (sector != kFatEndOfChain && sector != kFatFreeSector) {
        if (++hops > maxHops) {
            m_lastError = "mini-FAT chain loop or runaway";
            return QByteArray();
        }
        if (sector >= static_cast<quint32>(m_miniFat.size())) {
            m_lastError = "mini-FAT sector index out of range";
            return QByteArray();
        }
        const qint64 offset = static_cast<qint64>(sector) * mss;
        if (offset + mss > static_cast<qint64>(m_miniStream.size())) {
            m_lastError = "mini-FAT offset past end of mini stream";
            return QByteArray();
        }
        out.append(m_miniStream.constData() + offset, mss);
        sector = m_miniFat.at(sector);
    }
    if (out.size() > static_cast<int>(totalSize))
        out.truncate(static_cast<int>(totalSize));
    return out;
}

} // namespace MasterSDR
