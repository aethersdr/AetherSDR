#include "OleCompoundFile.h"
#include "LogManager.h"

#include <QtEndian>
#include <QSet>

#include <limits>
#include <algorithm>

namespace AetherSDR {

namespace {

// CFB magic at offset 0. [MS-CFB] §2.2.
constexpr quint8 kCfbMagic[8] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
constexpr quint64 kMaxMiniStreamBytes = 16ULL * 1024ULL * 1024ULL;
constexpr quint64 kMaxDirectoryBytes = 16ULL * 1024ULL * 1024ULL;
constexpr quint64 kMaxFatBytes = 16ULL * 1024ULL * 1024ULL;

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
    if (m_header.sectorShift != (majorVersion == 3 ? 9 : 12)
        || m_header.miniSectorShift != 6 || m_header.miniStreamCutoff != 4096
        || (majorVersion == 3 && m_header.numDirectorySectors != 0)
        || (majorVersion == 4 && m_header.numDirectorySectors == 0)) {
        m_lastError = "Unsupported CFB sector layout";
        return false;
    }
    const quint64 sectorsInFile = static_cast<quint64>(m_file.size()) / sectorSize();
    const quint64 maxMiniFatSectors = (kMaxMiniStreamBytes / miniSectorSize()
                                      + sectorSize() / 4 - 1) / (sectorSize() / 4);
    if (m_header.numFatSectors > sectorsInFile || m_header.numMiniFatSectors > sectorsInFile
        || m_header.numDifatSectors > sectorsInFile || m_header.numMiniFatSectors > maxMiniFatSectors
        || static_cast<quint64>(m_header.numFatSectors) * sectorSize() > kMaxFatBytes
        || static_cast<quint64>(m_header.numDirectorySectors) * sectorSize() > kMaxDirectoryBytes) {
        m_lastError = "CFB allocation tables exceed file size";
        return false;
    }

    // Inline DIFAT: header bytes 76..511, 109 entries of 4 bytes each.
    m_difat.reserve(static_cast<int>(m_header.numFatSectors));
    for (int i = 0; i < 109; ++i) {
        const quint32 fatSector = readLE<quint32>(hdr, 76 + i * 4);
        if (fatSector == kFatFreeSector) break;
        if (m_difat.size() >= m_header.numFatSectors) {
            m_lastError = "CFB FAT entries exceed declared count";
            return false;
        }
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
            if (m_difat.size() >= m_header.numFatSectors) {
                m_lastError = "CFB DIFAT entries exceed declared FAT count";
                return false;
            }
            m_difat.append(fatSector);
        }
        // Last 4 bytes of the DIFAT sector point to the next DIFAT sector.
        nextDifat = readLE<quint32>(sec, sectorSize() - 4);
    }
    return true;
}

