#pragma once

// microHAM TRIO antenna-switch controller: one held session and the state it
// reports, for the Peripherals page; and this radio's antenna from the RX ANT
// and TX ANT menus (selectTxAntenna / selectRxAntenna), split implied: an RX
// pick other than the TX port turns split on, an RX pick equal to it turns it
// off. A radio-agnostic peripheral, never behind IRadioBackend. Nothing here
// selects a radio, an RX-only antenna (RX1..RX3) or RX EXT on the device.
// Wire and decisions: core/MicrohamTrioProtocol.h, core/MicrohamTrioSession.h.
// Main-thread only; no session and no timer while none is wanted.

#include "core/MicrohamTrioProtocol.h"
#include "core/MicrohamTrioSession.h"
#include "core/MicrohamTrioSettings.h"
#include "core/RadioSettingsScope.h"

#include <QObject>
#include <QString>

#include <memory>
#include <optional>

namespace AetherSDR {

class MicrohamTrioModel : public QObject {
    Q_OBJECT

public:
    // What a test replaces: bytes, monotonic time, randomness. None is owned;
    // all must outlive the model.
    struct Environment {
        MicrohamTrioTransport*       transport{nullptr};
        const MicrohamTrioClock*     clock{nullptr};
        MicrohamTrioSession::Entropy entropy;
    };

    enum class SessionWord {
        NotConfigured,
        Off,          // configured, no session wanted
        Connecting,
        Direct,       // held, alive, and this session's state received
        Offline,      // no answer, or a failed handshake; retried when reconnect is on
        Evicted,      // another client took the session; not taken back by itself
        Released,     // left to the Trio's own web page
    };

    struct View {
        bool    configured{false};
        QString host;
        int     port{MicrohamTrioSettings::kDefaultPort};
        SessionWord session{SessionWord::NotConfigured};
        // The last state received. `stateCurrent` is false once the session
        // it arrived on is gone: shown, marked as not current.
        std::optional<MicrohamTrio::Snapshot> snapshot;
        bool    stateCurrent{false};
        QString failure;   // why the last attempt failed, for the operator; empty when none

        // The connected radio and what the operator said about its place on
        // the Trio. `bindingIssue` is a sentence when the stored binding no
        // longer matches what the Trio reports; empty otherwise.
        bool    radioKnown{false};
        MicrohamTrioBinding::Stored binding{MicrohamTrioBinding::Stored::None};
        QString slotKey;
        QString slotLabel;      // as bound
        QString slotLabelNow;   // as the Trio reports it now; empty when not read
        QString bindingIssue;
    };

    struct WriteVerdict {
        bool    allowed{false};
        bool    sent{false};
        QString message;   // why not, for the operator; empty when allowed
    };

    enum class BindResult {
        Ok,
        NoTrio,
        NoRadioIdentity,
        NoState,          // no state to read the port's label from
        NoSuchPort,
        WriteFailed,
    };

    // Production: a TCP transport and a monotonic clock, made when first needed.
    explicit MicrohamTrioModel(QObject* parent = nullptr);
    explicit MicrohamTrioModel(const Environment& environment, QObject* parent = nullptr);
    ~MicrohamTrioModel() override;

    // Read the stored TRIO and, when one is configured and `connect` is true
    // (its "Connect automatically"), take its session.
    void loadSettings(bool connect);

    // Store the address and take the session. An empty host removes the TRIO.
    void connectNetwork(const QString& host, quint16 port);
    // Close the session and keep the address.
    void disconnect();
    // Close the session and forget the address.
    void removeTrio();
    // Close the session so the Trio's own page can have it. Connect takes it back.
    void releaseForBrowser();
    // Off: a failed session is closed instead of retried.
    void setAutoReconnect(bool on);

    // "Test connection": take the session once, read the state, release it,
    // and answer with testFinished(). Selects nothing; stores nothing. A held
    // session is not taken again: its current state is the answer. A session
    // that was wanted before the test is taken again after it.
    void testConnection(const QString& host, quint16 port);
    bool isTesting() const { return m_testing; }

    // Which radio this application has connected. A scope without a radio id
    // names no radio: no binding is read or written for it.
    void setRadio(const RadioSettingsScope& scope);
    // "This radio is on Trio radio port <key>": stores the key and the label it
    // carries now. Stored per radio; selects nothing on the Trio.
    BindResult bindRadioSlot(const QString& slotKey);
    // "This radio is not on the Trio".
    BindResult setNotOnTrio();
    // Forget what was said about this radio.
    BindResult clearBinding();

