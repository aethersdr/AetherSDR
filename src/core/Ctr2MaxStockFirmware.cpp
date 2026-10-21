#include "Ctr2MaxStockFirmware.h"

#include "EspFlashTarget.h"
#include "ZipArchive.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QRegularExpression>

#include <algorithm>

namespace AetherSDR {

namespace {

constexpr quint8 kImageMagic = 0xE9;
constexpr quint16 kEsp32S3ChipId = 9;
constexpr int kImageHeaderSize = 24;
constexpr int kMaxSegments = 16;
constexpr quint16 kPartitionMagic = 0x50AA;   // 0xAA 0x50, little-endian
constexpr quint16 kPartitionMd5Magic = 0xEBEB;
constexpr int kPartitionRowSize = 32;
constexpr quint8 kTypeApp = 0;
constexpr quint8 kTypeData = 1;
constexpr quint8 kSubtypeOta0 = 0x10;
constexpr quint8 kSubtypeOtaLast = 0x1F;
constexpr quint8 kSubtypeOtadata = 0;
constexpr quint32 kBootloaderOffset = 0x0;
// The image Lynovation publishes is a few MB; a zip of it with its manuals
// some more.
constexpr qint64 kMaxImage = 16 * 1024 * 1024;

// The rows the CTR2-Max firmware relies on: its slot, the boot selection,
// ESP-IDF's key-value store, and the volume holding its settings files and
// the registration. The unit's must be
// where the file's are.
const char* const kReliedOn[] = {"nvs", "otadata", "app0", "spiffs"};

QString tr(const char* text)
{
    return QCoreApplication::translate("Ctr2MaxStockFirmware", text);
}

quint32 le32(const QByteArray& b, qsizetype at)
{
    const auto u = [&](qsizetype i) { return static_cast<quint32>(static_cast<quint8>(b[at + i])); };
    return u(0) | (u(1) << 8) | (u(2) << 16) | (u(3) << 24);
}

quint16 le16(const QByteArray& b, qsizetype at)
{
    return static_cast<quint16>(static_cast<quint8>(b[at]) | (static_cast<quint8>(b[at + 1]) << 8));
}

const Ctr2MaxStockFirmware::Partition* find(const QList<Ctr2MaxStockFirmware::Partition>& rows,
                                            const QString& name)
{
    for (const Ctr2MaxStockFirmware::Partition& p : rows) {
        if (p.name == name)
            return &p;
    }
    return nullptr;
}

bool same(const Ctr2MaxStockFirmware::Partition& a, const Ctr2MaxStockFirmware::Partition& b)
{
    return a.type == b.type && a.subtype == b.subtype && a.offset == b.offset && a.size == b.size;
}

// The firmware's version as it shows it ("v1.01.03"): a string of its own
// in the image.
QString findVersion(const QByteArray& app)
{
    static const QRegularExpression kVersion(QStringLiteral("^v\\d{1,2}\\.\\d{2}\\.\\d{2}[a-z]?$"));
    qsizetype start = 0;
    while (start < app.size()) {
        qsizetype end = app.indexOf('\0', start);
        if (end < 0)
            end = app.size();
        if (end - start >= 8 && end - start <= 12) {
            const QString s = QString::fromLatin1(app.mid(start, end - start));
            if (kVersion.match(s).hasMatch())
                return s;
        }
        start = end + 1;
    }
    return {};
}

} // namespace

namespace {

// The unit's app slots, ota_0 first: the boot selection counts them, so they
// must be the unit's own, and none may be missing.
bool appSlotsOf(const QList<Ctr2MaxStockFirmware::Partition>& unit,
                QList<Ctr2MaxStockFirmware::Partition>* out)
{
    out->clear();
    for (const Ctr2MaxStockFirmware::Partition& p : unit) {
        if (p.type == kTypeApp && p.subtype >= kSubtypeOta0 && p.subtype <= kSubtypeOtaLast)
            out->append(p);
    }
    std::sort(out->begin(), out->end(),
              [](const auto& a, const auto& b) { return a.subtype < b.subtype; });
    for (int i = 0; i < out->size(); ++i) {
        if ((*out)[i].subtype != kSubtypeOta0 + i)
            return false;
    }
    return !out->isEmpty();
}

// Everything but the boot selection, and but writable (app0 for an install):
// the bootloader, the table itself, and every row of the unit's table.
void protectAllBut(const QList<Ctr2MaxStockFirmware::Partition>& unit, const QString& writable,
                   EspFirmwareManifest* m)
{
    m->protect.append({tr("the bootloader"), kBootloaderOffset,
                       Ctr2MaxStockFirmware::kTableOffset, {}});
    m->protect.append({tr("the partition table"),
                       Ctr2MaxStockFirmware::kTableOffset, EspFlashTarget::kSectorSize, {}});
    for (const Ctr2MaxStockFirmware::Partition& p : unit) {
        if (p.name != writable && p.name != QLatin1String("otadata"))
            m->protect.append({p.name, p.offset, p.size, {}});
    }
}

} // namespace

QList<Ctr2MaxStockFirmware::Partition> Ctr2MaxStockFirmware::readPartitionTable(const QByteArray& table)
{
    QList<Partition> rows;
    for (qsizetype at = 0; at + kPartitionRowSize <= table.size(); at += kPartitionRowSize) {
        const quint16 magic = le16(table, at);
        if (magic != kPartitionMagic)
            break;   // 0xFFFF (the end) or kPartitionMd5Magic
        Partition p;
        p.type = static_cast<quint8>(table[at + 2]);
        p.subtype = static_cast<quint8>(table[at + 3]);
        p.offset = le32(table, at + 4);
        p.size = le32(table, at + 8);
        const QByteArray label = table.mid(at + 12, 16);
        p.name = QString::fromLatin1(label.left(label.indexOf('\0') < 0 ? 16 : label.indexOf('\0')));
        rows.append(p);
    }
    return rows;
}

qint64 Ctr2MaxStockFirmware::imageLength(const QByteArray& data, QString* problem)
{
    const auto bad = [problem](const QString& why) {
        *problem = why;
        return qint64(0);
    };
    if (data.size() < kImageHeaderSize || static_cast<quint8>(data[0]) != kImageMagic)
        return bad(tr("It is not an ESP32 firmware image."));
    if (le16(data, 12) != kEsp32S3ChipId)
        return bad(tr("It is firmware for another chip, not the CTR2-Max's ESP32-S3."));
    const int segments = static_cast<quint8>(data[1]);
    const bool hashAppended = static_cast<quint8>(data[23]) == 1;
    if (segments == 0 || segments > kMaxSegments)
        return bad(tr("The firmware image is damaged."));
    qint64 at = kImageHeaderSize;
    for (int i = 0; i < segments; ++i) {
        if (at + 8 > data.size())
            return bad(tr("The firmware image is incomplete."));
        at += 8 + qint64(le32(data, at + 4));
        if (at > data.size())
            return bad(tr("The firmware image is incomplete."));
    }
    // The checksum byte ends a 16-byte block; the SHA-256 follows.
    const qint64 body = (at + 1 + 15) / 16 * 16;
    if (!hashAppended)
        return bad(tr("The firmware image carries no checksum to check it by."));
    if (body + 32 > data.size())
        return bad(tr("The firmware image is incomplete."));
    if (QCryptographicHash::hash(QByteArrayView(data).first(body), QCryptographicHash::Sha256)
        != data.mid(body, 32))
        return bad(tr("The firmware image is damaged: its checksum does not match."));
    return body + 32;
}

bool Ctr2MaxStockFirmware::read(const QByteArray& file, Ctr2MaxStockFirmware* out, QString* problem)
{
    const auto bad = [problem](const QString& why) {
        *problem = why;
        return false;
    };
    QByteArray image = file;
    if (file.startsWith("PK\x03\x04")) {
        // Lynovation's zip: one .bin beside manuals and settings files.
        ZipReadLimits limits;
        limits.maxEntryUncompressedBytes = kMaxImage;
        limits.maxTotalUncompressedBytes = 8 * quint64(kMaxImage);
        QString error;
        const QMap<QString, QByteArray> entries = readZipEntries(file, &error, limits);
        if (entries.isEmpty())
            return bad(tr("The zip file could not be read: %1").arg(error));
        QList<QByteArray> images;
        for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
            if (it.key().endsWith(QLatin1String(".bin"), Qt::CaseInsensitive)
                && !it.key().startsWith(QLatin1String("__MACOSX/")))
                images.append(it.value());
        }
        if (images.size() != 1)
            return bad(images.isEmpty()
                           ? tr("The zip file holds no firmware (.bin) file.")
                           : tr("The zip file holds more than one .bin file; open the CTR2-Max "
                                "firmware's .bin file itself."));
        image = images.first();
    }
    if (image.size() > kMaxImage)
        return bad(tr("The file is too large to be CTR2-Max firmware."));

