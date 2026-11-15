#include "core/CabExtractor.h"
#include "core/MsiFirmwareMetadata.h"
#include "core/OleCompoundFile.h"
#include "core/FirmwareStager.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QObject>
#include <QPair>
#include <QString>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <cstdio>
#include <utility>

using AetherSDR::CabExtractor;
using AetherSDR::MsiFirmwareMetadata;
using AetherSDR::MsiFirmwareImage;

namespace {
int failures = 0;

void check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

void append(QByteArray& bytes, quint32 value, int width)
{
    for (int i = 0; i < width; ++i) {
        bytes.append(static_cast<char>((value >> (i * 8)) & 0xffU));
    }
}

void write(QByteArray& bytes, qsizetype offset, quint32 value, int width)
{
    QByteArray field;
    append(field, value, width);
    bytes.replace(offset, width, field);
}

quint32 integer(qint64 value, int width)
{
    return static_cast<quint32>(value + (width == 2 ? 0x8000LL : 0x80000000LL));
}

using Row = QList<quint32>;

QByteArray table(const QList<Row>& rows, const QList<int>& widths)
{
    QByteArray data;
    for (qsizetype column = 0; column < widths.size(); ++column) {
        for (const Row& row : rows) {
            append(data, row.at(column), widths.at(column));
        }
    }
    return data;
}

struct File {
    QByteArray key;
    QByteArray name;
    qint64 size;
    int sequence;
};
struct Medium {
    int disk;
    int last;
    QByteArray cabinet;
};

// Original, socket-free MSI storage fixtures. File/Media schemas follow the
// Microsoft tables; the long-string and column-major encodings follow Wine's
// public storage reader. No proprietary installer or firmware is embedded.
struct Fixture {
    int refs{2};
    int sequenceWidth{4};
    bool longString{false};
    bool highStringIds{false};
    bool wrongSchema{false};
    bool objectSchema{false};
    QList<File> files{{"six-key", "SHORT.SSD|FLEX-6x00_v4.3.16.42034.ssdr", 23, 14},
                     {"bigbend-key", "FLEX-9600_v4.3.16.42034.ssdr", 300, 15}};
    QList<Medium> media{{2, 15, "#bigbend.cab"}, {1, 14, "#six.cab"}};

