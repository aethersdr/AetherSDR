#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QTcpSocket>
#include <QTimer>

#ifdef HAVE_SERIALPORT
#include <QSerialPort>
#endif

#include "LpMeterProtocol.h"

namespace AetherSDR {

// Peripheral transport for a TelePost LP-100A digital vector RF wattmeter —
// a standalone RS-232 instrument with no FlexRadio awareness at all, so this
// is a peripheral(lp100a) accessory alongside AcomConnection/SpeConnection/
// VkampConnection/PgxlConnection, not an IRadioBackend implementor. See
// docs/architecture/lp-100a-wattmeter-design.md for the full design note.
//
// The wire protocol is transport-agnostic — the same ASCII records arrive
// whether the peer is a local COM port or a raw-mode ser2net TCP proxy — so
// one LpMeter::ResponseParser decodes either. Only one transport is active
// at a time, selected by which connect method is called.
//
// Three things make this different from its peers, all measured rather than
// assumed (design note §Phase 0):
//
//   1. The meter NEVER pushes. Unlike the ACOM (which streams once enabled)
//      it answers 'P' and nothing else, so this class owns a poll loop.
//   2. The wire is commonly SHARED. Other clients — a Node-RED flow,
//      TelePost's own VCP — are often already polling through the same
//      ser2net port, and a second connection receives their replies. So the
//      poll loop is gated (LpMeter::PollGate): ride along when someone else
//      is polling, poll when the wire is quiet.
//   3. `connected` does not imply `working`. The meter can wedge with the
//      transport perfectly healthy — observed on real hardware: TCP up,
//      ser2net serving its banner, and zero records until the meter was
//      power-cycled. dataFlowingChanged() exists for exactly that state, and
//      it deliberately does NOT drop the link (contrast
//      VkampConnection's dead-link watchdog, which aborts the socket and so
//      makes its applet tile vanish at the moment the operator most needs it).
class LpMeterConnection : public QObject {
    Q_OBJECT

public:
    explicit LpMeterConnection(QObject* parent = nullptr);

    bool isConnected() const { return m_connected; }

    // Whether a reconnect attempt is currently counting down. Exposed so the
    // timer's LIFECYCLE is observable -- disabling auto-reconnect must cancel
    // a pending retry, and that is otherwise invisible from outside.
    bool reconnectPending() const { return m_reconnectTimer.isActive(); }
    QString description() const;  // "COM4" or "192.168.1.7:2000", for status display
    // "SERIAL" / "NETWORK" for the applet's compact source label — derived
    // from the LIVE transport, never the persisted ConnectionMode setting
    // (same divergence rationale as AcomConnection::sourceLabel(): switching
    // the Radio Setup mode combo persists the setting without disconnecting,
    // so a serial link that later auto-reconnects would be mislabelled).
    QString sourceLabel() const;

#ifdef HAVE_SERIALPORT
    // Fixed 115200 8N1, no handshake — mandated by the meter's own spec, not
    // user-configurable. Firmware before 1.2.0.0 used 38400 and before 1.0.3
    // used 19200 without dBm or SWR; neither is supported, and an older unit
    // simply produces records the parser rejects rather than misreading them.
    void connectSerial(const QString& portName);
#endif
    // Network mode assumes a RAW TCP proxy (ser2net `connection type: raw`,
    // or a Lantronix/Digi device server — the manual itself blesses this
    // route). The proxy's connect banner, if any, is discarded by the
    // parser's resync rather than needing special handling here.
    void connectNetwork(const QString& host, quint16 port);
    void disconnect();

    // Disabling must also CANCEL a retry already armed. Flipping the flag
    // alone left a pending 5 s timer to fire and reconnect once after the
    // operator had switched the option off -- the callback never re-read the
    // flag. Caught by @rfoust in review of #5320.
    //
    // NOTE: AcomConnection, SpeConnection and VkampConnection all still have
    // the flag-only version, so this is a deliberate divergence from three
    // siblings rather than an oversight; they have the same defect and it is
    // theirs to fix, not this PR's. See the design note's divergences table.
    void setAutoReconnect(bool on);

