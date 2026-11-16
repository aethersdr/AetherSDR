#pragma once

#include "ITxCaptureSource.h"
#include "core/AetherPDProtocol.h"

#include <QElapsedTimer>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QTimer>

#include <array>

class QNetworkReply;

namespace AetherSDR::txlin {

// All methods and callbacks run on this QObject's thread. The session predicate
// must be a read-only test of the original connection/GUI/credential lifetime.
// No stream setup, calibration, keying, or automatic retry after failure.
class AetherPDCaptureSource final : public QObject, public ITxCaptureSource {
public:
    explicit AetherPDCaptureSource(QObject* parent = nullptr);
    explicit AetherPDCaptureSource(QNetworkAccessManager& network, QObject* parent = nullptr);
    ~AetherPDCaptureSource() override;

    void setSession(const QHostAddress& address, const QString& token, std::function<bool()> current);
    void clearSession();
    bool setMeasurementSource(quint64 sourceId,
                              std::optional<aetherpd::MeasurementToneConfig> tones = std::nullopt);
    bool start(const CaptureConfig& config) override;
    void stop() override;
    bool checkReceiveReady(ReadyCallback ready) override;
    void setGateOpen(bool open) override;
    bool hasReferenceChannel() const override { return false; }
    std::string describe() const override { return "On-radio AetherPD raw DAX IQ"; }
    void setFrameCallback(FrameCallback callback) override;
    void setErrorCallback(ErrorCallback callback) override;
    std::optional<aetherpd::CaptureSourceIdentity> receiveSourceIdentity() const;

private:
    void initialize();
    void abandon();
    void beginCapture();
    void send(const aetherpd::Request& request, int chunk = -1);
    void finish(QNetworkReply* reply, quint64 generation, int chunk);
    void fail(const std::string& reason);
    void captureTimeout();
    bool sessionCurrent() const;
    aetherpd::Request beginRequest() const;
    bool tonesMatch(const std::optional<aetherpd::MeasurementToneConfig>& observed) const;

    QNetworkAccessManager m_ownedNetwork;
    QPointer<QNetworkAccessManager> m_network;
    QPointer<QNetworkReply> m_reply;
    static constexpr int kChunkCount = aetherpd::kCaptureSamples / aetherpd::kCaptureChunkSamples;
    std::array<QPointer<QNetworkReply>, kChunkCount> m_chunkReplies;
    QTimer m_deadline;
    QElapsedTimer m_age;
    QHostAddress m_address;
    QString m_token;
    std::function<bool()> m_current;
    CaptureConfig m_config;
    FrameCallback m_frameCallback;
    ErrorCallback m_errorCallback;
    ReadyCallback m_readyCallback;
    std::optional<aetherpd::CaptureManifest> m_manifest;
    std::optional<aetherpd::CaptureSourceIdentity> m_verifiedReceiveSource;
    QByteArray m_bytes;
    quint64 m_generation{0};
    int m_chunk{0};
    int m_captureBudgetMs{0};
    bool m_running{false};
    bool m_gate{false};
    bool m_checkingReady{false};
    bool m_waitingForTones{false};
    bool m_legacyCapture{false};
    bool m_sourceIdentitySupported{false};
    quint64 m_measurementSourceId{0};
    std::optional<aetherpd::MeasurementToneConfig> m_measurementTones;
};
} // namespace AetherSDR::txlin
