#pragma once

#include <QObject>
#include <QString>
#include <QVector>
#include <QMap>
#include <optional>

namespace MasterSDR {

// NAVTEX waveform status (per FlexLib v4.2.18)
enum class NavtexStatus {
    Inactive,
    Active,
    Transmitting,
    QueueFull,
    Unlicensed,
    Error
};

// Per-message status tracking
enum class NavtexMsgStatus {
    Error,
    Pending,    // Awaiting response from radio
    Queued,     // Radio confirmed receipt, queued for TX
    Sent        // Radio confirmed transmission
};

struct NavtexMsg {
    QString dateTime;
    uint idx{0};
    uint serialNum{0};
    QChar txIdent;
    QChar subjectIndicator;
    QString msgText;
    NavtexMsgStatus status{NavtexMsgStatus::Pending};
};

// Model for NAVTEX waveform protocol (FlexLib v4.2.18).
// Parses "navtex sent" and "navtex status" status messages,
// and formats "navtex send" commands.
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

    // Status parsing (from radio status dispatch)
    void parseStatus(const QString& object, const QMap<QString, QString>& kvs);

    // Called by RadioModel when a reply to "navtex send" arrives
    void handleSendResponse(int seq, uint respVal, const QString& indexStr);

signals:
    void commandReady(const QString& cmd);
    void replyCommandReady(const QString& cmd, int seq);
    void statusChanged(NavtexStatus status);
    void messagesChanged();

private:
    void parseSentStatus(const QMap<QString, QString>& kvs);
    static NavtexStatus parseStatusString(const QString& str);

    NavtexStatus m_status{NavtexStatus::Inactive};
    QVector<NavtexMsg> m_msgs;
    QMap<int, NavtexMsg> m_pendingMsgs;  // keyed by command sequence number
    int m_nextSeq{1};
};

} // namespace MasterSDR
