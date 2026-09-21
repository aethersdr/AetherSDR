#pragma once

#include <QObject>
#include <QString>
#include <QTcpSocket>
#include <QTimer>

#include "Kpa1500Protocol.h"

namespace AetherSDR {

// Peripheral transport for an Elecraft KPA1500 amplifier (#4097).
//
// A standalone Ethernet device with no FlexRadio awareness at all — the
// Flex radio neither discovers it nor proxies its telemetry the way it does
// for a PGXL/TGXL — so this is a peripheral(kpa1500) accessory alongside
// AcomConnection/SpeConnection/VkampConnection, not an IRadioBackend
// implementor. See docs/architecture/kpa1500-amplifier-design.md.
//
// TCP only, on purpose. The amp also exposes a UDP server on the same port
// accepting the identical command set, but every control path here is a
// command whose delivery matters — OPERATE, tune, antenna select, and above
// all the keying refresh — and UDP gives no delivery signal at all.
//
// The amp does not broadcast spontaneously: every reading below arrives
// because this class asked for it. m_pollTimer walks the query set; see
// fastPollCommands()/slowPollCommands() for why it is split in two.
class Kpa1500Connection : public QObject {
    Q_OBJECT

public:
    explicit Kpa1500Connection(QObject* parent = nullptr);
    ~Kpa1500Connection() override;

    bool isConnected() const { return m_connected; }
    QString description() const;  // "192.168.1.60:1500", for status display

    void connectNetwork(const QString& host, quint16 port = Kpa1500::kDefaultPort);
    void disconnect();
    void setAutoReconnect(bool on) { m_autoReconnect = on; }

    const Kpa1500::Status& lastStatus() const { return m_status; }

    // Control (host -> amp). Every one is a no-op, logged, when not
    // connected — the amp is the authority on its own state, so nothing
    // here latches an optimistic value; the applet repaints when the poll
    // reply lands (constitution Principle II applied to a peripheral).
    void setOperate(bool operate);
    void setPower(bool on);
    void startTune();
    void cancelTune();
    void setAtuMode(Kpa1500::AtuMode mode);
    void setAtuInline(bool inLine);
    void selectAntenna(int port);  // 1-3; out of range is a no-op, logged
    void clearFault();

    // ── Network keying ───────────────────────────────────────────────────
    //
    // NOT wired to any transmit path in this revision, and deliberately so:
    // whether Ethernet keying is trusted as the SOLE keying path (vs.
    // remaining paired with the hardware KEY IN line, which runs in
    // parallel with `^TX` rather than instead of it) is an open maintainer
    // decision on #4097, and it is the decision that governs an emission.
    // Constitution Principle VI says a path that can transmit fails closed
    // when intent is not unambiguous; nothing in this build calls key(), so
    // the fail-closed state is the shipped state. These exist so the
    // mechanism the maintainer has to rule on is reviewable code with tests
    // rather than a description.
    //
    // key() never emits the unbounded `^TX;`. It sends `^TX<kKeyTimeoutSec>;`
    // and refreshes it every kKeyRefreshMs for as long as the key is held,
    // so if this application dies or the LAN drops the amp unkeys itself
    // when the timeout expires instead of staying keyed indefinitely.
    void key();
    void unkey();
    bool isKeyed() const { return m_keyed; }

signals:
    void connected();
    void disconnected();
    void connectionFailed(const QString& errorString);
    // Full current snapshot, emitted when a reply actually changed
    // something. Consumers read the fields they care about; unset optionals
    // mean "the amp has not reported this yet", not zero.
    void statusUpdated(const AetherSDR::Kpa1500::Status& status);
    // Non-zero fault code newly reported by `^FL`. Separate from
    // statusUpdated so a fault can drive an alert without every consumer
    // diffing the snapshot itself.
    void faultRaised(int code);
    void faultCleared();

private slots:
    void onReadyRead();

private:
    void onTransportUp();
    void onTransportDown();
    void onTransportError(const QString& errorString);
    void onMessage(const Kpa1500::Message& message);
    void poll();
    bool sendRaw(const QByteArray& frame);
    void armReconnect();
    void stopKeyRefresh();

    QTcpSocket m_socket;
    Kpa1500::MessageParser m_parser;
    Kpa1500::Status m_status;

    QString m_lastHost;
    quint16 m_lastPort{Kpa1500::kDefaultPort};

    bool m_connected{false};
    bool m_autoReconnect{false};
    bool m_deliberateDisconnect{false};
    bool m_keyed{false};

    // Guards against re-emitting faultRaised() on every poll for a fault
    // that is already displayed — `^FL` answers with the same non-zero code
    // for as long as the fault stands.
    int m_lastFaultCode{0};

    QTimer m_reconnectTimer;
    static constexpr int kReconnectIntervalMs = 5000;

    // An unreachable IP otherwise falls back to the OS TCP connect timeout
    // (~21s on Windows) before connectionFailed() ever fires, which reads
    // as a hung dialog rather than a slow one. Same epoch-guarded
    // singleShot pattern as VkampConnection/DxClusterClient (#2380: a stale
    // timeout from attempt N must never abort a later attempt that already
    // succeeded).
    static constexpr int kConnectTimeoutMs = 8000;
    int m_connectEpoch{0};

    QTimer m_pollTimer;
    static constexpr int kPollIntervalMs = 500;
    // Rotates one slow query per tick alongside the fast set, so the meters
    // stay at the full poll rate while the configuration readbacks (band,
    // antenna, ATU, operate/power state) still refresh a couple of times a
    // second without putting the whole query set on the wire every 500ms.
    int m_slowPollIndex{0};

    // Keying refresh. kKeyRefreshMs is well under kKeyTimeoutSec so a
    // single dropped refresh does not unkey mid-transmission, while a dead
    // application still releases the amp within kKeyTimeoutSec.
    QTimer m_keyRefreshTimer;
    static constexpr int kKeyTimeoutSec = 10;
    static constexpr int kKeyRefreshMs = 3000;
};

}  // namespace AetherSDR
