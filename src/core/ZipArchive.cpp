#include "ZipArchive.h"

#include <zlib.h>

#include <algorithm>
#include <limits>

namespace AetherSDR {

namespace {

void setZipError(QString* error, const QString& message)
{
    if (error)
        *error = message;
}

quint16 readLe16(const QByteArray& bytes, qsizetype offset)
{
    if (offset < 0 || offset + 2 > bytes.size())
        return 0;
    const auto* p = reinterpret_cast<const uchar*>(bytes.constData() + offset);
    return quint16(p[0]) | (quint16(p[1]) << 8);
}

quint32 readLe32(const QByteArray& bytes, qsizetype offset)
{
    if (offset < 0 || offset + 4 > bytes.size())
        return 0;
    const auto* p = reinterpret_cast<const uchar*>(bytes.constData() + offset);
    return quint32(p[0]) | (quint32(p[1]) << 8) | (quint32(p[2]) << 16) | (quint32(p[3]) << 24);
}

void appendLe16(QByteArray& out, quint16 value)
{
    out.append(char(value & 0xff));
    out.append(char((value >> 8) & 0xff));
}

void appendLe32(QByteArray& out, quint32 value)
{
    out.append(char(value & 0xff));
    out.append(char((value >> 8) & 0xff));
    out.append(char((value >> 16) & 0xff));
    out.append(char((value >> 24) & 0xff));
}

bool inflateRawDeflate(const QByteArray& compressed, qsizetype expectedSize,
                       QByteArray* out, QString* error)
{
    if (expectedSize < 0) {
        setZipError(error, QStringLiteral("Invalid ZIP entry size."));
        return false;
    }

    out->resize(expectedSize);
    z_stream stream{};
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(compressed.constData()));
    stream.avail_in = static_cast<uInt>(compressed.size());
    stream.next_out = reinterpret_cast<Bytef*>(out->data());
    stream.avail_out = static_cast<uInt>(out->size());

    int rc = inflateInit2(&stream, -MAX_WBITS);
    if (rc != Z_OK) {
        setZipError(error, QStringLiteral("Could not initialize ZIP inflater."));
        return false;
    }

    rc = inflate(&stream, Z_FINISH);
    inflateEnd(&stream);
    if (rc != Z_STREAM_END || stream.total_out != static_cast<uLong>(expectedSize)) {
        setZipError(error, QStringLiteral("Could not decompress ZIP entry."));
        return false;
    }
    return true;
}

} // namespace

QMap<QString, QByteArray> readZipEntries(const QByteArray& zip, QString* error)
{
    return readZipEntries(zip, error, ZipReadLimits{});
}