    // Operator-configured gauge ceilings. The meter reports WHICH range is
    // active but never what that range's ceiling in watts is — see
    // LpMeter::RangeCeilings.
    // `source` distinguishes a stored-config load from a deliberate context-menu
    // edit; see LpMeter::RangeTracker::CeilingSource for why that matters.
    void setRangeCeilings(const LpMeter::RangeCeilings& ceilings,
                          LpMeter::RangeTracker::CeilingSource source,
                          std::optional<int> editedRange = std::nullopt);

    const LpMeter::Reading& lastReading() const { return m_lastReading; }

    // Gauge scale for the currently displayed range, following the meter with
    // the expand-now/contract-slowly hysteresis in LpMeter::RangeTracker.
    double gaugeCeilingW() const { return m_range.ceilingW(); }
    bool   gaugeCeilingAutoExpanded() const { return m_range.ceilingAutoExpanded(); }

    // True while another client's polling is suppressing ours. Surfaced so
    // the applet can explain an update rate it does not control — otherwise
    // riding along behind a slow foreign poller looks like a bug.
    bool isRidingAlong() const { return m_gate.isRidingAlong(); }
    qint64 foreignIntervalMs() const { return m_gate.foreignIntervalMs(); }

    // False while the link is up but no valid record has arrived recently.
    bool isDataFlowing() const { return m_dataFlowing; }

signals:
    void connected();
    void disconnected();
    void connectionFailed(const QString& errorString);
    void readingUpdated(const AetherSDR::LpMeter::Reading& reading);
    // Emitted when the gauge scale changes, so the GUI re-applies ranges
    // rather than recomputing them per reading.
    void gaugeCeilingChanged(double ceilingW, bool autoExpanded);
    // Link is up but the meter has gone quiet, or has started talking again.
    void dataFlowingChanged(bool flowing);

private slots:
    void onReadyRead();

private:
    enum class Mode { None, Serial, Network };

    void onTransportUp();
    void onTransportDown();
    void onTransportError(const QString& errorString);
    void onReading(const LpMeter::Reading& reading);
    void teardownDevice();
    void armReconnect();
    void pollTick();
    void setDataFlowing(bool flowing);
    static qint64 nowMs();

    QIODevice*    m_device{nullptr};
    QTcpSocket    m_socket;
#ifdef HAVE_SERIALPORT
    QSerialPort*  m_serialPort{nullptr};
#endif

    LpMeter::ResponseParser m_parser;
    LpMeter::PollGate       m_gate;
    LpMeter::RangeTracker   m_range;
    LpMeter::RangeCeilings  m_ceilings;
    LpMeter::Reading        m_lastReading;

    Mode      m_mode{Mode::None};
    QString   m_lastSerialPort;
    QString   m_lastHost;
    quint16   m_lastPort{0};

    bool m_connected{false};
    bool m_autoReconnect{false};
    bool m_deliberateDisconnect{false};
    bool m_dataFlowing{false};
    bool m_dataFlowStateKnown{false};

    QTimer m_pollTimer;
    QTimer m_reconnectTimer;
    // Link-alive-but-meter-quiet watchdog. Restarted by every valid record.
    QTimer m_dataWatchdog;
    // Reports the poll gate's state only once it has held still. The gate can
    // legitimately flap for the first second of a connect while the foreign
    // cadence is still unknown, and logging each transition made an ordinary
    // startup read like a fault. Restarted by every transition; logs whatever
    // the state is when it finally fires.
    QTimer m_gateSettleTimer;

    // Latched so a permanently malformed stream logs once on the transition
    // into failure rather than at the ~10 Hz record rate — the same reason
    // AcomConnection latches its own decode warning, except that this
    // protocol has no checksum, so this warning is the only signal an
    // operator will ever get that the bytes are arriving but unusable.
    bool m_bytesWithoutRecords{false};
    qint64 m_lastRecordMs{-1};
    // When bytes first arrived after this connect. Needed because "no record
    // yet" is normal for the first moments of a link — the ser2net banner
    // arrives before any record — so the warning must be about a DURATION of
    // fruitless bytes, not about the first chunk.
    qint64 m_firstBytesMs{-1};
    // "Seen" is the gate's current state; "reported" is the last state
    // actually logged. Keeping them apart is what makes a flap that returns
    // to where it started produce no log at all.
    bool m_ridingAlongSeen{false};
    bool m_ridingAlongReported{false};
    bool m_gateStateReported{false};
};

}  // namespace AetherSDR