    MsiFirmwareMetadata::Streams streams() const
    {
        QList<QByteArray> strings;
        if (highStringIds) {
            strings.resize(65535);
        }
        if (longString) {
            strings.append(QByteArray(65539, 'L'));
        }
        const auto text = [&strings](const QByteArray& value) -> quint32 {
            if (value.isEmpty()) { return 0; }
            strings.append(value);
            return static_cast<quint32>(strings.size());
        };
        QList<Row> definitions;
        const auto schema = [&](const QByteArray& name, const QList<QByteArray>& names,
                                const QList<int>& types) {
            for (qsizetype i = 0; i < names.size(); ++i) {
                definitions.append({text(name), integer(i + 1, 2), text(names.at(i)), integer(types.at(i), 2)});
            }
        };
        schema("File", {"File", "Component_", "FileName", "FileSize", "Version", "Language", "Attributes", "Sequence"},
               {wrongSchema ? 0x104 : objectSchema ? 0x2948 : 0x2d48,
                0xd48, 0xfff, 0x104, 0x1d48, 0x1d14, 0x1502,
                sequenceWidth == 2 ? 0x502 : 0x104});
        schema("Media", {"DiskId", "LastSequence", "DiskPrompt", "Cabinet", "VolumeLabel", "Source"},
               {0x2502, sequenceWidth == 2 ? 0x502 : 0x104, 0x1f40, 0x1dff, 0x1d20, 0x1d48});
        QList<Row> fileRows;
        for (const File& file : files) {
            fileRows.append({text(file.key), text("component"), text(file.name), integer(file.size, 4),
                             0, 0, integer(0, 2), integer(file.sequence, sequenceWidth)});
        }
        QList<Row> mediaRows;
        for (const Medium& disk : media) {
            mediaRows.append({integer(disk.disk, 2), integer(disk.last, sequenceWidth), 0,
                              text(disk.cabinet), 0, 0});
        }
        MsiFirmwareMetadata::Streams out;
        append(out.stringPool, 1252U | (refs == 3 ? 0x80000000U : 0U), 4);
        for (const QByteArray& value : strings) {
            if (value.size() >= 65536) {
                append(out.stringPool, 0, 2);
                append(out.stringPool, 1, 2);
                append(out.stringPool, value.size(), 4);
            } else {
                append(out.stringPool, value.size(), 2);
                append(out.stringPool, value.isEmpty() ? 0 : 1, 2);
            }
            out.stringData.append(value);
        }
        out.columns = table(definitions, {refs, 2, refs, 2});
        out.files = table(fileRows, {refs, refs, refs, 4, refs, refs, 2, sequenceWidth});
        out.media = table(mediaRows, {2, sequenceWidth, refs, refs, refs, refs});
        return out;
    }
};

bool refused(const MsiFirmwareMetadata::Streams& streams, const QString& family = "6x00")
{
    QString error;
    return !MsiFirmwareMetadata::resolve(streams, family, error) && !error.isEmpty();
}

void checkMetadata()
{
    for (const QString& model : {QStringLiteral("FLEX-6300"), QStringLiteral("FLEX-6400M"),
                                QStringLiteral("FLEX-6700R"), QStringLiteral("FLEX-6600")}) {
        check(AetherSDR::FirmwareStager::modelToFamily(model) == "6x00",
              "Microburst and DeepEddy models select the 6x00 package");
    }
    for (const QString& model : {QStringLiteral("FLEX-8400"), QStringLiteral("FLEX-8600"),
                                QStringLiteral("AU-520M"), QStringLiteral("ML-9600W")}) {
        check(AetherSDR::FirmwareStager::modelToFamily(model) == "9600",
              "BigBend models select the MSI's 9600 package");
    }
    check(AetherSDR::FirmwareStager::modelToFamily("RT-2122").isEmpty(),
          "unverified DragonFire firmware family is refused");
    check(AetherSDR::FirmwareStager::modelToFamily("unknown").isEmpty(),
          "unknown model does not default to a firmware family");
    QTemporaryDir temp;
    check(temp.isValid(), "isolated local-file staging directory is available");
    if (!temp.isValid()) { return; }
    AetherSDR::FirmwareStager stager(nullptr, temp.path());
    int refusedUnknown = 0;
    QString unknownReason;
    QObject::connect(&stager, &AetherSDR::FirmwareStager::stageFailed,
                     [&](const QString& reason) { ++refusedUnknown; unknownReason = reason; });
    const QString wrongFile = temp.filePath("FLEX-9600_v4.3.16.ssdr");
    QFile input(wrongFile);
    check(input.open(QIODevice::WriteOnly) && input.write("Salted__test", 12) == 12,
          "manual-file mismatch fixture is written");
    input.close();
    stager.stageFromLocalFile(wrongFile, {});
    check(refusedUnknown == 1 && !stager.isStaged()
              && unknownReason == "Cannot determine the firmware family for this radio.",
          "unknown family is rejected before staging a readable local file");
    QString rejection;
    QObject::connect(&stager, &AetherSDR::FirmwareStager::stageFailed,
                     [&rejection](const QString& reason) { rejection = reason; });
    stager.stageFromLocalFile(wrongFile, "6x00");
    check(rejection.contains("FLEX-9600, not FLEX-6x00") && !stager.isStaged(),
          "a manually selected file named for the other family is not relabeled");
    QString error;
    for (int refs : {2, 3}) {
        for (int sequenceWidth : {2, 4}) {
            Fixture fixture;
            fixture.refs = refs;
            fixture.sequenceWidth = sequenceWidth;
            fixture.longString = true;
            fixture.highStringIds = refs == 3;
            const auto image = MsiFirmwareMetadata::resolve(fixture.streams(), "6x00", error);
            check(image && image->memberName == "six-key" && image->size == 23
                      && image->cabinet == "six.cab" && error.isEmpty(),
                  "metadata selects the 6x00 image across ref/sequence widths and long strings");
            const auto bigbend = MsiFirmwareMetadata::resolve(fixture.streams(), "9600", error);
            check(bigbend && bigbend->memberName == "bigbend-key" && bigbend->size == 300
                      && bigbend->cabinet == "bigbend.cab",
                  "metadata independently selects the BigBend image");
            std::swap(fixture.files[0].size, fixture.files[1].size);
            std::swap(fixture.files[0], fixture.files[1]);
            const auto reversed = MsiFirmwareMetadata::resolve(fixture.streams(), "6x00", error);
            check(reversed && reversed->memberName == "six-key" && reversed->size == 300,
                  "reversing image size and row order does not change family selection");
            const auto reversedBigbend = MsiFirmwareMetadata::resolve(fixture.streams(), "9600", error);
            check(reversedBigbend && reversedBigbend->memberName == "bigbend-key"
                      && reversedBigbend->size == 23,
                  "BigBend selection also ignores the relative image sizes");
        }
    }
    Fixture fixture;
    fixture.files.append({"other-six", "FLEX-6x00_v4.2.20.ssdr", 50, 14});
    check(!MsiFirmwareMetadata::resolve(fixture.streams(), "6x00", error)
              && error == "Ambiguous or invalid MSI firmware File entry",
          "multiple matching firmware versions are refused by the ambiguity guard");
    fixture = Fixture{};
    fixture.files[1].key = fixture.files[0].key;
    check(refused(fixture.streams()), "duplicate CAB member identity is refused");
    fixture = Fixture{};
    fixture.files.removeFirst();
    check(refused(fixture.streams()), "missing family is refused without another-family fallback");
    fixture = Fixture{};
    fixture.media[1].cabinet = "six.cab";
    check(refused(fixture.streams()), "external cabinet is refused");
    fixture = Fixture{};
    fixture.media[1].last = 20;
    check(refused(fixture.streams()), "descending media sequence ranges are refused");
    fixture = Fixture{};
    fixture.media[0].disk = 1;
    check(refused(fixture.streams()), "duplicate media disk is refused");
    fixture = Fixture{};
    fixture.files[0].sequence = 40;
    check(refused(fixture.streams()), "uncovered file sequence is refused");
    fixture = Fixture{};
    fixture.files[0].size = MsiFirmwareMetadata::kMaxFirmwareBytes + 1;
    check(refused(fixture.streams()), "oversized firmware is refused");
    fixture.files[0].size = -1;
    check(refused(fixture.streams()), "negative file size is refused");
    fixture = Fixture{};
    fixture.wrongSchema = true;
    check(refused(fixture.streams()), "wrong column types are refused");
    fixture = Fixture{};
    fixture.objectSchema = true;
    check(refused(fixture.streams()), "an OBJECT schema cannot masquerade as a string reference");
    fixture = Fixture{};
    fixture.files[0].name = "extra|SHORT.SSD|FLEX-6x00_v4.3.16.ssdr";
    check(refused(fixture.streams()), "malformed short/long filename is refused");
    fixture = Fixture{};
    fixture.files[0].name = "../FLEX-6x00_v4.3.16.ssdr";
    check(refused(fixture.streams()), "filename match is anchored to the complete filename");
    check(refused(fixture.streams(), "unknown"), "unknown radio firmware family is refused");
    const auto valid = Fixture{}.streams();
    for (int stream = 0; stream < 5; ++stream) {
        auto broken = valid;
        QList<QByteArray*> buffers{&broken.stringPool, &broken.stringData, &broken.columns, &broken.files, &broken.media};
        buffers[stream]->chop(1);
        check(refused(broken), "truncation of each metadata stream is refused");
    }
    auto broken = valid;
    broken.files[0] = static_cast<char>(0xff);
    broken.files[1] = static_cast<char>(0xff);
    check(refused(broken), "out-of-range string reference is refused");
    broken = valid;
    broken.stringPool = QByteArray::fromHex("e404000000000100");
    check(refused(broken), "truncated extended string length is refused");
}

QByteArray cabinet(const QList<QPair<QByteArray, QByteArray>>& members)
{
    QByteArray fileTable;
    QByteArray payload;
    for (const auto& [name, bytes] : members) {
        append(fileTable, bytes.size(), 4);
        append(fileTable, payload.size(), 4);
        append(fileTable, 0, 2); // folder
        append(fileTable, 0, 2); // date
        append(fileTable, 0, 2); // time
        append(fileTable, 0x20, 2);
        fileTable.append(name);
        fileTable.append('\0');
        payload.append(bytes);
    }
    QByteArray cab("MSCF");
    append(cab, 0, 4);
    append(cab, 44 + fileTable.size() + 8 + payload.size(), 4);
    append(cab, 0, 4);
    append(cab, 44, 4); // file table offset
    append(cab, 0, 4);
    append(cab, 3, 1);
    append(cab, 1, 1);
    append(cab, 1, 2); // folders
    append(cab, members.size(), 2);
    append(cab, 0, 2); // flags
    append(cab, 0, 2); // set ID
    append(cab, 0, 2); // cabinet index
    append(cab, 44 + fileTable.size(), 4); // folder data offset
    append(cab, 1, 2); // data blocks
    append(cab, 0, 2); // no compression
    cab.append(fileTable);
    append(cab, 0, 4); // optional checksum
    append(cab, payload.size(), 2);
    append(cab, payload.size(), 2);
    cab.append(payload);
    return cab;
}

void checkCabinet()
{
    Fixture fixture;
    const QByteArray wanted("Salted__six payload");
    fixture.files[0].size = wanted.size();
    QString error;
    const auto image = MsiFirmwareMetadata::resolve(fixture.streams(), "6x00", error);
    check(image.has_value(), "CAB fixture metadata resolves");
    if (!image) { return; }
    const QByteArray decoy("Salted__" + QByteArray(300, 'D'));
    const QByteArray cab = cabinet({{"wrong-salted-image", decoy}, {"six-key", wanted}});
    CabExtractor extractor;
    QByteArray extracted;
    check(extractor.extractFile(cab, image->memberName, image->size, extracted) && extracted == wanted,
          "selected MSI File key wins over the first/largest Salted__ CAB member");
    check(!extractor.extractFile(cab, "absent", wanted.size(), extracted) && extracted.isEmpty(),
          "missing member is refused without magic/size fallback");
    check(!extractor.extractFile(cab, image->memberName, image->size + 1, extracted) && extracted.isEmpty(),
          "CAB FileSize mismatch is refused");
    check(!extractor.extractFile(cabinet({{"six-key", wanted}, {"six-key", wanted}}),
                                 "six-key", wanted.size(), extracted) && extracted.isEmpty(),
          "duplicate CAB member is refused");
    check(!extractor.extractFile(cab.left(cab.size() - 5), "six-key", wanted.size(), extracted)
              && extracted.isEmpty(), "truncated cabinet cannot expose a partial firmware image");
    QList<QPair<QString, QByteArray>> all;
    check(extractor.extractAll(cab, all) && all.size() == 2
              && all.at(0).second == decoy && all.at(1).second == wanted,
          "bounded archive inspection still extracts ordinary members");
    QByteArray shortMember = cabinet({{"complete", wanted}, {"short", wanted}});
    write(shortMember, 44 + 16 + sizeof("complete"), wanted.size() + 1, 4);
    check(!extractor.extractAll(shortMember, all) && all.isEmpty(),
          "a short cabinet member discards previously extracted members");
    QByteArray oversized = cab;
    write(oversized, 44, 500U * 1024U * 1024U + 1U, 4);
    check(!extractor.extractAll(oversized, all) && all.isEmpty()
              && extractor.lastError() == "Cabinet output exceeds size limit",
          "archive inspection rejects oversized output before decompression");
    QByteArray aggregate = cab;
    write(aggregate, 44, 500U * 1024U * 1024U, 4);
    check(!extractor.extractAll(aggregate, all) && all.isEmpty()
              && extractor.lastError() == "Cabinet output exceeds size limit",
          "archive inspection bounds the aggregate of all members");
    check(extractor.extractAll(cabinet({{"empty", {}}}), all)
              && all.size() == 1 && all.first().second.isEmpty(),
          "an empty cabinet member has a zero-byte output bound");
    check(AetherSDR::OleCompoundFile::decodeMsiName(QString(QChar(0x380a))) == "A0",
          "MSI packed stream names preserve a second character of zero");
    check(AetherSDR::OleCompoundFile::decodeMsiName(QString(QChar(0x480a))) == "A",
          "MSI single-character stream names decode independently of packed pairs");
}

QByteArray compoundFile(int binarySectors = 8)
{
    constexpr quint32 kEnd = 0xfffffffeU;
    constexpr quint32 kFree = 0xffffffffU;
    QByteArray bytes(512, '\0');
    bytes.replace(0, 8, QByteArray::fromHex("d0cf11e0a1b11ae1"));
    write(bytes, 26, 3, 2);
    write(bytes, 28, 0xfffe, 2);
    write(bytes, 30, 9, 2);
    write(bytes, 32, 6, 2);
    write(bytes, 44, 1, 4); // FAT sectors
    write(bytes, 48, 0, 4); // directory sector
    write(bytes, 56, 4096, 4);
    write(bytes, 60, kEnd, 4);
    write(bytes, 68, kEnd, 4);
    for (int i = 0; i < 109; ++i) { write(bytes, 76 + i * 4, i == 0 ? 1 : kFree, 4); }
    QByteArray directory(512, '\0');
    const auto entry = [&](int index, const QString& name, int type, quint32 start, quint32 size) {
        const qsizetype offset = index * 128;
        for (qsizetype i = 0; i < name.size(); ++i) { write(directory, offset + i * 2, name.at(i).unicode(), 2); }
        write(directory, offset + 64, (name.size() + 1) * 2, 2);
        write(directory, offset + 66, type, 1);
        write(directory, offset + 116, start, 4);
        write(directory, offset + 120, size, 4);
    };
    entry(0, "Root Entry", 5, kEnd, 0);
    entry(1, "File", 2, 2, 4096);
    entry(2, QString(QChar(0x4840)) + "File", 2, 2 + binarySectors, 4096);
    QByteArray fat(512, static_cast<char>(0xff));
    write(fat, 0, kEnd, 4);
    write(fat, 4, 0xfffffffdU, 4);
    for (int i = 0; i < binarySectors; ++i) {
        write(fat, (2 + i) * 4, i + 1 == binarySectors ? kEnd : 3 + i, 4);
    }
    for (int i = 0; i < 8; ++i) {
        write(fat, (2 + binarySectors + i) * 4, i == 7 ? kEnd : 3 + binarySectors + i, 4);
    }
    bytes.append(directory);
    bytes.append(fat);
    bytes.append(QByteArray(binarySectors * 512, 'B'));
    bytes.append(QByteArray(4096, 'T'));
    return bytes;
}

void checkCompoundFileBounds()
{
    QTemporaryDir temp;
    const auto open = [&](QByteArray bytes, AetherSDR::OleCompoundFile& reader) {
        static int serial = 0;
        const QString path = temp.filePath(QString::number(++serial) + ".msi");
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) { return false; }
        file.close();
        return reader.open(path);
    };
    AetherSDR::OleCompoundFile valid;
    check(open(compoundFile(), valid), "minimal compound-file fixture opens");
    check(valid.readMsiStream("File", 4096) == QByteArray(4096, 'B')
              && valid.readMsiTable("File", 4096) == QByteArray(4096, 'T'),
          "table markers distinguish metadata from same-named binary streams");
    check(valid.readMsiStream("File", 4095).isEmpty(), "stream cap is checked before reading");
    AetherSDR::OleCompoundFile longChain;
    check(open(compoundFile(9), longChain) && longChain.readMsiStream("File", 4096).isEmpty(),
          "an overlong FAT chain cannot bypass the declared stream bound");
    AetherSDR::OleCompoundFile shortChain;
    check(open(compoundFile(7), shortChain) && shortChain.readMsiStream("File", 4096).isEmpty(),
          "a short FAT chain is refused");
    QByteArray hugeRoot = compoundFile();
    write(hugeRoot, 512 + 120, 0x7fffffffU, 4);
    AetherSDR::OleCompoundFile rootReader;
    check(!open(hugeRoot, rootReader), "oversized root mini stream is refused before allocation");
    QByteArray badShift = compoundFile();
    write(badShift, 32, 32, 2);
    AetherSDR::OleCompoundFile shiftReader;
    check(!open(badShift, shiftReader), "invalid mini-sector shift is refused before use");
}

