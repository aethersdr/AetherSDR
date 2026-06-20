#pragma once

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

Q_SIGNALS:
    void changed(bool smartMtr);

private:
    MeterViewController();
    bool m_smartMtr{false};
};

} // namespace AetherSDR