QMap<QString, QByteArray> readZipEntries(const QByteArray& zip,
                                         QString* error,
                                         const ZipReadLimits& limits)
{
    QMap<QString, QByteArray> entries;
    if (error) {
        error->clear();
    }
    if (limits.maxEntries < 0
        || limits.maxEntryUncompressedBytes > limits.maxTotalUncompressedBytes) {
        setZipError(error, QStringLiteral("Invalid ZIP safety limits."));
        return entries;
    }
    if (zip.size() < 22 || !zip.startsWith("PK")) {
        setZipError(error, QStringLiteral("Package is not a ZIP-format .ssdr_cfg file."));
        return entries;
    }

    const qsizetype minEocd = 22;
    const qsizetype searchStart = std::max<qsizetype>(0, zip.size() - 65557);
    qsizetype eocd = -1;
    for (qsizetype i = zip.size() - minEocd; i >= searchStart; --i) {
        if (readLe32(zip, i) == 0x06054b50) {
            eocd = i;
            break;
        }
        if (i == 0)
            break;
    }
    if (eocd < 0) {
        setZipError(error, QStringLiteral("Could not find ZIP central directory."));
        return entries;
    }

    const quint16 diskNumber = readLe16(zip, eocd + 4);
    const quint16 centralDirectoryDisk = readLe16(zip, eocd + 6);
    const quint16 diskEntryCount = readLe16(zip, eocd + 8);
    const quint16 entryCount = readLe16(zip, eocd + 10);
    const quint32 centralDirSize = readLe32(zip, eocd + 12);
    const quint32 centralDirOffset = readLe32(zip, eocd + 16);
    if (diskNumber != 0U || centralDirectoryDisk != 0U
        || diskEntryCount != entryCount) {
        setZipError(error, QStringLiteral("Multi-disk ZIP packages are not supported."));
        return entries;
    }
    if (static_cast<qsizetype>(entryCount) > limits.maxEntries) {
        setZipError(error, QStringLiteral("ZIP package contains too many entries."));
        return entries;
    }
    const quint64 centralDirEnd = static_cast<quint64>(centralDirOffset)
        + static_cast<quint64>(centralDirSize);
    if (centralDirEnd > static_cast<quint64>(zip.size())
        || centralDirEnd > static_cast<quint64>(eocd)) {
        setZipError(error, QStringLiteral("ZIP central directory is outside the package."));
        return entries;
    }

    qsizetype offset = centralDirOffset;
    quint64 totalUncompressedBytes = 0U;
    for (quint16 i = 0; i < entryCount; ++i) {
        if (offset < 0
            || static_cast<quint64>(offset) + 46U > centralDirEnd
            || readLe32(zip, offset) != 0x02014b50) {
            setZipError(error, QStringLiteral("Invalid ZIP central directory entry."));
            entries.clear();
            return entries;
        }

        const quint16 flags = readLe16(zip, offset + 8);
        const quint16 method = readLe16(zip, offset + 10);
        const quint32 expectedCrc = readLe32(zip, offset + 16);
        const quint32 compressedSize = readLe32(zip, offset + 20);
        const quint32 uncompressedSize = readLe32(zip, offset + 24);
        const quint16 nameLen = readLe16(zip, offset + 28);
        const quint16 extraLen = readLe16(zip, offset + 30);
        const quint16 commentLen = readLe16(zip, offset + 32);
        const quint32 localOffset = readLe32(zip, offset + 42);

        const quint64 centralEntryEnd = static_cast<quint64>(offset) + 46U
            + static_cast<quint64>(nameLen)
            + static_cast<quint64>(extraLen)
            + static_cast<quint64>(commentLen);
        if (centralEntryEnd > centralDirEnd
            || static_cast<quint64>(localOffset) + 30U
                > static_cast<quint64>(zip.size())
            || readLe32(zip, localOffset) != 0x04034b50) {
            setZipError(error, QStringLiteral("Invalid ZIP entry header."));
            entries.clear();
            return entries;
        }
        if ((flags & 0x0001U) != 0U) {
            setZipError(error, QStringLiteral("Encrypted ZIP entries are not supported."));
            entries.clear();
            return entries;
        }
        if (static_cast<quint64>(uncompressedSize)
                > limits.maxEntryUncompressedBytes
            || totalUncompressedBytes
                > limits.maxTotalUncompressedBytes
                    - static_cast<quint64>(uncompressedSize)) {
            setZipError(error, QStringLiteral("ZIP package exceeds the uncompressed safety limit."));
            entries.clear();
            return entries;
        }
        if (method == 0U && compressedSize != uncompressedSize) {
            setZipError(error, QStringLiteral("Stored ZIP entry has inconsistent sizes."));
            entries.clear();
            return entries;
        }

        const QString name = QString::fromUtf8(zip.constData() + offset + 46, nameLen);
        const quint16 localNameLen = readLe16(zip, localOffset + 26);
        const quint16 localExtraLen = readLe16(zip, localOffset + 28);
        const quint64 dataOffset = static_cast<quint64>(localOffset) + 30U
            + static_cast<quint64>(localNameLen)
            + static_cast<quint64>(localExtraLen);
        if (dataOffset + static_cast<quint64>(compressedSize)
                > static_cast<quint64>(zip.size())
            || localNameLen != nameLen
            || QByteArray(zip.constData() + localOffset + 30, localNameLen)
                != QByteArray(zip.constData() + offset + 46, nameLen)) {
            setZipError(error, QStringLiteral("ZIP entry data is outside the package."));
            entries.clear();
            return entries;
        }
        if (entries.contains(name)) {
            setZipError(error, QStringLiteral("ZIP package contains duplicate entry names."));
            entries.clear();
            return entries;
        }

        const QByteArray compressed = zip.mid(static_cast<qsizetype>(dataOffset),
                                              compressedSize);
        QByteArray data;
        if (method == 0) {
            data = compressed;
        } else if (method == 8) {
            if (!inflateRawDeflate(compressed, uncompressedSize, &data, error)) {
                entries.clear();
                return entries;
            }
        } else {
            setZipError(error, QStringLiteral("Unsupported ZIP compression method %1.").arg(method));
            entries.clear();
            return entries;
        }

        const quint32 actualCrc = crc32(0L, reinterpret_cast<const Bytef*>(data.constData()),
                                        static_cast<uInt>(data.size()));
        if (actualCrc != expectedCrc) {
            setZipError(error, QStringLiteral("ZIP entry checksum mismatch."));
            entries.clear();
            return entries;
        }

        entries.insert(name, data);
        totalUncompressedBytes += static_cast<quint64>(uncompressedSize);
        offset = static_cast<qsizetype>(centralEntryEnd);
    }

    return entries;
}

