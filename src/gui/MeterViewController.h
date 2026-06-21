#pragma once

#include "DisplaySettings.h"

#include <QObject>

namespace AetherSDR {

// Global, app-wide owner of the VFO meter-view choice (standard S-Meter vs the
// SmartMTR component).  Every VfoWidget reads the current value and connects to
// changed() so a toggle on one flag updates all open flags live; the value is
// persisted (via DisplaySettings) and restored at startup.  Single source of
// truth: the meter menu in each flag calls setSmartMtr() here.
class MeterViewController : public QObject {
    Q_OBJECT
public:
    static MeterViewController& instance();

    bool smartMtr() const { return m_smartMtr; }
    void setSmartMtr(bool on);

    // SmartMTR-only extremes options. Persisted via DisplaySettings (the store);
    // changing any of them emits extremesChanged() so every open VFO flag can
    // re-push the options to its SmartMtrWidget — same live-broadcast model as the
    // meter-view choice above. The meter-menu controls in each flag call these.
    bool showExtremes() const { return DisplaySettings::showExtremes(); }
    DisplaySettings::ExtremesSpeed extremesSpeed() const
    {
        return DisplaySettings::extremesSpeed();
    }
    DisplaySettings::MeterValues showValues() const
    {
        return DisplaySettings::showValues();
    }
    void setShowExtremes(bool on);
    void setExtremesSpeed(DisplaySettings::ExtremesSpeed v);
    void setShowValues(DisplaySettings::MeterValues v);

Q_SIGNALS:
    void changed(bool smartMtr);
    void extremesChanged();

private:
    MeterViewController();
    bool m_smartMtr{false};
};

} // namespace AetherSDR