bool OleCompoundFile::readFat()
{
    if (m_difat.size() != m_header.numFatSectors) {
        m_lastError = "CFB FAT count does not match declaration";
        return false;
    }
    QSet<quint32> seen;
    const quint32 entriesPerSector = sectorSize() / 4;
    m_fat.reserve(m_difat.size() * static_cast<int>(entriesPerSector));
    for (quint32 fatSector : m_difat) {
        if (seen.contains(fatSector)) {
            m_lastError = "Duplicate CFB FAT sector";
            return false;
        }
        seen.insert(fatSector);
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
    // CFB v4 records the sector count; v3 uses the bounded unsized-chain path.
    const QByteArray dirData = readChain(m_header.firstDirSector,
        static_cast<quint64>(m_header.numDirectorySectors) * sectorSize());
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
    if (root.streamSize > kMaxMiniStreamBytes) {
        m_lastError = "CFB mini stream exceeds size limit";
        return false;
    }
    m_miniStream = readChain(root.startSector, root.streamSize);
    return static_cast<quint64>(m_miniStream.size()) == root.streamSize;
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
    for (qsizetype index = 0; index < encoded.size(); ++index) {
        const QChar qc = encoded.at(index);
        const ushort cp = qc.unicode();
        if (cp >= 0x3800 && cp < 0x4800) {
            const ushort v = cp - 0x3800;
            out.append(QChar::fromLatin1(kTable[v & 0x3F]));
            const ushort hi = (v >> 6) & 0x3F;
            out.append(QChar::fromLatin1(kTable[hi]));
        } else if (cp >= 0x4800 && cp < 0x4840) {
            out.append(QChar::fromLatin1(kTable[cp - 0x4800]));
        } else if (cp == 0x4840 && index == 0) {
            // The leading marker distinguishes tables from binary streams.
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

QByteArray OleCompoundFile::readMsiStream(const QString& decodedName, qint64 maxBytes) const
{
    return readMsiEntry(decodedName, maxBytes, false);
}

QByteArray OleCompoundFile::readMsiTable(const QString& decodedName, qint64 maxBytes) const
{
    return readMsiEntry(decodedName, maxBytes, true);
}

QByteArray OleCompoundFile::readMsiEntry(const QString& decodedName, qint64 maxBytes, bool table) const
{
    const DirEntry* match = nullptr;
    for (const auto& e : m_entries) {
        if (e.type != EntryType::Stream) continue;
        const bool isTable = !e.name.isEmpty() && e.name.front().unicode() == 0x4840;
        if (isTable != table) continue;
        if (decodeMsiName(e.name) != decodedName) continue;
        if (match) {
            m_lastError = "Duplicate MSI stream: " + decodedName;
            return {};
        }
        match = &e;
    }
    if (!match) {
        m_lastError = "MSI stream not found: " + decodedName;
        return {};
    }
    if (match->streamSize == 0) {
        return {};
    }
    if (match->streamSize > static_cast<quint64>(std::numeric_limits<int>::max())
        || (maxBytes > 0 && match->streamSize > static_cast<quint64>(maxBytes))) {
        m_lastError = "MSI stream exceeds size limit: " + decodedName;
        return {};
    }
    QByteArray bytes = match->streamSize < m_header.miniStreamCutoff
        ? readMiniChain(match->startSector, match->streamSize)
        : readChain(match->startSector, match->streamSize);
    if (static_cast<quint64>(bytes.size()) != match->streamSize) {
        m_lastError = "Truncated MSI stream: " + decodedName;
        return {};
    }
    return bytes;
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
    if (totalSize > static_cast<quint64>(m_file.size())
        || totalSize > static_cast<quint64>(std::numeric_limits<int>::max())) {
        m_lastError = "CFB chain size exceeds file or allocation limit";
        return {};
    }
    QByteArray out;
    out.reserve(static_cast<int>(totalSize > 0 ? totalSize : sectorSize()));
    quint32 sector = startSector;
    quint32 hops = 0;
    const quint64 maxHops = totalSize > 0
        ? (totalSize + sectorSize() - 1) / sectorSize()
        : std::min(static_cast<quint64>(m_fat.size()), kMaxDirectoryBytes / sectorSize());
    while (sector != kFatEndOfChain && sector != kFatFreeSector) {
        if (++hops > maxHops) {
            m_lastError = "FAT chain loop or runaway";
            return QByteArray();
        }
        if (sector >= static_cast<quint32>(m_fat.size())) {
            m_lastError = "FAT sector index out of range";
            return QByteArray();
        }
        const QByteArray bytes = readSector(sector);
        if (bytes.size() != sectorSize()) {
            m_lastError = "Truncated CFB sector";
            return {};
        }
        out.append(bytes);
        sector = m_fat.at(sector);
    }
    if (totalSize > 0 && static_cast<quint64>(out.size()) < totalSize) {
        m_lastError = "CFB chain shorter than declared stream";
        return {};
    }
    if (totalSize > 0 && static_cast<quint64>(out.size()) > totalSize)
        out.truncate(static_cast<int>(totalSize));
    return out;
}

QByteArray OleCompoundFile::readMiniChain(quint32 startSector, quint64 totalSize) const
{
    if (totalSize > static_cast<quint64>(m_miniStream.size())) {
        m_lastError = "CFB mini chain exceeds root stream size";
        return {};
    }
    QByteArray out;
    out.reserve(static_cast<int>(totalSize));
    quint32 sector = startSector;
    quint32 hops = 0;
    const quint32 mss = miniSectorSize();
    const quint64 maxHops = (totalSize + mss - 1) / mss;
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
    if (static_cast<quint64>(out.size()) < totalSize) {
        m_lastError = "CFB mini chain shorter than declared stream";
        return {};
    }
    if (out.size() > static_cast<int>(totalSize))
        out.truncate(static_cast<int>(totalSize));
    return out;
}

} // namespace AetherSDR
