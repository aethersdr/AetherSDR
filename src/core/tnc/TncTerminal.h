#pragma once

#include "core/tnc/Ax25.h"

#include <QByteArray>
#include <QObject>
#include <QString>

class QFile;

namespace AetherSDR {

class Ax25Connection;
class HeardList;

// A simple, reliable connected-mode AX.25 *client* terminal — the calling-side
// counterpart of the PmsMailbox (which is the answering side). It drives an
// Ax25Connection in the outbound role so an operator can CONNECT to a 1200-baud
// VHF packet BBS, converse with it, and disconnect, with the same retransmit /
// error-correction / T1 timer machinery the mailbox uses.
//
// Two input modes, exactly like a classic hardware TNC:
//   * Command mode  — typed lines are interpreted as terminal commands
//                     (CONNECT, BYE, MYCALL, HELP, ...). This is the prompt.
//   * Converse mode — entered once a link is up; typed lines are sent to the
//                     peer as I-frame data (CR-terminated, with local echo).
// An escape character (default '~' on a line by itself) drops from converse
// back to command mode WITHOUT disconnecting, so the operator can issue another
// command (e.g. BYE, or CONNECT to a different station).
//
// It is RF-agnostic and Qt::Network-free, exactly like PmsMailbox: feed it every
// decoded frame via onAirFrame() and key whatever it emits on transmitFrame().
// Deliberately no ANSI/VT100 handling — line in, line out, kept simple.
class TncTerminal : public QObject {
    Q_OBJECT

public:
    enum class Mode { Command, Converse };

    explicit TncTerminal(QObject* parent = nullptr);
    ~TncTerminal() override;

    // Our station callsign-SSID (the address outbound connects originate from).
    // Invalid/empty text leaves the terminal unable to connect until set.
    void setMyCall(const QString& callWithSsid);
    QString myCall() const;
    bool hasMyCall() const;

    // The single character that, alone on a line in converse mode, returns to the
    // command prompt. Default '~'. A null QChar is ignored.
    void setEscapeChar(QChar c);
    QChar escapeChar() const { return m_escape; }

    // Data-link tunables forwarded to the Ax25Connection (retries / timers / MTU).
    void setRetryTimeoutMs(int t1);
    void setMaxRetries(int n2);
    void setPaclen(int bytes);

    // The shared station-heard log (for the MHEARD command and quick-connect).
    // Non-owning; the GUI feeds it from the RX path. Safe to leave unset.
    void setHeardList(HeardList* heard) { m_heard = heard; }

    // Directory for session transcript logs. The LOG command writes a
    // timestamped file here. Empty (default) disables logging.
    void setLogDirectory(const QString& dir) { m_logDir = dir; }
    bool isLogging() const;
    // Start/stop transcript logging directly (for a GUI toggle, vs. the LOG
    // command). Safe to call in any mode — never routed to the peer.
    void setLogging(bool on);
    // Print the heard list to the transcript (for a GUI button), independent of
    // command/converse mode.
    void printMheard() { cmdMheard(); }

    // When on, low-level protocol activity (TX/RX frame detail) is echoed inline
    // into the transcript, prefixed with '·'. Off by default — the transcript
    // shows only the BBS data and the *** session lines.
    void setVerbose(bool on) { m_verbose = on; }
    bool isVerbose() const { return m_verbose; }

    Mode mode() const { return m_mode; }
    bool isConnected() const;
    bool isConnecting() const { return m_connecting; }
    // The peer we are connected to / dialing, or empty when idle.
    QString peerCall() const;
    // One-line human-readable status for the GUI (e.g. "Connected to KX9X-1").
    QString statusSummary() const;
    // Live data-link instrumentation: "V(S)=2 V(R)=3  retry 0/8  TX 1.2k RX 480".
    QString linkStats() const;
    quint64 txBytes() const { return m_txBytes; }
    quint64 rxBytes() const { return m_rxBytes; }

public slots:
    // Feed every decoded AX.25 frame (address..info, no FCS) here.
    void onAirFrame(const QByteArray& rawNoFcs);

    // A full line of operator input (the terminal submits on Enter). In command
    // mode it is parsed as a command; in converse mode it is sent to the peer.
    void submitLine(const QString& line);

    // Return to the command prompt without disconnecting (the escape action).
    void enterCommandMode();

    // Tell the terminal the transcript view was cleared by the GUI, so the next
    // *** line doesn't think it must insert a leading newline.
    void noteScreenCleared() { m_atLineStart = true; }

    // Gracefully disconnect the current link (DISC). No-op if idle.
    void disconnectLink();

    // Drop everything immediately (e.g. on shutdown / window close).
    void reset();

signals:
    // A raw AX.25 frame (address..info, no FCS) to key on the air.
    void transmitFrame(const QByteArray& rawNoFcs);

    // Text to append to the terminal transcript pane. Already newline-normalised
    // (peer CR / CRLF collapsed to '\n'); never carries a trailing prompt.
    void output(const QString& text);

    // Human-readable protocol activity for the shared AetherModem system log.
    void activity(const QString& message);

    // Mode and/or connection state changed — the GUI should refresh its labels.
    void stateChanged();

    // Emitted just before an outbound connect is dialed (from the CONNECT command
    // or the GUI Connect button). The GUI uses this to make sure the modem RX tap
    // is running before the link comes up, so the BBS's frames are actually heard.
    void connectRequested(const QString& peer);

private:
    void onLinkConnected(const ax25::Address& peer);
    void onLinkDisconnected(const ax25::Address& peer, bool byPeer);
    void onLinkConnectFailed(const ax25::Address& peer, const QString& reason);
    void onLinkData(const QByteArray& data);
    void onLinkActivity(const QString& msg);

    void handleCommand(const QString& line);
    void sendToPeer(const QString& line);
    void setMode(Mode mode);
    void emitLine(const QString& text);    // output() one CR/LF-free line + '\n'
    void emitOutput(const QString& text);  // emit + tee to the session log
    void cmdMheard();
    void cmdStatus();

    Ax25Connection* m_link{nullptr};
    HeardList* m_heard{nullptr}; // non-owning; shared station-heard log
    ax25::Address m_myCall;
    Mode m_mode{Mode::Command};
    QChar m_escape{QLatin1Char('~')};
    bool m_connecting{false};
    bool m_verbose{false};
    // Tracks whether the transcript currently sits at the start of a line, so a
    // *** session line (or debug line) always begins fresh and never overprints
    // a BBS prompt that didn't end in a newline.
    bool m_atLineStart{true};
    // Set when connectFailed() already explained why the link went down, so the
    // disconnected() that follows doesn't print a redundant "DISCONNECTED" line.
    bool m_failureReported{false};

    quint64 m_txBytes{0};
    quint64 m_rxBytes{0};

    QString m_logDir;
    QFile* m_logFile{nullptr};
};

} // namespace AetherSDR
