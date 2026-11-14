#pragma once

#include <QObject>
#include <QString>
#include <QVector>
#include <QMap>
#include <optional>
#include "core/backends/TransmitServiceRequest.h"
#include "core/backends/TransmitServiceStatus.h"

namespace AetherSDR {

// NAVTEX waveform status (per FlexLib v4.2.18).
using NavtexStatus = NavtexState;

// Per-message status tracking
enum class NavtexMsgStatus {
    Error,
    Pending,    // Awaiting response from radio
    Queued,     // Radio confirmed receipt, queued for TX
    Sent,       // Radio confirmed transmission
    Unknown     // Locally retired; execution or radio queue is not known
};

struct NavtexMsg {
    QString dateTime;
    uint idx{0};
    uint serialNum{0};
    QChar txIdent;
    QChar subjectIndicator;
    QString msgText;
    NavtexMsgStatus status{NavtexMsgStatus::Pending};
    quint64 session{0};
    quint64 requestId{0};
};

// Model for NAVTEX waveform protocol (FlexLib v4.2.18).
// Parses "navtex sent" and "navtex status" status messages,
// and submits bounded typed message intent to the backend.
class NavtexModel : public QObject {
    Q_OBJECT
public:
    explicit NavtexModel(QObject* parent = nullptr);

    // State
    NavtexStatus status() const { return m_status; }
    const QVector<NavtexMsg>& messages() const { return m_msgs; }

    // Actions
    void sendMessage(QChar txIdent, QChar subjectIndicator,
                     const QString& msgText, std::optional<uint> serialNum = std::nullopt);

    // One decoded NAVTEX report (the backend's status ingress).
    void applyObservation(const NavtexStatusObservation& observation);

    // Called by RadioModel when a reply to "navtex send" arrives
    void handleSendResponse(quint64 requestId, const TransmitServiceReply& reply);
    void reset();
    bool requestCurrent(quint64 requestId) const { return m_pendingMsgs.contains(requestId); }

signals:
    void controlRequested(const AetherSDR::NavtexRequest& request, quint64 requestId);
    void operationSettled(quint64 requestId);
    void statusChanged(NavtexStatus status);
    void messagesChanged();

private:
    void markSent(uint idx, uint serial);
    void appendMessage(const NavtexMsg& message);

    NavtexStatus m_status{NavtexStatus::Inactive};
    QVector<NavtexMsg> m_msgs;
    QMap<quint64, NavtexMsg> m_pendingMsgs;
    QMap<uint, quint64> m_queuedRequests;
    quint64 m_nextSeq{0};
    quint64 m_session{1};
};

} // namespace AetherSDR
