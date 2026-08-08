#include "BandPlanManager.h"
#include "core/AppSettings.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

namespace AetherSDR {

BandPlanManager::BandPlanManager(QObject* parent)
    : QObject(parent)
{
}

void BandPlanManager::loadPlans()
{
    m_plans.clear();

    // Load all bundled plans from Qt resources
    QDir resDir(":/bandplans");
    const auto entries = resDir.entryList({"*.json"}, QDir::Files, QDir::Name);
    for (const auto& filename : entries) {
        PlanData plan;
        if (loadPlanFromJson(":/bandplans/" + filename, plan))
            m_plans.append(std::move(plan));
    }

    // Activate saved plan or default to first available
    QString saved = AppSettings::instance().value("BandPlanName", "ARRL (US)").toString();
    bool found = false;
    for (const auto& p : m_plans) {
        if (p.name == saved) { found = true; break; }
    }
    if (!found && !m_plans.isEmpty())
        saved = m_plans.first().name;

    setActivePlan(saved);
    loadKiwiDxSpots();
}

void BandPlanManager::loadKiwiDxSpots()
{
    m_kiwiDxSpots.clear();

    QFile f(":/dist.dx_community.json");
    if (!f.open(QIODevice::ReadOnly)) {
        return;
    }

    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError) {
        return;
    }

    QJsonObject root = doc.object();
    QJsonArray dxArray = root.value("dx").toArray();
    for (const auto& itemVal : dxArray) {
        if (!itemVal.isArray()) continue;
        QJsonArray entry = itemVal.toArray();
        if (entry.size() < 4) continue;

        KiwiDxSpot spot;
        const double freqKhz = entry.at(0).toDouble();
        if (freqKhz <= 0.0) continue;
        spot.freqMhz = freqKhz / 1000.0;
        spot.mode = entry.at(1).toString();
        spot.name = entry.at(2).toString();
        spot.notes = entry.at(3).toString();

        if (entry.size() >= 5 && entry.at(4).isObject()) {
            QJsonObject opts = entry.at(4).toObject();
            if (opts.contains("lo")) spot.loOffsetHz = opts.value("lo").toInt();
            if (opts.contains("hi")) spot.hiOffsetHz = opts.value("hi").toInt();
            if (opts.contains("b0")) spot.timeBegin = opts.value("b0").toInt();
            if (opts.contains("e0")) spot.timeEnd = opts.value("e0").toInt();
            if (opts.contains("d0")) spot.dowMask = opts.value("d0").toInt();
            if (opts.contains("p"))  spot.extParam = opts.value("p").toString();
        }

        m_kiwiDxSpots.append(spot);
    }

    emit kiwiDxSpotsChanged();
}

QString BandPlanManager::normalizeSpotMode(const QString& rawMode)
{
    const QString mode = rawMode.trimmed().toUpper();
    if (mode == QStringLiteral("CWN") || mode == QStringLiteral("CW")) {
        return QStringLiteral("CW");
    }
    if (mode == QStringLiteral("AMN") || mode == QStringLiteral("AM") || mode == QStringLiteral("DRM")) {
        return QStringLiteral("AM");
    }
    if (mode == QStringLiteral("SAM") || mode == QStringLiteral("SAL") || mode == QStringLiteral("SAU")) {
        return QStringLiteral("SAM");
    }
    if (mode == QStringLiteral("NBFM") || mode == QStringLiteral("FM")) {
        return QStringLiteral("FM");
    }
    if (mode == QStringLiteral("NFM") || mode == QStringLiteral("DFM")
        || mode == QStringLiteral("USB") || mode == QStringLiteral("LSB")
        || mode == QStringLiteral("RTTY") || mode == QStringLiteral("DIGU")
        || mode == QStringLiteral("DIGL") || mode == QStringLiteral("FDV")) {
        return mode;
    }
    return QString();
}

void BandPlanManager::setActivePlan(const QString& name)
{
    for (const auto& p : m_plans) {
        if (p.name == name) {
            m_activeName = name;
            m_segments = p.segments;
            m_spots = p.spots;
            m_licenseClasses = p.licenseClasses;
            AppSettings::instance().setValue("BandPlanName", name);
            AppSettings::instance().save();
            emit planChanged();
            return;
        }
    }
}

