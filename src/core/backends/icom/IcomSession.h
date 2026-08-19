#pragma once

#include <QHostAddress>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVariantMap>

#include <cstdint>
#include <span>
#include <vector>

#include "core/backends/icom/CivCodec.h"
#include "core/backends/icom/IcomAudio.h"
#include "core/backends/icom/IcomProtocol.h"
#include "core/backends/icom/IcomStream.h"

class QTimer;

namespace AetherSDR::icom {

// The RS-BA1 session: login, authentication, token renewal, and the three
// streams it brings up.
//
// This is the layer that knows the ORDER things must happen in, which is the
// part of the protocol with no documentation and the most ways to get subtly
// wrong. Everything below it (IcomStream, IcomProtocol) is mechanism.
//
// Deliberately knows nothing about AetherSDR's seam: it emits parsed CI-V
// frames and decoded audio, and IcomCivBackend turns those into model deltas.
// That split is what lets the whole session be tested against a fake radio on
// localhost without constructing a backend.
class IcomSession : public QObject {
    Q_OBJECT

public:
    struct Params {
        QHostAddress host;
        quint16 controlPort = kControlPort;
        quint16 serialPort  = kSerialPort;
        quint16 audioPort   = kAudioPort;
        QString username;
        QString password;
        quint32 sampleRateHz = 48000;
        AudioCodec codec = AudioCodec::Lpcm1ch16;
        // Whether to negotiate a transmit channel at all. False leaves the
        // radio with no TX codec, which is a stronger guarantee than simply
        // not sending audio — a receive-only session cannot key by accident.
        bool enableTx = true;
        quint16 txBufferMs = 200;
        // Production lease timing. Tests override these values to exercise the
        // renewal watchdog without waiting more than a minute.
        int tokenRenewalMs = 60000;
        int tokenAckGraceMs = 3000;
        int tokenDeadMs = 80000;
        // The one-time early renewal that covers a reconnect grant's shorter
        // first window. Overridable so a test can prove the early renewal
        // actually fires ahead of the steady cadence, rather than only that
        // the flag was set.
        int initialMaintenanceMs = 30000;
        // Zero selects a fresh random nonzero ID. Tests may override it to
        // prove reconnect correlation deterministically.
        quint16 tokenRequestId = 0;
        // The radio's CI-V address. Seeded from settings and CORRECTED from the
        // 0x19 0x00 reply once the session is up — never assumed, because the
        // address is user-changeable and other Icoms speak this same transport.
        std::uint8_t civAddress = 0xA4;
    };

    explicit IcomSession(QObject* parent = nullptr);
    ~IcomSession() override;

    Q_INVOKABLE bool start(const AetherSDR::icom::IcomSession::Params& params);
    Q_INVOKABLE void stop();

    [[nodiscard]] bool isConnected() const noexcept { return m_connected; }
    [[nodiscard]] QString deviceName() const { return m_deviceName; }
    [[nodiscard]] std::uint8_t civAddress() const noexcept { return m_params.civAddress; }

    // RETARGET the session at a different CI-V address, mid-session.
    //
    // The address the session opened with is a SEED — from the operator's pick,
    // or from the model the RS-BA1 handshake named — and the radio's own
    // 0x19 0x00 reply is what corrects it. Without a setter the correction had
    // nowhere to land: Params::civAddress is baked at start() and read through a
    // const getter, so an IC-9700 seeded at the IC-705's 0xA4 went on being
    // addressed at 0xA4 for the whole session and answered nothing.
    //
    // The echo filter in onSerialPayload() reads this same value, which is why
    // retargeting BEFORE the connect-edge read burst is the correct order: the
    // burst then goes to the new address and its echoes are recognised as ours.
    void setCivAddress(std::uint8_t address) noexcept { m_params.civAddress = address; }

    // Send one CI-V frame. Frames are built by CivCodec's cmd* helpers.
    void sendCiv(std::span<const std::uint8_t> frame);
    // Queue transmit audio (mono float). Nothing leaves until a full 20 ms
    // frame is available — the radio's jitter buffer reads a short packet as a
    // discontinuity.
    void sendAudio(std::span<const float> mono);
    // Discard queued transmit audio. Call on unkey.
    void flushTxAudio();