    // A merged flash image: the bootloader at 0x0, the partition table, and
    // the application in app0.
    QString why;
    if (imageLength(image.mid(kBootloaderOffset), &why) == 0
        || image.size() < qsizetype(kTableOffset + kTableSize))
        return bad(tr("This is not a CTR2-Max firmware file as Lynovation publishes it (a whole "
                      "flash image). %1")
                       .arg(why));
    const QList<Partition> table = readPartitionTable(image.mid(kTableOffset, kTableSize));
    const Partition* app0 = find(table, QStringLiteral("app0"));
    if (!app0 || app0->type != kTypeApp || app0->subtype != kSubtypeOta0
        || app0->offset >= image.size())
        return bad(tr("This file has no CTR2-Max firmware where the CTR2-Max starts it from."));
    const QByteArray rest = image.mid(app0->offset);
    const qint64 length = imageLength(rest, &why);
    if (length == 0)
        return bad(why);
    if (length > app0->size)
        return bad(tr("The firmware does not fit the CTR2-Max's firmware slot."));
    const QByteArray app = rest.left(length);
    if (!app.contains("CTR2-Max") || !app.contains("Lynovation"))
        return bad(tr("This is ESP32-S3 firmware, but not the CTR2-Max's."));
    out->m_version = findVersion(app);
    out->m_app = app;
    out->m_image = image;
    out->m_table = table;
    return true;
}

