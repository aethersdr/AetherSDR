#pragma once

#include "core/LpMeterProtocol.h"

#include <QWidget>

class QLabel;

namespace AetherSDR {

class HGauge;

// Dedicated applet for a TelePost LP-100A digital vector RF wattmeter — a
// sibling of AcomApplet/SpeApplet/VkampApplet, not a variant of any of them,
// and deliberately NOT an extension of CrossNeedleMeterApplet. See
// docs/architecture/lp-100a-wattmeter-design.md.
//
// Two gauges rather than the amplifier family's three: the LP-100A reports
// power and SWR, and does NOT report reflected power. The protocol authority
// does not resolve whether field 0 is forward or net power. Reflected could
// be derived from the pair, but the fields are not always mutually coherent
// (see LpMeter::Reading::coherent), so nothing here cross-derives between
// fields — every number displayed is one the meter actually sent.
//
// Two display rules this class exists to enforce, both from the protocol
// rather than from taste:
//
//   1. Impedance is shown as |Z| and |phase|, NEVER as signed reactance or an
//      R+jX form. The sign is not on the wire at all; the manual has the
//      operator recover it by QSY-ing and watching the slope. Rendering a
//      sign here would be inventing data.
//   2. dBm is shown as the meter reports it, including negative values. The
//      reference Node-RED flow converts to dBW and clamps at 0, which
//      silently discards everything below 1 W — the QRP end where a dB
//      readout earns its keep.
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
