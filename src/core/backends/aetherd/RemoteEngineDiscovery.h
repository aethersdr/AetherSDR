#pragma once

#include <QHostAddress>
#include <QObject>
#include <QString>

#include <memory>
#include <optional>

class QUdpSocket;

namespace AetherSDR::aetherd {

// The desktop half of aetherd's LAN advertisement (docs/aetherd-remote-backend.md
// §1 and §3): broadcast "AETHERD-DISCOVER 1" to UDP 4996 and list each engine
// that answers. A reply only names an engine; nothing is sent to it until the
// operator pairs, and the pairing code's check digits pin its certificate.
class RemoteEngineDiscovery final : public QObject {
    Q_OBJECT

public:
    struct Advert {
        QString name;
        QString model;
        QString family;
        bool radioConnected{false};
        QString wssUrl;
        QString certificateSha256;
        QHostAddress address;
        quint16 port{0};
    };

    explicit RemoteEngineDiscovery(QObject* parent = nullptr);
    ~RemoteEngineDiscovery() override;

    // Opens the reply socket; false when it cannot bind.
    bool start();
    void stop();
    // One probe to every IPv4 broadcast address (and 255.255.255.255).
    void probe();
    // Tests only: send the probe to this one address and port instead.
    void setProbeTargetForTest(const QHostAddress& address, quint16 port);

    // A reply, if it is one: aetherd 1, a wss://<IP>:<port> URL whose address
    // is the sender's, and a 64-hex fingerprint. Bounded to 1 KiB.
    [[nodiscard]] static std::optional<Advert> parseReply(const QByteArray& datagram,
                                                          const QHostAddress& sender);

signals:
    void engineFound(const AetherSDR::aetherd::RemoteEngineDiscovery::Advert& advert);

private:
    void readReplies();

    std::unique_ptr<QUdpSocket> m_socket;
    std::optional<std::pair<QHostAddress, quint16>> m_testTarget;
};

} // namespace AetherSDR::aetherd
