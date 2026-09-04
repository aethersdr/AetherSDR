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
// a button that does not light, not as a UI that lies. turnTo() holds the
// same line and it matters more there: a commanded 64.3 settled at a mean
// 62.9, so writing the commanded heading into the model would put a number on
// screen that no antenna is pointing at.
//
// ONE SOCKET, TWO HALVES. The Everyware server carries its antenna switches
// and any rotator it has a controller for down the same connection, so this
// model owns both. They are not symmetric, and the asymmetry is the whole of
// the rotator design here: the switch roster is announced once and retained
// across a blip, while a rotator is announced ONLY while its controller is
// powered on and disappears by going quiet. Silence is the only signal there
// is — powering the controller off is not a socket event, and SWITCHUPDATE
// and SWITCHLOCKS carry on regardless.

#include "core/GreenHeronProtocol.h"

#include <QElapsedTimer>
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

// What the device has told us about one rotator.
//
// `heading` is the REPORTED heading and is never written from a command we
// sent. `announced` records whether a matching ADD was seen on this
// connection; it is false for a rotator known only through POINT, which should
// not happen on a healthy connection but is not worth discarding a heading
// over — POINT is what the tile acts on either way.
struct GreenHeronRotorState {
    QString name;
    double  heading{0.0};
    // POINT field 3, carried and never interpreted — see GreenHeronProtocol.h.
    QString status;
    bool    announced{false};
    // Monotonic, from the model's own clock, so a wall-clock step cannot make
    // a live rotator look silent.
    qint64  lastPointMs{0};
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
    // True when the link is down and we are still showing the last panel the
    // device sent. Blanking the grid on every blip loses more than it
    // protects. This is a STATUS-TEXT signal, not a safety one: it is false
    // during a first connect that has not replayed yet. Gate anything that
    // acts on the panel — enablement, commands — on isReady() instead.
    bool    isStale()     const { return m_stale; }
    bool    isWanted()    const { return m_wantConnected; }

    // The only flag anything may act on. TCP being up is not the same as the
    // device having told us where the relays are: a reconnect re-establishes
    // the socket long before SWITCHADD/SWITCHUPDATE/SWITCHLOCKS arrive, and in
    // that window the retained roster is a pre-drop guess. Enabling the panel
    // or sending SET_SWITCH off isConnected() alone moves real relays from
    // that guess. Both the view's enablement and selectPort() gate on THIS.
    bool    isReady()     const { return m_connected && !m_awaitingReplay; }
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
    // ready (see isReady() — connected is not enough) or the names could not
    // be encoded.
    bool selectPort(const QString& switchName, const QString& portName);

    // ── Rotators ────────────────────────────────────────────────────────────

    // Rotators currently being reported, in the order POINT first named them.
    //
    // EMPTY MEANS "NO ROTATOR", NOT "NOT CONNECTED". A server whose rotator
    // controller is switched off sends neither ADD nor POINT while its switch
    // records carry on, so absence is how the device reports absence and this
    // list takes it at face value. Names go away again when they fall silent —
    // see isRotorLive().
    QStringList rotorNames() const;

    // Last reported state for one rotator. A default-constructed value back
    // means we have never had a POINT for that name on this connection.
    GreenHeronRotorState rotorState(const QString& name) const;

    // True while `name` has reported inside GreenHeron::kRotorSilentAfterMs.
    //
    // This is the ONLY presence test that exists. Powering a rotator's
    // controller off produces no protocol record at all, so a client without
    // its own silence timeout inherits a phantom: a heading sitting on screen
    // for an antenna nobody is driving, beside a Turn button that would
    // happily aim a controller that is off.
    bool isRotorLive(const QString& name) const;

    // Ask `rotorName` to turn to `degrees`. Fire-and-forget; the local model
    // is deliberately not updated, because the commanded heading is not where
    // the antenna ends up. Returns false when the rotator is not live on THIS
    // connection or the command could not be encoded.
    //
    // THERE IS NO STOP OR PARK VERB IN THIS PROTOCOL. A rotation that starts
    // cannot be recalled in software — the only way to stop it is to walk to
    // the controller. Every caller must therefore make choosing a heading and
    // sending it two separate gestures; nothing may transmit on a single
    // click, hover, or drag.
    bool turnTo(const QString& rotorName, double degrees);

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
    void onRotorSilenceTick();

private:
    void openSocket();
    void teardownSocket();
    void applyRecord(const GreenHeron::Record& record);
    // Called for every record the device names a switch in: this connection
    // has now vouched for the panel, so the replay gate opens.
    void noteAuthoritativeState();
    void scheduleReconnect();
    void setLastError(const QString& message);

    QTcpSocket* m_socket{nullptr};
    QTimer*     m_keepAliveTimer{nullptr};
    QTimer*     m_reconnectTimer{nullptr};
    // A dropped SYN leaves QTcpSocket in HostLookup/Connecting with no error
    // and no timeout of its own, which is precisely the half-open shape this
    // device's idle-drop behaviour produces. Bound it ourselves.
    QTimer*     m_connectTimer{nullptr};
    // A rotator going quiet produces no record, so its disappearance cannot
    // arrive as a signal — there is nothing to signal with. Poll for it.
    QTimer*     m_rotorSilenceTimer{nullptr};

    // Monotonic since construction. QDateTime would let an NTP step or a
    // suspend/resume decide a live rotator is silent, or hide one that is.
    QElapsedTimer m_clock;

    QString m_host;
    quint16 m_port{GreenHeron::kDefaultPort};
    bool    m_wantConnected{false};
    bool    m_connected{false};
    bool    m_stale{false};
    // Set for every socket we open, cleared by the first record the device
    // sends on it. Deliberately not folded into m_stale: the view needs to
    // tell "link down, showing the last panel" from "link up, waiting to hear"
    // — they read differently to an operator, and only one of them is going to
    // resolve on its own.
    bool    m_awaitingReplay{false};
    QString m_lastError;

    QByteArray m_pending;   // partial tail between reads

    QHash<QString, GreenHeronSwitchState> m_switches;
    QStringList m_announced;   // first-seen SWITCHADD order; append-only

    // Rotators, keyed by name, and the order POINT first named them.
    //
    // CLEARED FOR EVERY SOCKET WE OPEN, unlike the switch roster which is
    // retained and flagged stale. The switch panel degrades honestly — a grey
    // grid nobody can click — but a retained heading beside an enabled Turn
    // button is the phantom above, and this is also what makes turnTo()'s
    // liveness guard a statement about the CURRENT connection rather than
    // about some socket that is already gone.
    QHash<QString, GreenHeronRotorState> m_rotors;
    QStringList m_rotorOrder;
    // Names seen in an ADD, in arrival order, append-only. Held separately
    // because ADD ARRIVES BEFORE THE FIRST POINT — measured on the reference
    // hardware, where the announcement lands within ~100 ms of connect and the
    // first heading about a second later. Annotating the rotor table directly
    // from the ADD branch would therefore annotate nothing, every time. POINT
    // consults this list when it creates the entry.
    QStringList m_announcedDevices;
    // Names that were live at the last tick, so the poll only redraws on a
    // transition instead of once a second forever.
    QStringList m_liveRotors;

    int m_backoffMs{kBackoffStartMs};

    static constexpr int kBackoffStartMs = 1000;
    static constexpr int kBackoffMaxMs   = 30000;
    static constexpr int kConnectTimeoutMs = 10000;
    // Ten times finer than kRotorSilentAfterMs, so the blanking lands within a
    // second of the deadline without polling hard.
    static constexpr int kRotorPollMs = 1000;
};

} // namespace AetherSDR
