#pragma once

#include "core/AetherPDClient.h"

#include <QObject>
#include <QElapsedTimer>
#include <QTimer>
#include <optional>
#include <functional>

namespace AetherSDR {
namespace txlin { class AetherPDCaptureSource; class AetherPDTestClient; }

// One connection's volatile PD state. The owner must replace the session on
// reconnect, GUI identity change or token replacement, even at the same IP.
// This model has no radio keying or automatic calibration/recovery operation.
class AetherPDModel : public QObject {
    Q_OBJECT
public:
    explicit AetherPDModel(QObject* parent = nullptr);
    explicit AetherPDModel(QNetworkAccessManager& network, QObject* parent = nullptr);

    void setSession(const QHostAddress& address, const QString& adminToken,
                    std::function<bool()> current = {},
                    std::optional<aetherpd::StartIdentity> identity = std::nullopt);
    void clearSession();
    void reconcileSession();
    bool refresh();
    bool request(aetherpd::Operation operation, std::optional<quint32> toneSpacingHz = std::nullopt,
                 std::optional<aetherpd::IQSelection> selection = std::nullopt);
    // Bind a capture source and return the SAME original-session fence for the
    // measurement controller. Empty means refused. Credentials stay inside
    // the model/source; this never starts capture or grants transmit authority.
    std::function<bool()> bindCaptureSource(txlin::AetherPDCaptureSource& source);

    // Binds an unused helper client without starting it or granting TX authority.
    std::function<bool()> bindTestClient(txlin::AetherPDTestClient& client);

    bool active() const { return m_active && (!m_current || m_current()); }
    std::optional<aetherpd::Status> status() const;
    std::optional<aetherpd::Operation> pendingOperation() const { return m_pending; }
    std::optional<aetherpd::Acknowledgment> lastAcknowledgment() const { return m_acknowledgment; }
    std::optional<AetherPDClient::Failure> lastFailure() const { return m_failure; }

signals:
    void changed();
    void operationFailed(AetherSDR::aetherpd::Operation operation,
                         AetherSDR::AetherPDClient::Failure failure);

private:
    std::function<bool()> measurementFence();
    void initialize();
    void invalidateStatus();
    void resetSession();
    void onStatus(const aetherpd::Status& status);
    void onAcknowledgment(aetherpd::Operation operation, aetherpd::Acknowledgment acknowledgment);
    void onFailure(aetherpd::Operation operation, AetherPDClient::Failure failure);

    AetherPDClient m_client;
    QTimer m_poll;
    QTimer m_freshness;
    QElapsedTimer m_statusAge;
    QString m_token;
    QHostAddress m_address;
    std::optional<aetherpd::StartIdentity> m_identity;
    std::optional<aetherpd::IQSelection> m_expectedIq;
    std::optional<aetherpd::IQSelection> m_observedIq;
    bool m_bindingRejected{false};
    std::function<bool()> m_current;
    quint64 m_generation{0};
    quint64 m_captureGeneration{0};
    bool m_active{false};
    std::optional<aetherpd::Status> m_status;
    std::optional<aetherpd::Operation> m_pending;
    std::optional<aetherpd::Acknowledgment> m_acknowledgment;
    std::optional<AetherPDClient::Failure> m_failure;
};

} // namespace AetherSDR