bool Ctr2MaxStockFirmware::newer(const QString& a, const QString& b)
{
    static const QRegularExpression kShape(QStringLiteral("^v(\\d+)\\.(\\d+)\\.(\\d+)([a-z]?)$"));
    if (a.isEmpty() || b.isEmpty())
        return !a.isEmpty() && b.isEmpty();
    const QRegularExpressionMatch ma = kShape.match(a);
    const QRegularExpressionMatch mb = kShape.match(b);
    if (!ma.hasMatch() || !mb.hasMatch())
        return a > b;
    for (int i = 1; i <= 3; ++i) {
        const int x = ma.captured(i).toInt(), y = mb.captured(i).toInt();
        if (x != y)
            return x > y;
    }
    return ma.captured(4) > mb.captured(4);
}

bool Ctr2MaxStockFirmware::plan(const QByteArray& unitTable, quint32 flashSize,
                                EspFirmwareManifest* manifest, QHash<QString, QByteArray>* files,
                                QString* problem) const
{
    const QList<Partition> unit = readPartitionTable(unitTable);
    for (const char* name : kReliedOn) {
        const Partition* u = find(unit, QString::fromLatin1(name));
        const Partition* f = find(m_table, QString::fromLatin1(name));
        if (!u || !f || !same(*u, *f)) {
            *problem = tr("This CTR2-Max's flash is not laid out as the firmware expects (%1). "
                          "Nothing was written.")
                           .arg(QString::fromLatin1(name));
            return false;
        }
    }
    const Partition* app0 = find(unit, QStringLiteral("app0"));
    const Partition* otadata = find(unit, QStringLiteral("otadata"));
    if (app0->type != kTypeApp || app0->subtype != kSubtypeOta0 || otadata->type != kTypeData
        || otadata->subtype != kSubtypeOtadata || quint64(m_app.size()) > app0->size) {
        *problem = tr("This CTR2-Max's flash is not laid out as the firmware expects. Nothing was "
                      "written.");
        return false;
    }
    QList<Partition> otaSlots;
    if (!appSlotsOf(unit, &otaSlots)) {
        *problem = tr("This CTR2-Max's flash is not laid out as the firmware expects. Nothing "
                      "was written.");
        return false;
    }

    EspFirmwareManifest m;
    m.product = tr("CTR2-Max firmware");
    m.version = m_version;
    m.device = QStringLiteral("CTR2-Max");
    m.chip = QStringLiteral("ESP32-S3");
    m.flashSize = flashSize;
    // Everything but app0 and the boot selection is kept: the bootloader,
    // the table itself, the settings, the registration, AetherCTR2.
    protectAllBut(unit, QStringLiteral("app0"), &m);
    m.require.append({tr("partition table"), kTableOffset, kTableSize,
                      QCryptographicHash::hash(unitTable, QCryptographicHash::Sha256)});
    m.otadata = otadata->offset;
    for (const Partition& p : otaSlots)
        m.appSlots.append(p.offset);
    m.ownSlots = {0};
    const QString file = QStringLiteral("app.bin");
    m.images.append({file, app0->offset, static_cast<quint32>(m_app.size()),
                     QCryptographicHash::hash(m_app, QCryptographicHash::Sha256)});
    if (!EspFirmwareManifest::checkPlan(m, problem))
        return false;
    *manifest = m;
    files->clear();
    files->insert(file, m_app);
    return true;
}