// Small streams use the CFB mini stream; one FAT and one mini-FAT sector
// suffice for this original fixture's five metadata tables and two cabinets.
QByteArray firmwareInstaller(const QList<QPair<QString, QByteArray>>& streams)
{
    constexpr quint32 kEnd = 0xfffffffeU;
    constexpr quint32 kFree = 0xffffffffU;
    constexpr int kSector = 512;
    constexpr int kMiniSector = 64;
    const int directorySectors = ((streams.size() + 1) * 128 + kSector - 1) / kSector;
    const int miniFatSector = 1 + directorySectors;
    const int rootSector = miniFatSector + 1;
    QByteArray directory(directorySectors * kSector, '\0');
    const auto entry = [&](int index, const QString& name, int type, int start, int size) {
        const int offset = index * 128;
        for (qsizetype i = 0; i < name.size(); ++i) {
            write(directory, offset + i * 2, name.at(i).unicode(), 2);
        }
        write(directory, offset + 64, (name.size() + 1) * 2, 2);
        write(directory, offset + 66, type, 1);
        write(directory, offset + 116, start, 4);
        write(directory, offset + 120, size, 4);
    };
    QByteArray miniFat(kSector, static_cast<char>(0xff));
    QByteArray root;
    int index = 1;
    for (const auto& [name, data] : streams) {
        const int first = root.size() / kMiniSector;
        const int count = (data.size() + kMiniSector - 1) / kMiniSector;
        entry(index++, name, 2, first, data.size());
        for (int i = 0; i < count; ++i) {
            write(miniFat, (first + i) * 4, i + 1 == count ? kEnd : first + i + 1, 4);
        }
        root.append(data);
        root.append(QByteArray(count * kMiniSector - data.size(), '\0'));
    }
    entry(0, "Root Entry", 5, rootSector, root.size());
    const int rootSectors = (root.size() + kSector - 1) / kSector;
    root.append(QByteArray(rootSectors * kSector - root.size(), '\0'));
    QByteArray fat(kSector, static_cast<char>(0xff));
    write(fat, 0, 0xfffffffdU, 4);
    for (int i = 1; i <= directorySectors; ++i) {
        write(fat, i * 4, i == directorySectors ? kEnd : i + 1, 4);
    }
    write(fat, miniFatSector * 4, kEnd, 4);
    for (int i = 0; i < rootSectors; ++i) {
        write(fat, (rootSector + i) * 4, i + 1 == rootSectors ? kEnd : rootSector + i + 1, 4);
    }
    QByteArray header(kSector, '\0');
    header.replace(0, 8, QByteArray::fromHex("d0cf11e0a1b11ae1"));
    write(header, 26, 3, 2);
    write(header, 28, 0xfffe, 2);
    write(header, 30, 9, 2);
    write(header, 32, 6, 2);
    write(header, 44, 1, 4);
    write(header, 48, 1, 4);
    write(header, 56, 4096, 4);
    write(header, 60, miniFatSector, 4);
    write(header, 64, 1, 4);
    write(header, 68, kEnd, 4);
    for (int i = 0; i < 109; ++i) {
        write(header, 76 + i * 4, i == 0 ? 0 : kFree, 4);
    }
    return header + fat + directory + miniFat + root;
}

