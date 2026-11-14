#pragma once

#include <QByteArray>
#include <QHostAddress>
#include <QObject>

#include <cstdint>
#include <functional>
#include <optional>
#include <span>

class QUdpSocket;

namespace AetherSDR {

// One question before an automatic reconnect: is the radio already streaming
// to another client? HPSDR radios do not authenticate the controller. ANAN's
// p2app obeys whichever client sent last; the HL2 gateware latches its stream
// host but decodes commands from any. A retry that took the radio over would
// leave two controllers sending it different band, antenna and T/R relay
// settings, and the relays chatter (Zeus documents a brown-out reboot of a G2).
//
// One unicast discovery from a short-lived socket; the family's parser turns a
// reply into streaming yes/no. No reply within the timeout never blocks the
// reconnect (fail open, as Zeus does), so a slow radio cannot strand a retry.
class ReconnectBusyProbe : public QObject {
    Q_OBJECT
public:
    // nullopt = this datagram is not a discovery reply; otherwise "streaming".
    using Parse = std::function<std::optional<bool>(std::span<const std::uint8_t>)>;
    // nullopt = no reply in time.
    using Done = std::function<void(std::optional<bool> streaming)>;

    static constexpr int kTimeoutMs = 500;

    explicit ReconnectBusyProbe(QObject* parent = nullptr);

    // Ask `host`. A probe already in flight is cancelled first. `done` runs once,
    // on this object's thread, unless cancel() or a newer start() comes first.
    void start(const QHostAddress& host, quint16 port, const QByteArray& request,
               Parse parse, Done done);
    void cancel();
    [[nodiscard]] bool active() const noexcept { return static_cast<bool>(m_done); }

    // Only a reply saying the radio is streaming stops a reconnect.
    [[nodiscard]] static bool mayProceed(std::optional<bool> streaming) noexcept
    {
        return !(streaming && *streaming);
    }

    // Tests: arm a probe without a socket, then hand it the radio's answer.
    void armForTest(Done done);
    void answerForTest(std::optional<bool> streaming) { finish(m_generation, streaming); }

private:
    void finish(quint64 generation, std::optional<bool> streaming);

    QUdpSocket* m_socket = nullptr;
    Done m_done;
    quint64 m_generation = 0;
};

} // namespace AetherSDR