bool Ctr2MaxStockFirmware::planStart(const QByteArray& unitTable, quint32 flashSize,
                                     bool aetherCtr2, EspFirmwareManifest* manifest,
                                     QString* problem)
{
    const QList<Partition> unit = readPartitionTable(unitTable);
    const Partition* otadata = find(unit, QStringLiteral("otadata"));
    QList<Partition> otaSlots;
    if (!otadata || otadata->type != kTypeData || otadata->subtype != kSubtypeOtadata
        || !appSlotsOf(unit, &otaSlots)) {
        *problem = tr("This CTR2-Max's flash is not laid out as expected. Nothing was written.");
        return false;
    }
    if (aetherCtr2 && otaSlots.size() < 2) {
        *problem = tr("AetherCTR2 is not installed on this CTR2-Max.");
        return false;
    }
    EspFirmwareManifest m;
    m.product = aetherCtr2 ? QStringLiteral("AetherCTR2") : tr("CTR2-Max firmware");
    m.device = QStringLiteral("CTR2-Max");
    m.chip = QStringLiteral("ESP32-S3");
    m.flashSize = flashSize;
    protectAllBut(unit, {}, &m);
    m.require.append({tr("partition table"), kTableOffset, kTableSize,
                      QCryptographicHash::hash(unitTable, QCryptographicHash::Sha256)});
    m.otadata = otadata->offset;
    for (const Partition& p : otaSlots)
        m.appSlots.append(p.offset);
    if (aetherCtr2) {
        for (int i = 1; i < otaSlots.size(); ++i)
            m.ownSlots.append(i);
    } else {
        m.ownSlots = {0};
    }
    if (!EspFirmwareManifest::checkPlan(m, problem))
        return false;
    *manifest = m;
    return true;
}

} // namespace AetherSDR