void checkStagedFilename()
{
    QTemporaryDir temp;
    check(temp.isValid(), "isolated staging directory is available");
    if (!temp.isValid()) { return; }
    const QByteArray six("Salted__six payload");
    const QByteArray bigbend("Salted__" + QByteArray(300, 'B'));
    Fixture fixture;
    fixture.files[0].size = six.size();
    fixture.files[1].size = bigbend.size();
    const MsiFirmwareMetadata::Streams metadata = fixture.streams();
    const QString marker(QChar(0x4840));
    const QByteArray installer = firmwareInstaller({
        {marker + "_StringPool", metadata.stringPool},
        {marker + "_StringData", metadata.stringData},
        {marker + "_Columns", metadata.columns},
        {marker + "File", metadata.files},
        {marker + "Media", metadata.media},
        {"six.cab", cabinet({{"decoy", bigbend}, {"six-key", six}})},
        {"bigbend.cab", cabinet({{"bigbend-key", bigbend}})}});
    const QString installerPath = temp.filePath("SmartSDR_v4.3.16_x64.msi");
    QFile input(installerPath);
    const bool saved = input.open(QIODevice::WriteOnly) && input.write(installer) == installer.size();
    input.close();
    check(saved, "synthetic MSI fixture is written");
    if (!saved) { return; }
    for (const QString& family : {QStringLiteral("6x00"), QStringLiteral("9600")}) {
        AetherSDR::FirmwareStager stager(nullptr, temp.path());
        QString error;
        QObject::connect(&stager, &AetherSDR::FirmwareStager::stageFailed,
                         [&error](const QString& reason) { error = reason; });
        const QString expectedName = "FLEX-" + family + "_v4.3.16.42034.ssdr";
        const QString expectedPath = temp.filePath(expectedName);
        QString path;
        QString completedVersion;
        QObject::connect(&stager, &AetherSDR::FirmwareStager::stageComplete,
                         [&](const QString& stagedPath, const QString& version) {
            path = stagedPath;
            completedVersion = version;
        });
        stager.stageFromLocalFile(installerPath, family);
        check(stager.isStaged() && error.isEmpty(),
              "public MSI staging flow stages each firmware family in its isolated directory");
        check(path == expectedPath && QFileInfo(path).fileName() == expectedName,
              "returned staging path preserves the MSI filename and build number");
        check(completedVersion == "4.3.16.42034" && stager.stagedVersion() == completedVersion,
              "MSI metadata supplies the full completion version rather than the installer filename");
        QFile output(expectedPath);
        check(output.open(QIODevice::ReadOnly) && output.readAll() == (family == "6x00" ? six : bigbend),
              "preserved filename contains the metadata-selected firmware bytes");
        check(!QFileInfo::exists(temp.filePath("FLEX-" + family + "_v4.3.16.ssdr")),
              "MSI staging does not create a generated filename without the build number");
    }
    check(QDir(temp.path()).entryList({"*.ssdr"}, QDir::Files).size() == 2,
          "staging creates only the two metadata-named firmware files");
}

