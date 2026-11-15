#pragma once

#include "ITxCaptureSource.h"
#include "core/AetherPDProtocol.h"

#include <QElapsedTimer>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QTimer>

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
    bool setMeasurementSource(quint64 sourceId);
    bool start(const CaptureConfig& config) override;
    void stop() override;
    bool checkReceiveReady(ReadyCallback ready) override;
    void setGateOpen(bool open) override;
    bool hasReferenceChannel() const override { return false; }
    std::string describe() const override { return "On-radio AetherPD raw DAX IQ"; }
    void setFrameCallback(FrameCallback callback) override;
    void setErrorCallback(ErrorCallback callback) override;

private:
    void initialize();
    void abandon();
    void beginCapture();
    void send(const aetherpd::Request& request);
    void finish(QNetworkReply* reply, quint64 generation);
    void fail(const std::string& reason);
    bool sessionCurrent() const;

    QNetworkAccessManager m_ownedNetwork;
    QPointer<QNetworkAccessManager> m_network;
    QPointer<QNetworkReply> m_reply;
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
    QByteArray m_bytes;
    quint64 m_generation{0};
    int m_chunk{0};
    bool m_running{false};
    bool m_gate{false};
    bool m_checkingReady{false};
    bool m_waitingForTones{false};
    quint64 m_measurementSourceId{0};
};
} // namespace AetherSDR::txlin