namespace {

// Raw DEFLATE (no zlib header), matching the readZipEntries inflate path
// (inflateInit2(-MAX_WBITS)) so the writer and reader agree on the framing.
bool rawDeflate(const QByteArray& in, QByteArray& out)
{
    z_stream z{};
    if (deflateInit2(&z, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8,
                     Z_DEFAULT_STRATEGY) != Z_OK) {
        return false;
    }
    out.resize(static_cast<int>(deflateBound(&z, static_cast<uLong>(in.size()))));
    z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.constData()));
    z.avail_in = static_cast<uInt>(in.size());
    z.next_out = reinterpret_cast<Bytef*>(out.data());
    z.avail_out = static_cast<uInt>(out.size());
    const int rc = deflate(&z, Z_FINISH);
    const uLong produced = z.total_out;
    deflateEnd(&z);
    if (rc != Z_STREAM_END)
        return false;
    out.resize(static_cast<int>(produced));
    return true;
}

// Shared ZIP writer. compress=false → stored (method 0); compress=true →
// raw-deflate (method 8). Headers carry compressed + uncompressed sizes
// independently; the CRC is always over the uncompressed data.
QByteArray writeZipImpl(const QList<ZipEntryData>& entries, bool compress)
{
    if (entries.size() > std::numeric_limits<quint16>::max())
        return {};

    QByteArray out;
    QByteArray centralDir;
    for (const ZipEntryData& entry : entries) {
        const QByteArray name = entry.name.toUtf8();
        const QByteArray& raw = entry.data;

        QByteArray deflated;
        if (compress && !rawDeflate(raw, deflated))
            return {};
        const QByteArray& payload = compress ? deflated : raw;
        const quint16 method = compress ? quint16(8) : quint16(0);

        if (name.size() > std::numeric_limits<quint16>::max()
            || raw.size() > std::numeric_limits<quint32>::max()
            || payload.size() > std::numeric_limits<quint32>::max()
            || out.size() > std::numeric_limits<quint32>::max()) {
            return {};
        }

        const quint32 localOffset = static_cast<quint32>(out.size());
        const quint32 checksum = crc32(0L, reinterpret_cast<const Bytef*>(raw.constData()),
                                       static_cast<uInt>(raw.size()));

        appendLe32(out, 0x04034b50);
        appendLe16(out, 20);
        appendLe16(out, 0);
        appendLe16(out, method);
        appendLe16(out, 0);
        appendLe16(out, 0);
        appendLe32(out, checksum);
        appendLe32(out, static_cast<quint32>(payload.size()));
        appendLe32(out, static_cast<quint32>(raw.size()));
        appendLe16(out, static_cast<quint16>(name.size()));
        appendLe16(out, 0);
        out += name;
        out += payload;

        appendLe32(centralDir, 0x02014b50);
        appendLe16(centralDir, 20);
        appendLe16(centralDir, 20);
        appendLe16(centralDir, 0);
        appendLe16(centralDir, method);
        appendLe16(centralDir, 0);
        appendLe16(centralDir, 0);
        appendLe32(centralDir, checksum);
        appendLe32(centralDir, static_cast<quint32>(payload.size()));
        appendLe32(centralDir, static_cast<quint32>(raw.size()));
        appendLe16(centralDir, static_cast<quint16>(name.size()));
        appendLe16(centralDir, 0);
        appendLe16(centralDir, 0);
        appendLe16(centralDir, 0);
        appendLe16(centralDir, 0);
        appendLe32(centralDir, 0);
        appendLe32(centralDir, localOffset);
        centralDir += name;
    }

    if (centralDir.size() > std::numeric_limits<quint32>::max()
        || out.size() > std::numeric_limits<quint32>::max()) {
        return {};
    }

    const quint32 centralDirOffset = static_cast<quint32>(out.size());
    out += centralDir;
    appendLe32(out, 0x06054b50);
    appendLe16(out, 0);
    appendLe16(out, 0);
    appendLe16(out, static_cast<quint16>(entries.size()));
    appendLe16(out, static_cast<quint16>(entries.size()));
    appendLe32(out, static_cast<quint32>(centralDir.size()));
    appendLe32(out, centralDirOffset);
    appendLe16(out, 0);
    return out;
}

} // namespace

QByteArray writeStoredZip(const QList<ZipEntryData>& entries)
{
    return writeZipImpl(entries, false);
}

QByteArray writeDeflatedZip(const QList<ZipEntryData>& entries)
{
    return writeZipImpl(entries, true);
}

} // namespace AetherSDR