void checkManualFilename()
{
    QTemporaryDir source;
    QTemporaryDir staging;
    check(source.isValid() && staging.isValid(), "manual staging directories are isolated");
    if (!source.isValid() || !staging.isValid()) { return; }
    const QByteArray firmware("Salted__manual firmware payload");
    for (const QString& name : {QStringLiteral("firmware.ssdr"),
                                QStringLiteral("FLEX-6x00_v4.3.16.42034.ssdr")}) {
        const QString sourcePath = source.filePath(name);
        QFile input(sourcePath);
        check(input.open(QIODevice::WriteOnly) && input.write(firmware) == firmware.size(),
              "manual firmware fixture is written");
        input.close();
        AetherSDR::FirmwareStager stager(nullptr, staging.path());
        QString error;
        QObject::connect(&stager, &AetherSDR::FirmwareStager::stageFailed,
                         [&error](const QString& reason) { error = reason; });
        QString completedPath;
        QString completedVersion;
        QObject::connect(&stager, &AetherSDR::FirmwareStager::stageComplete,
                         [&](const QString& path, const QString& version) {
            completedPath = path;
            completedVersion = version;
        });
        const QString expectedPath = staging.filePath(name);
        for (const QString& selectedPath : {sourcePath, expectedPath}) {
            stager.stageFromLocalFile(selectedPath, "6x00");
            check(error.isEmpty() && stager.isStaged() && completedPath == expectedPath
                      && stager.stagedFilePath() == expectedPath,
                  "manual staging and restaging preserve the selected filename");
            check(completedVersion == (name == "firmware.ssdr" ? "unknown" : "4.3.16.42034"),
                  "manual completion retains the parsed version or unknown");
            QFile output(expectedPath);
            check(output.open(QIODevice::ReadOnly) && output.readAll() == firmware,
                  "staging an existing staged file preserves its bytes");
        }
        QFile original(sourcePath);
        check(original.open(QIODevice::ReadOnly) && original.readAll() == firmware,
              "manual staging leaves the selected source intact");
    }
}
} // namespace

int main(int argc, char** argv)
{
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication app(argc, argv);
    checkMetadata();
    checkCabinet();
    checkCompoundFileBounds();
    checkStagedFilename();
    checkManualFilename();
    if (failures == 0) { std::puts("MSI metadata and named firmware extraction checks passed"); }
    return failures == 0 ? 0 : 1;
}
