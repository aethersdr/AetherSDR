#include "EspFirmwareManifest.h"

#include "EspFlashTarget.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace AetherSDR {

namespace {

constexpr int kFormat = 1;
constexpr quint32 kOtadataSize = 2 * EspFlashTarget::kSectorSize;

QString tr(const char* text)
{
    return QCoreApplication::translate("EspFirmwareManifest", text);
}

// A number given as a JSON number or a "0x..." / decimal string.
bool number(const QJsonValue& v, quint32* out)
{
    if (v.isDouble()) {
        const double d = v.toDouble();
        if (d < 0 || d > 4294967295.0 || d != static_cast<double>(static_cast<quint64>(d)))
            return false;
        *out = static_cast<quint32>(d);
        return true;
    }
    if (!v.isString())
        return false;
    bool ok = false;
    const quint64 n = v.toString().toULongLong(&ok, 0);
    if (!ok || n > 0xFFFFFFFFull)
        return false;
    *out = static_cast<quint32>(n);
    return true;
}

bool sha256(const QJsonValue& v, QByteArray* out)
{
    const QByteArray hex = v.toString().toLatin1();
    *out = QByteArray::fromHex(hex);
    return out->size() == 32 && out->toHex() == hex.toLower();
}

quint32 sectorFloor(quint32 a)
{
    return a & ~(EspFlashTarget::kSectorSize - 1);
}

quint64 sectorCeil(quint64 a)
{
    const quint64 s = EspFlashTarget::kSectorSize;
    return (a + s - 1) / s * s;
}

bool overlaps(quint64 a, quint64 aEnd, quint64 b, quint64 bEnd)
{
    return a < bEnd && b < aEnd;
}

} // namespace

bool EspFirmwareManifest::checkPlan(const EspFirmwareManifest& m, QString* problem)
{
    // No two writes may share a sector (writing erases whole sectors): an
    // image erased by the next one, or one under the boot selection, would
    // install a broken plan with every hash intact.
    struct Write {
        quint64 start, end;
        QString what;
    };
    QList<Write> writes;
    for (const Image& image : m.images)
        writes.append({sectorFloor(image.offset), sectorCeil(quint64(image.offset) + image.size),
                       image.file});
    const quint64 otaStart = sectorFloor(m.otadata);
    const quint64 otaEnd = sectorCeil(quint64(m.otadata) + kOtadataSize);
    writes.append({otaStart, otaEnd, QStringLiteral("boot.otadata")});
    for (int a = 0; a < writes.size(); ++a) {
        for (int b = a + 1; b < writes.size(); ++b) {
            if (overlaps(writes[a].start, writes[a].end, writes[b].start, writes[b].end)) {
                *problem = tr("The firmware manifest is invalid: %1 and %2 overlap.")
                               .arg(writes[a].what, writes[b].what);
                return false;
            }
        }
    }
    // A required region must stay as it is, so nothing that is always
    // rewritten may cover it: the boot selection changes on every install
    // and start. (Images are checked against the bytes they would leave.)
    for (const Region& r : m.require) {
        if (overlaps(r.offset, quint64(r.offset) + r.size, otaStart, otaEnd)) {
            *problem = tr("The firmware manifest is invalid: %1 must stay as it is, but the "
                          "boot selection is rewritten.")
                           .arg(r.name.isEmpty() ? tr("a required region") : r.name);
            return false;
        }
    }
    return true;
}