    struct Stats {
        IcomStream::Counters control;
        IcomStream::Counters serial;
        IcomStream::Counters audio;
    };
    [[nodiscard]] Stats stats() const;
    // Credential-free RS-BA1 lease state for health and automation diagnostics.
    [[nodiscard]] QVariantMap leaseDiagnostics() const;

signals:
    void connected(const QString& deviceName);
    void disconnected(const QString& reason);
    // One decoded CI-V frame from the radio. Echoes of our own commands are
    // already filtered out — see onSerialPayload().
    void civFrameReady(const AetherSDR::icom::CivFrame& frame);
    void audioReady(const std::vector<float>& mono);
    void audioLost(int packets);

private slots:
    void onControlReady();
    void onControlPayload(const QByteArray& packet);
    void onSerialReady();
    void onSerialPayload(const QByteArray& packet);
    void onAudioReady();
    void onAudioPayload(const QByteArray& packet);
    void onTokenRenew();
    void onTxPump();
    void onCivFrameTimeout();

private:
    void fail(const QString& reason);
    void sendRenewal(const QString& reason);
    [[nodiscard]] bool isCurrentControlPacket(std::span<const std::uint8_t> packet) const;
    void requestStreamsIfReady();
    void openMediaStreams();

    Params m_params;

    IcomStream* m_control = nullptr;
    IcomStream* m_serial  = nullptr;
    IcomStream* m_audio   = nullptr;

    QTimer* m_tokenTimer = nullptr;
    QTimer* m_txTimer = nullptr;
    QTimer* m_civTimeout = nullptr;
    // Re-sends the CI-V data-stream open until the radio actually starts
    // streaming. One open is not reliably enough — see onSerialReady().
    QTimer* m_civOpenRetry = nullptr;
    bool    m_civDataSeen = false;
    int     m_civOpenAttempts = 0;

    // Auth state. A grant may replace the auth ID, but only after its header
    // IDs prove it belongs to this control session.
    AuthId m_authId{};
    RadioId m_radioId{};
    QString m_radioName;
    QString m_deviceName;
    std::uint16_t m_innerSeq = 0;
    std::uint16_t m_tokenRequestId = 0;
    bool m_authOk = false;
    // Token-renewal watchdog. References renew at 60 s; a live IC-7300MK2
    // expired a reconnect grant around 45 s and an established lease about
    // 90 s after its last accepted token, both in silence.
    qint64 m_lastAuthOkMs = 0;   // when the radio last acknowledged an auth
    bool   m_renewUnacked = false;
    bool   m_initialMaintenancePending = false;
    int    m_renewRetries = 0;
    static constexpr int kTokenRenewMaxRetries = 4;
    QSet<quint16> m_pendingRenewals;
    quint16 m_lastRenewalSeq = 0;
    QString m_lastRenewalResult = QStringLiteral("not sent");
    quint32 m_lastRenewalResponse = 0;
    quint64 m_renewAcceptedCount = 0;
    quint64 m_tokenReissuedCount = 0;
    quint64 m_renewRejectedCount = 0;
    quint64 m_ignoredAuthReplies = 0;
    quint64 m_ignoredControlPackets = 0;
    bool m_haveRadioId = false;
    bool m_streamsRequested = false;
    bool m_streamGranted = false;
    bool m_connected = false;
    // Re-entrancy guard: a teardown makes several streams fail at once, and
    // each one calling stop() again would delete objects mid-signal.
    bool m_failing = false;

    std::uint16_t m_serialSendSeq = 0;
    std::uint16_t m_audioSendSeq = 1;

    CivReassembler m_civ;
    TxPacketizer m_tx;
    RxAssembler m_rx;
};

}  // namespace AetherSDR::icom

Q_DECLARE_METATYPE(AetherSDR::icom::IcomSession::Params)
Q_DECLARE_METATYPE(AetherSDR::icom::CivFrame)
