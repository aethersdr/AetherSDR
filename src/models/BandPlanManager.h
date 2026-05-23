#pragma once

#include <QObject>
#include <QColor>
#include <QString>
#include <QVector>

namespace AetherSDR {

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

    // A contiguous, gap-free [low, high] band region produced by merging the
    // active plan's segments. Discrete-channel bands (e.g. US 60 m: 5
    // channels separated by tens of kHz of regulatory gap) collapse to one
    // Region per legal channel; continuous bands collapse to a single
    // Region spanning the band's regulatory edges. Consumers that need
    // gap-aware TX walking (ATU pre-tune, SWR sweep) read these instead of
    // a naive min/max across segments(). (#2822)
    struct Region {
        double lowMhz;
        double highMhz;
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

    // Walk the active plan's segments whose midpoint falls inside
    // [searchLowMhz, searchHighMhz], sort by low edge, and merge adjacent
    // or overlapping segments (1 Hz adjacency tolerance) into contiguous
    // regions. Returns regions sorted by ascending lowMhz. Empty if no
    // segment matches. (#2822)
    QVector<Region> contiguousRegionsForBand(double searchLowMhz,
                                             double searchHighMhz) const;

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

} // namespace AetherSDR
