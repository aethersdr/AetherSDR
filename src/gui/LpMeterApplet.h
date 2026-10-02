#pragma once

#include "core/LpMeterProtocol.h"

#include <QWidget>

class QLabel;

namespace AetherSDR {

class HGauge;

// TelePost LP-100A wattmeter applet, a sibling of AcomApplet/SpeApplet, not a
// CrossNeedleMeterApplet extension (docs/architecture/lp-100a-wattmeter-design.md).
// Two gauges: power and SWR; reflected is not reported and is never derived
// (fields are not always coherent). Impedance shows |Z| and |phase| only — the
// sign is not on the wire. dBm is shown as reported, negatives included.
class LpMeterApplet : public QWidget {
    Q_OBJECT

public:
    explicit LpMeterApplet(QWidget* parent = nullptr);

    // One decoded record. Display updates are throttled to 10 Hz (see
    // m_labelTimer) rather than repainting per record: the meter runs at
    // 10 Hz and every repaint re-announces accessibility text.
    void setReading(const LpMeter::Reading& reading);

    // Gauge ceiling for the meter's currently active range. The meter reports
    // WHICH range it is in but never that range's ceiling in watts, so this
    // comes from LpMeterConnection's RangeTracker.
    void setPowerCeiling(double ceilingW, bool autoExpanded);

    void setSource(const QString& text);   // "SERIAL" / "NETWORK"
    void setConnected(bool connected);     // resets readouts on disconnect

    // Link up but the meter has gone quiet. Distinct from disconnected: the
    // meter can wedge with the transport perfectly healthy, and an operator
    // staring at a frozen gauge deserves to be told which of the two it is.
    void setDataFlowing(bool flowing);

    // Another client is polling the same shared port and we are riding along
    // on its replies. Surfaced because it dictates the update rate, which the
    // operator would otherwise read as a fault.
    void setRidingAlong(bool riding, qint64 foreignIntervalMs);

    LpMeter::RangeCeilings ceilings() const { return m_ceilings; }
    void setCeilings(const LpMeter::RangeCeilings& ceilings);

signals:
    // Emitted when the operator edits a range ceiling from the context menu.
    // editedRange is 0..2 for one range, or -1 when resetting all ranges.
    void ceilingsChanged(const AetherSDR::LpMeter::RangeCeilings& ceilings,
                         int editedRange);

private:
    void showContextMenu(const QPoint& pos);
    void editCeiling(int rangeIndex);
    void refreshLabels();      // 10 Hz throttled
    void refreshStatusPill();
    void applyDimming();
    QString diagnosticTooltip() const;

    HGauge* m_pwrGauge{nullptr};
    HGauge* m_swrGauge{nullptr};

    QLabel* m_pwrValue{nullptr};
    QLabel* m_swrValue{nullptr};
    QLabel* m_statusPill{nullptr};
    QLabel* m_sourceLabel{nullptr};
    QLabel* m_callsignLabel{nullptr};

    QLabel* m_dbmLabel{nullptr};
    QLabel* m_rlLabel{nullptr};
    QLabel* m_zLabel{nullptr};
    QLabel* m_phaseLabel{nullptr};
    QLabel* m_rangeLabel{nullptr};
    QLabel* m_modeLabel{nullptr};

    LpMeter::Reading       m_reading;
    LpMeter::RangeCeilings m_ceilings;

    bool   m_connected{false};
    bool   m_dataFlowing{false};
    bool   m_ridingAlong{false};
    qint64 m_foreignIntervalMs{-1};
    double m_ceilingW{0.0};
    bool   m_ceilingAutoExpanded{false};

    // Readings arrive at 10 Hz and every field can change on each one, so
    // labels refresh on a timer with a dirty flag rather than per record —
    // the same throttle AcomApplet uses, and for the same reason: repainting
    // and re-announcing accessible names at full record rate floods screen
    // readers.
    class QTimer* m_labelTimer{nullptr};
    bool m_labelsDirty{false};
};

}  // namespace AetherSDR
