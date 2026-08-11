#pragma once

// GreenHeronModel — connection and state tracking for a Green Heron
// "Everyware" antenna-switch server (TCP, default port 10000).
//
// This is a PERIPHERAL accessory model, in the same class as
// AntennaGeniusModel / TgxlConnection / AcomConnection: it speaks a
// standalone device's own transport, knows nothing about any radio family,
// and works with whatever radio the operator has connected (or none). It is
// NOT behind the IRadioBackend radio seam and must never be put there.
//
// The wire format, its provenance, and the caps this model relies on all live
// in core/GreenHeronProtocol.h, which is pure and testable. This file adds
// only the socket, the timers, and the state they produce.
//
// THREADING: none. The Python implementation this is ported from used a
// reader thread plus a keepalive thread and a mutex-guarded snapshot; a
// QTcpSocket with readyRead plus two QTimers covers the same ground on the
// main thread with no locking at all, and adding threads is not a change to
// make in passing (AGENTS.md, Autonomous Agent Boundaries).
//
// STATE IS NEVER INFERRED FROM COMMANDS WE SEND. selectPort() transmits and
// returns; the device is the sole authority on where the relays actually are
// and republishes within ~123 ms. A relay that fails to move must show up as
// a button that does not light, not as a UI that lies.

#include "core/GreenHeronProtocol.h"

#include <QHash>
#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>

class QTcpSocket;
class QTimer;

namespace AetherSDR {

// Everything the device has told us about one switch.
struct GreenHeronSwitchState {
    QString     name;
    QStringList ports;            // roster, in the order SWITCHADD listed it
    QString     selected;         // the port this switch is currently on
    QStringList locks;            // slot N ↔ announcement order, NOT display order
    QString     wirelessSignal;   // link signal; parsed, carried, not displayed
};

class GreenHeronModel : public QObject {
    Q_OBJECT

public:
    explicit GreenHeronModel(QObject* parent = nullptr);
    ~GreenHeronModel() override;

    // Connect, and keep reconnecting with backoff until disconnectFromHost().
    void connectToHost(const QString& host,
                       quint16 port = GreenHeron::kDefaultPort);
    void disconnectFromHost();

    bool    isConnected() const { return m_connected; }
    // True when the link has dropped but the last known panel is still shown.
    // Blanking the grid on every blip loses more than it protects.
    bool    isStale()     const { return m_stale; }
    bool    isWanted()    const { return m_wantConnected; }
    QString host()        const { return m_host; }
    quint16 port()        const { return m_port; }
    QString lastError()   const { return m_lastError; }

    // Switch names in a stable display order (trailing number numerically,
    // unnumbered last). The device announces its roster unsorted — 1, 3, 2, 4
    // on the reference hardware — which would make the columns jump around.
    QStringList displayOrder() const;

    // Switch names in the order the device announced them. This is NOT
    // display order and is not cosmetic: it is the index basis for lock
    // slots. Append-only, so a reconnect replaying the roster cannot reorder
    // it.
    QStringList announcedOrder() const { return m_announced; }

    GreenHeronSwitchState switchState(const QString& name) const;

    // Which OTHER switch, if any, currently holds each port — port name →
    // holder switch name. Empty when this switch has no lock record yet.
    //
    // Slot N carries the port selected by the Nth switch IN ANNOUNCEMENT
    // ORDER. Verified with a case that discriminates between the two
    // candidate orderings: selecting Beam-15 on AS-84F-3 put it in slot 1,
    // and AS-84F-3 is announced second — sorted order would predict slot 2.
    // An earlier test using AS-84F-4 appeared to confirm sorted order and
    // proved nothing, because AS-84F-4 is index 3 under both orderings.
    QMap<QString, QString> locksBySwitch(const QString& name) const;

    // Ask the device to put `switchName` on `portName`. Fire-and-forget; the
    // local model is deliberately not updated. Returns false if we are not
    // connected or the names could not be encoded.
    bool selectPort(const QString& switchName, const QString& portName);

signals:
    // The panel changed in some way a view should redraw for.
    void panelChanged();
    // connected / stale / wanted transitions.
    void connectionStateChanged();
    void errorOccurred(const QString& message);

private slots:
    void onSocketConnected();
    void onSocketDisconnected();
    void onSocketReadyRead();
    void onSocketError();
    void onKeepAlive();
    void onReconnect();
    void onConnectTimeout();

private:
    void openSocket();
    void teardownSocket();
    void applyRecord(const GreenHeron::Record& record);
    void scheduleReconnect();
    void setLastError(const QString& message);

    QTcpSocket* m_socket{nullptr};
    QTimer*     m_keepAliveTimer{nullptr};
    QTimer*     m_reconnectTimer{nullptr};
    // A dropped SYN leaves QTcpSocket in HostLookup/Connecting with no error
    // and no timeout of its own, which is precisely the half-open shape this
    // device's idle-drop behaviour produces. Bound it ourselves.
    QTimer*     m_connectTimer{nullptr};

    QString m_host;
    quint16 m_port{GreenHeron::kDefaultPort};
    bool    m_wantConnected{false};
    bool    m_connected{false};
    bool    m_stale{false};
    QString m_lastError;

    QByteArray m_pending;   // partial tail between reads

    QHash<QString, GreenHeronSwitchState> m_switches;
    QStringList m_announced;   // first-seen SWITCHADD order; append-only

    int m_backoffMs{kBackoffStartMs};

    static constexpr int kBackoffStartMs = 1000;
    static constexpr int kBackoffMaxMs   = 30000;
    static constexpr int kConnectTimeoutMs = 10000;
};

} // namespace AetherSDR
