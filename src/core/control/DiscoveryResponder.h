#pragma once

#include <QByteArray>
#include <QHostAddress>
#include <QJsonObject>
#include <QObject>
#include <QString>

#include <deque>
#include <functional>
#include <memory>

class QUdpSocket;

namespace AetherSDR::control {

// aetherd's LAN advertisement (docs/aetherd-remote-backend.md §1): while the
// TLS listener runs, answer an exact "AETHERD-DISCOVER 1" probe on UDP 4996
// with one unicast JSON reply naming the wss:// listener and its certificate
// fingerprint. It never sends anything unprompted, answers at most
// kMaxRepliesPerSecond in all, and ignores probes from anywhere but a private
// or link-local LAN source (never the tailnet). The reply carries no
// credential, no client list and no session state beyond whether a radio is
// held.
class DiscoveryResponder final : public QObject {
    Q_OBJECT

public:
    static constexpr quint16 kPort = 4996;
    static constexpr int kMaxRepliesPerSecond = 10;
    static constexpr qsizetype kMaxReplyBytes = 1024;
    static constexpr qsizetype kMaxFieldChars = 64;

    // What the reply says about the radio the engine holds, if any.
    struct Radio {
        QString model;
        QString family;
        bool connected{false};
    };

    struct Config {
        QString name;                // the host name
        QString wssUrl;              // "wss://<address>:<port>"
        QString certificateSha256;   // lowercase hex
        std::function<Radio()> radio;
        // Milliseconds from any monotonic origin; the rate bound reads it.
        std::function<qint64()> clock;
        QHostAddress bindAddress{QHostAddress::Any};
        quint16 port{kPort};
        // Tests only: loopback probes are otherwise ignored.
        bool acceptLoopback{false};
    };

    explicit DiscoveryResponder(Config config, QObject* parent = nullptr);
    ~DiscoveryResponder() override;

    // Binds the UDP port. False with *error set when it cannot; nothing else
    // in aetherd depends on it.
    [[nodiscard]] bool start(QString* error);
    void stop();
    [[nodiscard]] bool isListening() const;
    [[nodiscard]] quint16 boundPort() const;

    // Socket-free pieces, public so they can be tested without a network.
    [[nodiscard]] static bool isProbe(const QByteArray& datagram);
    // A private (RFC 1918, IPv6 ULA) or link-local source, and not the
    // tailnet: not 100.64/10 and not arriving on a tailscale*/wg* interface
    // (Tailscale's IPv6 range is itself ULA). An IPv4-mapped IPv6 source is
    // judged as IPv4.
    [[nodiscard]] static bool acceptsSource(const QHostAddress& source,
                                            const QString& arrivalInterface,
                                            bool acceptLoopback = false);
    [[nodiscard]] static bool isWireGuardInterfaceName(const QString& name);
    // The reply datagram, or empty when it cannot fit kMaxReplyBytes.
    [[nodiscard]] static QByteArray buildReply(const QString& name, const Radio& radio,
                                               const QString& wssUrl,
                                               const QString& certificateSha256);
    // Takes one reply slot at nowMs, or refuses when kMaxRepliesPerSecond
    // replies went out in the trailing second.
    [[nodiscard]] bool takeReplySlot(qint64 nowMs);

private:
    void readPending();

    Config m_config;
    std::unique_ptr<QUdpSocket> m_socket;
    std::deque<qint64> m_recentReplies;
};

} // namespace AetherSDR::control
