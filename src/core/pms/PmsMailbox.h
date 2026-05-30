#pragma once

#include "core/tnc/Ax25.h"

#include <QByteArray>
#include <QDateTime>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

class QTimer;

namespace AetherSDR {

class Ax25Connection;

// The Personal Mailbox System (PMS / PBBS) service: a compact, Kantronics-style
// AX.25 mailbox that a single remote caller can connect to at 1200 baud and
// read / list / send messages, see who has been heard, and disconnect. Messages,
// callers, and the heard list persist to JSON under the AetherSDR settings dir.
//
// This object owns an Ax25Connection (the connected-mode data link) and turns
// reassembled line input into mailbox command responses. It is RF-agnostic: feed
// it every decoded frame via onAirFrame() and key whatever it emits on
// transmitFrame(). The heard list and UI-beacon logic are deliberately split out
// so the future APRS/AX.25 digipeater can reuse the same plumbing.
class PmsMailbox : public QObject {
    Q_OBJECT

public:
    struct Message {
        int id{0};
        QChar type{QLatin1Char('P')}; // 'P' private, 'B' bulletin
        QString to;
        QString from;
        QString subject;
        QString body;
        QDateTime utc;
        bool read{false};
    };

    struct Caller {
        QString call;
        QDateTime utc;
    };

    struct Heard {
        QString call;
        QString dest;
        QString via;
        QDateTime utc;
        int count{1};
    };

    explicit PmsMailbox(QObject* parent = nullptr);
    ~PmsMailbox() override;

    // ---- Configuration (the GUI persists these via AppSettings) -------------
    void setEnabled(bool on);
    bool isEnabled() const { return m_enabled; }
    // Full listen callsign-SSID the mailbox answers on, e.g. "KI6BCJ-10".
    // Invalid/empty text leaves the mailbox without a primary address.
    void setListenCallsign(const QString& callWithSsid);
    QString listenCallsign() const { return m_listen.isValid() ? m_listen.toString() : QString(); }
    // Optional vanity/alias callsign-SSID also answered, e.g. "AETHBBS".
    // Empty text clears it.
    void setAliasCallsign(const QString& callWithSsid);
    QString aliasCallsign() const { return m_alias.isValid() ? m_alias.toString() : QString(); }
    bool hasValidAddress() const { return m_listen.isValid(); }
    // The configured primary address (or, mid-session, the one the caller dialed).
    ax25::Address localAddress() const;
    void setVersionString(const QString& version) { m_version = version; }

    void setWelcomeText(const QString& text) { m_welcome = text; }
    QString welcomeText() const { return m_welcome; }

    void setBeaconEnabled(bool on);
    bool beaconEnabled() const { return m_beaconEnabled; }
    void setBeaconText(const QString& text) { m_beaconText = text; }
    QString beaconText() const { return m_beaconText; }
    void setBeaconIntervalMinutes(int minutes);
    int beaconIntervalMinutes() const { return m_beaconIntervalMin; }
    void setBeaconDestination(const QString& dest) { m_beaconDest = dest; }

    // Data-link tunables forwarded to the Ax25Connection.
    void setRetryTimeoutMs(int t1);
    void setMaxRetries(int n2);
    void setPaclen(int bytes);

    // ---- Stats for the GUI -------------------------------------------------
    int messageCount() const { return m_messages.size(); }
    int callerCount() const { return m_callers.size(); }
    QStringList lastCallers(int n = 5) const;
    QStringList heardSummary(int n = 20) const;
    qint64 freeDiskBytes() const;
    QString storageDir() const;
    bool isCallerConnected() const;
    QString connectedCaller() const;

public slots:
    // Feed every decoded AX.25 frame (address..info, no FCS) here. The heard list
    // is updated for all frames; frames addressed to our PMS are handled by the
    // data link. Safe to call when disabled (only heard tracking happens).
    void onAirFrame(const QByteArray& rawNoFcs);

    // Force-disconnect the current caller (graceful DISC).
    void disconnectCaller();

    // Send a beacon immediately (also called by the hourly timer).
    void sendBeaconNow();

signals:
    // A raw AX.25 frame (address..info, no FCS) to key on the air.
    void transmitFrame(const QByteArray& rawNoFcs);
    // Human-readable activity for the AetherModem log.
    void activity(const QString& message);
    // Connection/state/stats changed — the GUI should refresh its Mailbox panel.
    void stateChanged();

private:
    void onLinkConnected(const ax25::Address& peer);
    void onLinkDisconnected(const ax25::Address& peer, bool byPeer);
    void onLinkData(const QByteArray& data);

    void recordHeard(const ax25::Frame& frame);
    void recordCaller(const ax25::Address& peer);

    void reply(const QString& text); // queue CR-terminated line(s) to the caller
    void flushReplies();             // hand queued output to the data link at once
    void sendGreeting(const ax25::Address& peer);
    void sendPrompt();
    void processLine(const QString& line);
    void handleCommand(const QString& line);

    // Command handlers.
    void cmdHelp();
    void cmdList(const QString& args, bool mineOnly);
    void cmdRead(const QString& args);
    void cmdKill(const QString& args);
    void cmdSendBegin(const QString& args, QChar type);
    void cmdJheard(const QString& args);
    void cmdUsers();
    void cmdInfo();
    void finishCompose(bool save);

    bool callerMayAccess(const Message& msg) const;

    // Persistence.
    QString messagesPath() const;
    QString callersPath() const;
    QString heardPath() const;
    void ensureStorageDir() const;
    void loadAll();
    void saveMessages() const;
    void saveCallers() const;
    void saveHeard() const;

    Ax25Connection* m_link{nullptr};
    QTimer* m_beaconTimer{nullptr};

    bool m_enabled{false};
    ax25::Address m_listen; // primary listen address (invalid until configured)
    ax25::Address m_alias;  // optional vanity/alias address (invalid = none)
    QString m_version{QStringLiteral("0.0")};
    QString m_welcome;
    bool m_beaconEnabled{false};
    QString m_beaconText{QStringLiteral("AetherMailbox online - connect for messages")};
    QString m_beaconDest{QStringLiteral("BEACON")};
    int m_beaconIntervalMin{60};

    QVector<Message> m_messages;
    int m_nextId{1};
    QVector<Caller> m_callers;
    QVector<Heard> m_heard;

    // Per-connection session state (single caller).
    bool m_connected{false};
    ax25::Address m_caller;
    QString m_lineBuffer;  // inbound, awaiting a CR
    QString m_pendingOut;  // outbound, coalesced until flushReplies()

    // Multi-line compose state.
    enum class Compose { None, Subject, Body };
    Compose m_compose{Compose::None};
    Message m_draft;
    QStringList m_draftLines;

    bool m_loaded{false};
};

} // namespace AetherSDR
