#include "BandPlanManager.h"
#include "core/AppSettings.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace MasterSDR {

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
}

void BandPlanManager::setActivePlan(const QString& name)
{
    for (const auto& p : m_plans) {
        if (p.name == name) {
            m_activeName = name;
            m_segments = p.segments;
            m_spots = p.spots;
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

} // namespace MasterSDR
