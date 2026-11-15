#include "MsiFirmwareMetadata.h"

#include <QList>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <array>

namespace AetherSDR {
namespace {

constexpr qsizetype kMaxStrings = 262144;
constexpr qsizetype kMaxRows = 100000;

quint32 unsignedCell(const QByteArray& data, qsizetype offset, int width)
{
    quint32 value = 0;
    for (int i = 0; i < width; ++i) {
        value |= static_cast<quint32>(static_cast<quint8>(data.at(offset + i))) << (8 * i);
    }
    return value;
}

class Strings {
public:
    bool load(const QByteArray& pool, const QByteArray& data)
    {
        if (pool.size() < 4 || pool.size() % 4 != 0 || pool.size() / 4 > kMaxStrings) {
            return false;
        }
        const quint32 header = unsignedCell(pool, 0, 4);
        const quint32 codepage = header & 0x7fffffffU;
        // Only identifying fields are interpreted, and they must be ASCII.
        // Unrelated strings (including localized license text) stay as bytes.
        if (codepage != 0 && codepage != 1252 && codepage != 65001) {
            return false;
        }
        width = (header & 0x80000000U) ? 3 : 2;
        values.append(QByteArray());
        qsizetype consumed = 0;
        for (qsizetype offset = 4; offset < pool.size(); offset += 4) {
            quint32 length = unsignedCell(pool, offset, 2);
            const quint32 refs = unsignedCell(pool, offset + 2, 2);
            if (length == 0 && refs == 0) {
                values.append(QByteArray());
                continue;
            }
            // A long string uses one logical ID but two physical pool entries.
            if (length == 0) {
                if (pool.size() - offset < 8) {
                    return false;
                }
                length = unsignedCell(pool, offset + 4, 4);
                offset += 4;
            }
            if (length == 0 || length > static_cast<quint64>(data.size() - consumed)) {
                return false;
            }
            values.append(refs ? data.mid(consumed, length) : QByteArray());
            consumed += length;
        }
        return consumed == data.size();
    }

    bool get(quint32 id, QByteArray& value) const
    {
        if (id >= static_cast<quint32>(values.size()) || (id != 0 && values.at(id).isNull())) {
            return false;
        }
        value = values.at(id);
        return true;
    }

    int width{2};

private:
    QList<QByteArray> values;
};

struct Column {
    QByteArray name;
    int width{};
    bool string{};
};

class Table {
public:
    Table(const QByteArray& bytes, const QList<Column>& schema)
        : data(bytes), columns(schema)
    {
        qsizetype rowSize = 0;
        for (const Column& column : columns) {
            offsets.append(rowSize);
            rowSize += column.width;
        }
        if (rowSize > 0 && data.size() % rowSize == 0) {
            rows = data.size() / rowSize;
            valid = rows > 0 && rows <= kMaxRows;
        }
    }

    quint32 raw(qsizetype row, qsizetype column) const
    {
        return unsignedCell(data, offsets.at(column) * rows + row * columns.at(column).width,
                            columns.at(column).width);
    }

    bool text(qsizetype row, qsizetype column, const Strings& strings, QByteArray& value) const
    {
        return strings.get(raw(row, column), value);
    }

    std::optional<qint64> integer(qsizetype row, qsizetype column) const
    {
        const quint32 value = raw(row, column);
        if (value == 0) {
            return std::nullopt;
        }
        return static_cast<qint64>(value) - (columns.at(column).width == 2 ? 0x8000LL : 0x80000000LL);
    }