bool EspFirmwareManifest::parse(const QByteArray& json, EspFirmwareManifest* out, QString* problem)
{
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (!doc.isObject()) {
        *problem = tr("The firmware manifest is not valid JSON.");
        return false;
    }
    const QJsonObject o = doc.object();
    if (o.value("format").toInt() != kFormat) {
        *problem = tr("This firmware needs a newer AetherSDR to install.");
        return false;
    }
    EspFirmwareManifest m;
    m.product = o.value("product").toString();
    m.version = o.value("version").toString();
    m.device = o.value("device").toString();
    m.chip = o.value("chip").toString();
    const auto bad = [&](const QString& what) {
        *problem = tr("The firmware manifest is invalid: %1.").arg(what);
        return false;
    };
    if (m.product.isEmpty() || m.chip.isEmpty())
        return bad(QStringLiteral("product and chip are required"));
    if (!number(o.value("flashSize"), &m.flashSize) || m.flashSize == 0
        || m.flashSize % EspFlashTarget::kSectorSize)
        return bad(QStringLiteral("flashSize"));

    const auto regions = [&](const char* key, bool hashed, QList<Region>* list) {
        for (const QJsonValue& v : o.value(QLatin1String(key)).toArray()) {
            const QJsonObject r = v.toObject();
            Region region;
            region.name = r.value("name").toString();
            if (!number(r.value("offset"), &region.offset) || !number(r.value("size"), &region.size)
                || region.size == 0
                || quint64(region.offset) + region.size > m.flashSize)
                return false;
            if (hashed && !sha256(r.value("sha256"), &region.sha256))
                return false;
            list->append(region);
        }
        return true;
    };
    if (!regions("protect", false, &m.protect))
        return bad(QStringLiteral("a protected region"));
    if (!regions("require", true, &m.require))
        return bad(QStringLiteral("a required region"));

    const QJsonObject boot = o.value("boot").toObject();
    if (!number(boot.value("otadata"), &m.otadata) || m.otadata % EspFlashTarget::kSectorSize
        || quint64(m.otadata) + kOtadataSize > m.flashSize)
        return bad(QStringLiteral("boot.otadata"));
    for (const QJsonValue& v : boot.value("slots").toArray()) {
        quint32 slot = 0;
        if (!number(v, &slot) || slot % EspFlashTarget::kSectorSize || slot >= m.flashSize)
            return bad(QStringLiteral("boot.slots"));
        m.appSlots.append(slot);
    }
    for (const QJsonValue& v : boot.value("ownSlots").toArray()) {
        const int i = v.toInt(-1);
        if (i < 0 || i >= m.appSlots.size() || m.ownSlots.contains(i))
            return bad(QStringLiteral("boot.ownSlots"));
        m.ownSlots.append(i);
    }
    if (m.appSlots.isEmpty() || m.ownSlots.isEmpty())
        return bad(QStringLiteral("boot needs slots and ownSlots"));

    for (const QJsonValue& v : o.value("images").toArray()) {
        const QJsonObject r = v.toObject();
        Image image;
        image.file = r.value("file").toString();
        if (image.file.isEmpty() || image.file.contains(QLatin1Char('/'))
            || image.file.contains(QLatin1Char('\\')) || image.file.startsWith(QLatin1Char('.')))
            return bad(QStringLiteral("an image file name"));
        if (!number(r.value("offset"), &image.offset) || !number(r.value("size"), &image.size)
            || image.size == 0 || image.offset % EspFlashTarget::kSectorSize
            || sectorCeil(quint64(image.offset) + image.size) > m.flashSize
            || !sha256(r.value("sha256"), &image.sha256))
            return bad(QStringLiteral("image %1").arg(image.file));
        m.images.append(image);
    }
    if (m.images.isEmpty())
        return bad(QStringLiteral("no images"));

    // Nothing this manifest writes may touch a protected region, counting
    // whole sectors: writing erases them.
    const auto clear = [&](quint64 start, quint64 end, const QString& what) {
        for (const Region& p : m.protect) {
            if (overlaps(sectorFloor(static_cast<quint32>(start)), sectorCeil(end), p.offset,
                         quint64(p.offset) + p.size)) {
                *problem = tr("The firmware manifest is invalid: %1 would overwrite %2.")
                               .arg(what, p.name.isEmpty() ? tr("a protected region") : p.name);
                return false;
            }
        }
        return true;
    };
    for (const Image& image : m.images) {
        if (!clear(image.offset, quint64(image.offset) + image.size, image.file))
            return false;
    }
    if (!clear(m.otadata, quint64(m.otadata) + kOtadataSize, QStringLiteral("boot.otadata")))
        return false;
    for (int i : m.ownSlots) {
        if (!clear(m.appSlots[i], quint64(m.appSlots[i]) + 1, QStringLiteral("own slot %1").arg(i)))
            return false;
    }

    if (!checkPlan(m, problem))
        return false;
    *out = m;
    return true;
}

} // namespace AetherSDR
