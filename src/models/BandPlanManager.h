#pragma once

#include <QObject>
#include <QColor>
#include <QString>
#include <QVector>

namespace MasterSDR {

// Manages selectable band plan overlays for the spectrum display.
// Plans are loaded from bundled Qt resource JSON files.
// Replaces the compile-time kBandPlan[]/kBandSpots[] arrays (#425).
class BandPlanManager : public QObject {
    Q_OBJECT

public:
    struct Segment {
        double lowMhz;
        double highMhz;
        QString label;
        QString license;
        QColor color;
    };

    struct Spot {
        double freqMhz;
        QString label;
    };

    explicit BandPlanManager(QObject* parent = nullptr);

    // Load all bundled plans from Qt resources
    void loadPlans();

    // Active plan
    QString activePlanName() const { return m_activeName; }
    void setActivePlan(const QString& name);
    const QVector<Segment>& segments() const { return m_segments; }
    const QVector<Spot>& spots() const { return m_spots; }

    // Available plans (display names)
    QStringList availablePlans() const;

signals:
    void planChanged();

private:
    struct PlanData {
        QString name;
        QVector<Segment> segments;
        QVector<Spot> spots;
    };

    bool loadPlanFromJson(const QString& path, PlanData& out);

    QVector<PlanData> m_plans;
    QString m_activeName;
    QVector<Segment> m_segments;  // active plan's segments
    QVector<Spot> m_spots;        // active plan's spots
};

} // namespace MasterSDR
