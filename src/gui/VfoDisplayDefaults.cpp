#include "VfoDisplayDefaults.h"

#include "core/AppSettings.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

namespace AetherSDR {
namespace VfoDisplayDefaults {
namespace {

constexpr auto kRootKey       = "VfoDisplayDefaults";
constexpr auto kMarkerWidth   = "markerWidth";
constexpr auto kFilterEdges   = "filterEdgesHidden";

// 3 px is the historical default: before this document existed, a slice with no
// stored width fell out of the MarkerThin migration as "not thin" → 3. Keeping
// it means an existing operator sees no change on a profile they never touched.
constexpr int kDefaultMarkerWidth = 3;

QJsonObject load()
{
    const QByteArray stored = AppSettings::instance()
        .value(QString::fromLatin1(kRootKey), QStringLiteral("{}"))
        .toString().toUtf8();
    const QJsonDocument doc = QJsonDocument::fromJson(stored);
    return doc.isObject() ? doc.object() : QJsonObject{};
}

// Read-modify-write the whole document so one property never clobbers the
// other: the object is the unit of ownership (Principle V).
void store(const QJsonObject& object)
{
    auto& settings = AppSettings::instance();
    settings.setValue(
        QString::fromLatin1(kRootKey),
        QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
    settings.save();
}

} // namespace

int normalizeMarkerWidth(int widthPx)
{
    if (widthPx <= 0) return 0;
    if (widthPx <= 1) return 1;
    return 3;
}

int markerWidth()
{
    return normalizeMarkerWidth(
        load().value(QString::fromLatin1(kMarkerWidth)).toInt(kDefaultMarkerWidth));
}

bool filterEdgesHidden()
{
    return load().value(QString::fromLatin1(kFilterEdges)).toBool(false);
}

void setMarkerWidth(int widthPx)
{
    QJsonObject object = load();
    object.insert(QString::fromLatin1(kMarkerWidth), normalizeMarkerWidth(widthPx));
    store(object);
}

void setFilterEdgesHidden(bool hide)
{
    QJsonObject object = load();
    object.insert(QString::fromLatin1(kFilterEdges), hide);
    store(object);
}

} // namespace VfoDisplayDefaults
} // namespace AetherSDR