    // What a pick would meet, without sending. Allowed only with a held
    // session and a current state, this radio bound with no label error, the
    // Trio's focus on its port, no pick of ours still under way, the radio not
    // transmitting (setSwitchBlocked), and for TX a port not marked RX only.
    //   TX pick: without split moves TX and RX; in split moves the TX half, or
    //            ends split when it equals the RX half.
    //   RX pick: without split, a port other than TX turns split on with that
    //            RX half; in split moves the RX half, or ends split when it
    //            equals the TX half.
    // Each takes up to three commands, each sent after the one before is
    // confirmed; any refusal or disagreement on the way stops it and says so.
    // The first reason the Trio cannot take any pick for this radio now (not
    // configured, not bound, a label error, held by another client, not
    // connected, focus on another radio); empty when it can.
    QString unavailableReason() const;
    WriteVerdict canSelectTxAntenna(const QString& key) const;
    WriteVerdict canSelectRxAntenna(const QString& key) const;
    // Refused picks are dropped, never queued. Allowed with nothing to send
    // (already in place): allowed, not sent, no message.
    WriteVerdict selectTxAntenna(const QString& key);
    WriteVerdict selectRxAntenna(const QString& key);
    bool pickUnderWay() const { return m_goal.has_value(); }
    // Asked before every command: true while the radio may radiate.
    void setSwitchBlocked(std::function<bool()> blocked) { m_switchBlocked = std::move(blocked); }

    bool isConnected() const { return view().session == SessionWord::Direct; }
    bool isConnecting() const { return view().session == SessionWord::Connecting; }
    QString description() const;   // "192.168.8.10:80" while a session is wanted, else empty

    View view() const;

    bool hasSession() const { return m_session != nullptr; }
    // For tests, which move the clock and run the session's decision by hand.
    MicrohamTrioSession* session() const { return m_session; }

    // Words for the operator.
    static QString failureText(MicrohamTrioSession::Failure failure);
    static QString describeState(const MicrohamTrio::Snapshot& snapshot);
    // The binding in words, for the Peripherals page.
    static QString describeBinding(const View& view);
    // Device text for a log line: newlines as " | ", every other control
    // character (CR, ESC, ...) as U+FFFD, so a label cannot forge log lines.
    static QString forLog(const QString& text);

signals:
    // Anything view() reports may have changed.
    void changed();
    // The page's link signals: held with state, the session gone, an attempt failed.
    void connected();
    void disconnected();
    void connectionFailed(const QString& error);
    // The answer to testConnection(): reachable or not, and what was read.
    void testFinished(bool reachable, const QString& report);
    // A sentence the operator should see once: a pick the Trio did not take.
    void notice(const QString& message);

private slots:
    void onSnapshot(const AetherSDR::MicrohamTrio::Snapshot& snapshot,
                    AetherSDR::MicrohamTrioSession::SnapshotSource source);
    void onSessionState(AetherSDR::MicrohamTrioSession::Status status);
    void onWriteResult(AetherSDR::MicrohamTrio::AntennaField field, const QString& key,
                       AetherSDR::MicrohamTrioSession::WriteOutcome outcome);

private:
    void startSession();
    void dropSession();
    void setWord(SessionWord word);

    Environment m_environment;
    std::unique_ptr<MicrohamTrioTransport> m_ownTransport;
    std::unique_ptr<MicrohamTrioClock>     m_ownClock;
    bool m_production{false};

    MicrohamTrioSettings m_settings;
    RadioSettingsScope   m_scope;
    MicrohamTrioBinding  m_binding;
    bool m_autoReconnect{false};
    bool m_released{false};
    bool m_evicted{false};
    bool m_testing{false};
    bool m_resumeAfterTest{false};
    MicrohamTrioSettings m_testTarget;
    void finishTest(bool reachable, const QString& report);
    void abandonTest();
    QString m_failure;

    MicrohamTrioSession* m_session{nullptr};
    SessionWord m_word{SessionWord::NotConfigured};
    std::optional<MicrohamTrio::Snapshot> m_snapshot;

    // Where a pick ends; reached one confirmed command at a time.
    struct AntennaGoal {
        bool    split{false};
        QString tx;
        QString rx;
        int     writesLeft{3};
    };
    struct Step {
        MicrohamTrio::AntennaField field;
        QString key;
    };
    std::optional<AntennaGoal> m_goal;
    std::function<bool()> m_switchBlocked;
    WriteVerdict pickVerdict(bool tx, const QString& key) const;
    WriteVerdict startPick(bool tx, const QString& key);
    static std::optional<Step> nextStep(const AntennaGoal& goal, const MicrohamTrio::Snapshot& s);
    static AntennaGoal goalFor(bool tx, const QString& key, const MicrohamTrio::Snapshot& s);
    void continuePick();
    void stopPick(const QString& why);
    void stopPickSaying(const QString& sentence);
};

} // namespace AetherSDR
