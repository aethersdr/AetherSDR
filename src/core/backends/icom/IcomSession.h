#pragma once

#include <QHostAddress>
#include <QObject>
#include <QString>

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

    // Auth state. The auth id and session ids are re-read from the stream grant
    // rather than cached from the login — see parseStreamGrant's comment; the
    // failure of not doing so is "audio stops after exactly one minute".
    AuthId m_authId{};
    RadioId m_radioId{};
    QString m_radioName;
    QString m_deviceName;
    std::uint16_t m_innerSeq = 0;
    bool m_authOk = false;
    bool m_haveRadioId = false;
    bool m_streamsRequested = false;
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
