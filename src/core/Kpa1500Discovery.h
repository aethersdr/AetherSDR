#pragma once

#include <QHash>
#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QUdpSocket>

#include "Kpa1500Protocol.h"

namespace AetherSDR {

// Finds Elecraft KPA1500 amplifiers on the local networks. The amp has no
// discovery protocol, but its UDP server answers `^SN;` from any client,
// even while asleep, so a sweep sends that one query to the amp's port on
// every host address of each local IPv4 network (bounded, see
// sweepTargets()) and reports each address that answers with a serial
// number. Sends only queries; nothing here changes an amplifier's state.
class Kpa1500Discovery : public QObject {
    Q_OBJECT

public:
    struct Found {
        QHostAddress address;
        QString serialNumber;
        QString firmware;  // empty if the ^RV reply was lost
    };

    explicit Kpa1500Discovery(QObject* parent = nullptr);

    // Sweeps this computer's networks. A running search is restarted.
    void start(quint16 port = Kpa1500::kDefaultPort);
    // Sweeps exactly `targets` instead (tests, or a known address).
    void startWithTargets(const QList<QHostAddress>& targets,
                          quint16 port = Kpa1500::kDefaultPort);
    void cancel();
    bool isRunning() const { return m_running; }
    QList<Found> results() const;

    // Host addresses to probe for one local interface address: the whole
    // subnet when it is /22 or smaller (at most 1022 hosts), otherwise just
    // the /24 around `local`. Network, broadcast and `local` itself are
    // excluded. Empty for anything that is not a usable IPv4 address.
    static QList<QHostAddress> sweepTargets(const QHostAddress& local, int prefixLength);

    static constexpr int kSendBatch = 32;
    static constexpr int kSendIntervalMs = 15;
    static constexpr int kReplyGraceMs = 1500;

signals:
    void found(const AetherSDR::Kpa1500Discovery::Found& amp);
    void finished(int count);

private:
    void sendNextBatch();
    void onReadyRead();
    void finish();

    QUdpSocket m_socket;
    QTimer m_sendTimer;
    QTimer m_graceTimer;
    QList<QHostAddress> m_targets;
    int m_next{0};
    quint16 m_port{Kpa1500::kDefaultPort};
    bool m_running{false};
    QHash<quint32, Found> m_found;  // keyed by IPv4 address
};

}  // namespace AetherSDR
