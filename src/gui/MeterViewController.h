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
    // Cached (the setters keep them in sync with DisplaySettings) so these stay
    // off the per-packet path — txMeter() is read on every meter update — rather
    // than re-parsing the Display JSON blob from AppSettings on each call.
    bool showExtremes() const { return m_showExtremes; }
    DisplaySettings::ExtremesSpeed extremesSpeed() const { return m_extremesSpeed; }
    DisplaySettings::MeterValues showValues() const { return m_showValues; }
    DisplaySettings::TxMeter txMeter() const { return m_txMeter; }
    bool showTxMeterType() const { return m_showTxMeterType; }
    void setShowExtremes(bool on);
    void setExtremesSpeed(DisplaySettings::ExtremesSpeed v);
    void setShowValues(DisplaySettings::MeterValues v);
    void setTxMeter(DisplaySettings::TxMeter v);
    void setShowTxMeterType(bool on);

Q_SIGNALS:
    void changed(bool smartMtr);
    void extremesChanged();
    // The TX-meter choice changed; flags re-evaluate which input to push (the TX
    // meter swaps the SmartMTR input, not its options, so it gets its own signal).
    void txMeterChanged();

private:
    MeterViewController();
    bool m_smartMtr{false};
    bool m_showExtremes{false};
    DisplaySettings::ExtremesSpeed m_extremesSpeed{DisplaySettings::ExtremesSpeed::Medium};
    DisplaySettings::MeterValues m_showValues{DisplaySettings::MeterValues::None};
    DisplaySettings::TxMeter m_txMeter{DisplaySettings::TxMeter::None};
    bool m_showTxMeterType{false};
};

} // namespace AetherSDR
