#pragma once

#include "core/aprs/AprsPacket.h"
#include "core/tnc/Ax25.h"

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVector>

namespace AetherSDR {

// APRS messaging (APRS 1.0.1 ch. 14): the store-and-retry engine behind the
// message UI. Incoming messages addressed to our callsign are stored, marked
// unread, auto-acked, and de-duplicated (a retrying sender repeats the same
// {msgNo} until our ack lands). Outgoing messages get a sequential message
// number and are retransmitted on a timer until the peer's ack arrives or
// the retry budget runs out. Frames go out through transmitFrame() — the
// owner wires that into the shared modem TX queue exactly like PmsMailbox.
class AprsMessenger : public QObject {
    Q_OBJECT

public:
    enum class State {
        Received, // incoming
        Pending,  // outgoing, waiting for first/next transmission slot
        Sent,     // outgoing, transmitted at least once, awaiting ack
        Acked,    // outgoing, peer confirmed
        Rejected, // outgoing, peer sent rej
        Failed,   // outgoing, retry budget exhausted
    };

    struct Message {
        QString counterpart;   // the remote station ("N0CALL-7")
        QString text;
        QDateTime utc;         // received / first-queued time, UTC
        bool outgoing{false};
        bool read{true};       // incoming messages start unread
        QString msgNo;         // APRS message number (may be empty inbound)
        State state{State::Received};
        int tries{0};
        QDateTime nextTryUtc;  // outgoing: next retransmission due
    };

    explicit AprsMessenger(QObject* parent = nullptr);
    ~AprsMessenger() override;

    void setMyAddress(const ax25::Address& addr);
    ax25::Address myAddress() const { return m_myAddress; }
    void setPath(const QVector<ax25::Address>& path) { m_path = path; }

    // Empty path (the default) keeps history in-memory only.
    void setPersistencePath(const QString& path);

    // Feed every parsed packet here; non-messages and messages addressed to
    // other stations are ignored (acks for our outbound traffic are matched).
    void onPacket(const aprs::Packet& packet);

    // Queue an outgoing message. Returns false when the destination callsign
    // is invalid or our own callsign is not configured.
    bool sendMessage(const QString& to, const QString& text);

    QVector<Message> messages() const { return m_messages; }
    int unreadCount() const;
    void markAllRead();
    void clear();

signals:
    void transmitFrame(const QByteArray& rawAx25NoFcs);
    void messageReceived(const Message& message);
    void messagesChanged();
    void unreadCountChanged(int count);
    void activity(const QString& line);

private:
    void transmitText(const QString& infoText);
    void serviceRetries();
    void load();
    void save() const;
    void scheduleSave();

    ax25::Address m_myAddress;
    QVector<ax25::Address> m_path;
    QVector<Message> m_messages;
    int m_nextMsgNo{1};
    QString m_persistPath;
    QTimer m_retryTimer;    // periodic scan for due retransmissions
    QTimer m_saveCoalesce;  // single-shot, HeardList-style coalesced save

    static constexpr int kRetryIntervalSecs = 30;
    static constexpr int kMaxTries = 4;
    static constexpr int kMaxStoredMessages = 500;
};

} // namespace AetherSDR