    qsizetype rows{};
    bool valid{false};

private:
    const QByteArray& data;
    QList<Column> columns;
    QList<qsizetype> offsets;
};

bool schemaFor(const Table& definitions, const Strings& strings, const QByteArray& name,
               const QList<Column>& expected, QList<Column>& schema)
{
    schema = expected;
    QSet<qsizetype> found;
    for (qsizetype row = 0; row < definitions.rows; ++row) {
        QByteArray tableName;
        if (!definitions.text(row, 0, strings, tableName)) {
            return false;
        }
        if (tableName != name) {
            continue;
        }
        const std::optional<qint64> number = definitions.integer(row, 1);
        const std::optional<qint64> type = definitions.integer(row, 3);
        QByteArray columnName;
        if (!number || !type || *number < 1 || *number > schema.size()
            || !definitions.text(row, 2, strings, columnName)) {
            return false;
        }
        const qsizetype column = *number - 1;
        const qint64 typeClass = *type & 0x0c00;
        const bool isString = typeClass == 0x0c00;
        const int width = isString ? strings.width : static_cast<int>(*type & 0xff);
        if (found.contains(column) || columnName != expected.at(column).name
            || (*type & 0xc000) != 0 || (*type & 0x0100) == 0
            || isString != expected.at(column).string
            || (isString && (*type & 0xff) == 0)
            || (!isString && width != 2 && width != 4)
            || (!isString && typeClass != (width == 2 ? 0x0400 : 0))
            || (expected.at(column).width != 0 && width != expected.at(column).width)) {
            return false;
        }
        schema[column].width = width;
        found.insert(column);
    }
    return found.size() == expected.size();
}

bool asciiName(const QByteArray& value)
{
    return !value.isEmpty() && std::all_of(value.cbegin(), value.cend(), [](char ch) {
        const quint8 byte = static_cast<quint8>(ch);
        return byte >= 0x20 && byte <= 0x7e && ch != '/' && ch != '\\' && ch != ':';
    });
}

} // namespace

std::optional<MsiFirmwareImage> MsiFirmwareMetadata::resolve(const Streams& streams,
                                                           const QString& family,
                                                           QString& error)
{
    error.clear();
    const auto fail = [&error](const QString& reason) -> std::optional<MsiFirmwareImage> {
        error = reason;
        return std::nullopt;
    };
    if (family != QStringLiteral("6x00") && family != QStringLiteral("9600")) {
        return fail(QStringLiteral("Unsupported firmware family"));
    }
    for (const QByteArray* bytes : std::array{&streams.stringPool, &streams.stringData,
                                             &streams.columns, &streams.files, &streams.media}) {
        if (bytes->isEmpty() || bytes->size() > kMaxMetadataBytes) {
            return fail(QStringLiteral("Missing or oversized MSI firmware metadata"));
        }
    }
    Strings strings;
    if (!strings.load(streams.stringPool, streams.stringData)) {
        return fail(QStringLiteral("Invalid or unsupported MSI string table"));
    }
    const Table definitions(streams.columns, {{"Table", strings.width, true}, {"Number", 2, false},
                                              {"Name", strings.width, true}, {"Type", 2, false}});
    QList<Column> fileSchema;
    QList<Column> mediaSchema;
    if (!definitions.valid
        || !schemaFor(definitions, strings, "File",
                      {{"File", strings.width, true}, {"Component_", strings.width, true},
                       {"FileName", strings.width, true}, {"FileSize", 4, false},
                       {"Version", strings.width, true}, {"Language", strings.width, true},
                       {"Attributes", 2, false}, {"Sequence", 0, false}}, fileSchema)
        || !schemaFor(definitions, strings, "Media",
                      {{"DiskId", 2, false}, {"LastSequence", 0, false},
                       {"DiskPrompt", strings.width, true}, {"Cabinet", strings.width, true},
                       {"VolumeLabel", strings.width, true}, {"Source", strings.width, true}}, mediaSchema)) {
        return fail(QStringLiteral("Invalid or unsupported MSI File/Media schema"));
    }
    const Table files(streams.files, fileSchema);
    const Table media(streams.media, mediaSchema);
    if (!files.valid || !media.valid) {
        return fail(QStringLiteral("Invalid MSI File/Media table length"));
    }
    const QRegularExpression filePattern(
        QStringLiteral(R"(^FLEX-%1_v[0-9]+\.[0-9]+\.[0-9]+(?:\.[0-9]+)?\.ssdr$)").arg(family),
        QRegularExpression::CaseInsensitiveOption);
    std::optional<MsiFirmwareImage> image;
    qint64 sequence = 0;
    QSet<QByteArray> fileKeys;
    for (qsizetype row = 0; row < files.rows; ++row) {
        QByteArray key;
        QByteArray fileName;
        if (!files.text(row, 0, strings, key) || !asciiName(key) || fileKeys.contains(key)
            || !files.text(row, 2, strings, fileName)) {
            return fail(QStringLiteral("Invalid or duplicate MSI File identity"));
        }
        fileKeys.insert(key);
        // FileName may be SHORTNAM.SSD|LongFileName.ssdr.
        const QByteArray longName = fileName.mid(fileName.indexOf('|') + 1);
        if (!asciiName(longName) || !filePattern.match(QString::fromLatin1(longName)).hasMatch()) {
            continue;
        }
        const std::optional<qint64> size = files.integer(row, 3);
        const std::optional<qint64> fileSequence = files.integer(row, 7);
        if (image || !size || *size <= 0 || *size > kMaxFirmwareBytes
            || !fileSequence || *fileSequence <= 0 || fileName.count('|') > 1) {
            return fail(QStringLiteral("Ambiguous or invalid MSI firmware File entry"));
        }
        image = MsiFirmwareImage{QString::fromLatin1(longName), QString::fromLatin1(key), {}, *size};
        sequence = *fileSequence;
    }
    if (!image) {
        return fail(QStringLiteral("FLEX-%1 firmware is missing from the MSI File table").arg(family));
    }
    struct Disk { qint64 id; qint64 last; QByteArray cabinet; };
    QList<Disk> disks;
    QSet<qint64> diskIds;
    for (qsizetype row = 0; row < media.rows; ++row) {
        const std::optional<qint64> id = media.integer(row, 0);
        const std::optional<qint64> last = media.integer(row, 1);
        QByteArray cabinet;
        if (!id || *id <= 0 || diskIds.contains(*id) || !last || *last <= 0
            || !media.text(row, 3, strings, cabinet)) {
            return fail(QStringLiteral("Invalid or duplicate MSI Media entry"));
        }
        diskIds.insert(*id);
        disks.append({*id, *last, cabinet});
    }
    std::sort(disks.begin(), disks.end(), [](const Disk& a, const Disk& b) { return a.id < b.id; });
    qint64 previous = 0;
    for (const Disk& disk : disks) {
        if (disk.last < previous) {
            return fail(QStringLiteral("MSI Media sequence ranges overlap"));
        }
        if (sequence > previous && sequence <= disk.last) {
            if (!disk.cabinet.startsWith('#') || !asciiName(disk.cabinet.mid(1))) {
                return fail(QStringLiteral("Firmware cabinet must be embedded in the MSI"));
            }
            image->cabinet = QString::fromLatin1(disk.cabinet.mid(1));
        }
        previous = disk.last;
    }
    if (image->cabinet.isEmpty()) {
        return fail(QStringLiteral("Firmware sequence is not covered by MSI Media"));
    }
    return image;
}

} // namespace AetherSDR