QStringList BandPlanManager::availablePlans() const
{
    QStringList names;
    for (const auto& p : m_plans)
        names << p.name;
    return names;
}

QVector<BandPlanManager::Region>
BandPlanManager::contiguousRegionsForBand(double searchLowMhz,
                                         double searchHighMhz) const
{
    return contiguousRegionsForBand(searchLowMhz, searchHighMhz, QString());
}

QVector<BandPlanManager::Region>
BandPlanManager::contiguousRegionsForBand(double searchLowMhz,
                                         double searchHighMhz,
                                         const QString& allowedClass) const
{
    QVector<Region> regions;
    for (const auto& seg : m_segments) {
        const double mid = (seg.lowMhz + seg.highMhz) / 2.0;
        if (mid < searchLowMhz || mid > searchHighMhz) continue;
        // Filter BEFORE the merge so that an allowed segment adjacent to a
        // disallowed one stays as two separate regions instead of merging
        // into one and reintroducing the disallowed centers. Empty-license
        // segments (BCN markers, general-purpose) always pass through. (#2649)
        if (!allowedClass.isEmpty() && !seg.license.isEmpty()) {
            const QString needle = allowedClass.trimmed();
            const auto codes = seg.license.split(',', Qt::SkipEmptyParts);
            bool allowed = false;
            for (const auto& code : codes) {
                if (code.trimmed() == needle) { allowed = true; break; }
            }
            if (!allowed) continue;
        }
        regions.append({seg.lowMhz, seg.highMhz});
    }
    if (regions.isEmpty()) return regions;

    std::sort(regions.begin(), regions.end(),
              [](const Region& a, const Region& b) { return a.lowMhz < b.lowMhz; });
    QVector<Region> merged;
    merged.append(regions.front());
    constexpr double kAdjacencyEpsMhz = 1.0e-6;  // 1 Hz
    for (int i = 1; i < regions.size(); ++i) {
        if (regions[i].lowMhz <= merged.last().highMhz + kAdjacencyEpsMhz) {
            merged.last().highMhz = std::max(merged.last().highMhz, regions[i].highMhz);
        } else {
            merged.append(regions[i]);
        }
    }
    return merged;
}

bool BandPlanManager::loadPlanFromJson(const QString& path, PlanData& out)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;

    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError) {
        qWarning() << "BandPlanManager: JSON parse error in" << path << err.errorString();
        return false;
    }

    QJsonObject root = doc.object();
    out.name = root.value("name").toString();
    if (out.name.isEmpty()) return false;

    // Optional license-class table: {"T": "Technician", "G": "General", ...}.
    // Plans without the block leave the map empty so the dialog hides its
    // class-filter UI entirely. (#2649)
    const QJsonObject classes = root.value("license_classes").toObject();
    for (auto it = classes.begin(); it != classes.end(); ++it) {
        if (it.value().isString())
            out.licenseClasses.insert(it.key(), it.value().toString());
    }

    // Parse segments
    const auto segs = root.value("segments").toArray();
    for (const auto& val : segs) {
        QJsonObject obj = val.toObject();
        Segment seg;
        seg.lowMhz = obj.value("low").toDouble();
        seg.highMhz = obj.value("high").toDouble();
        seg.label = obj.value("label").toString();
        seg.license = obj.value("license").toString();
        seg.color = QColor(obj.value("color").toString());
        if (seg.lowMhz < seg.highMhz && seg.color.isValid())
            out.segments.append(seg);
    }

    // Parse spots
    const auto spots = root.value("spots").toArray();
    for (const auto& val : spots) {
        QJsonObject obj = val.toObject();
        Spot spot;
        spot.freqMhz = obj.value("freq").toDouble();
        spot.label = obj.value("label").toString();
        if (spot.freqMhz > 0)
            out.spots.append(spot);
    }

    return true;
}

} // namespace AetherSDR
