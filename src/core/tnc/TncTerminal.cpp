#include "core/tnc/TncTerminal.h"

#include "core/tnc/Ax25Connection.h"
#include "core/tnc/HeardList.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringList>

namespace AetherSDR {

using ax25::Address;
using ax25::Frame;

TncTerminal::TncTerminal(QObject* parent)
    : QObject(parent)
{
    m_link = new Ax25Connection(this);
    // Defaults sized for 1200-baud VHF FM with PTT overhead — same as the PMS.
    m_link->setRetryTimeoutMs(6000);
    m_link->setMaxRetries(8);
    m_link->setPaclen(128);

    connect(m_link, &Ax25Connection::sendFrame, this, &TncTerminal::transmitFrame);
    connect(m_link, &Ax25Connection::activity, this, &TncTerminal::onLinkActivity);
    connect(m_link, &Ax25Connection::connected, this, &TncTerminal::onLinkConnected);
    connect(m_link, &Ax25Connection::disconnected, this, &TncTerminal::onLinkDisconnected);
    connect(m_link, &Ax25Connection::connectFailed, this, &TncTerminal::onLinkConnectFailed);
    connect(m_link, &Ax25Connection::dataReceived, this, &TncTerminal::onLinkData);
}

TncTerminal::~TncTerminal()
{
    setLogging(false);
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

void TncTerminal::setMyCall(const QString& callWithSsid)
{
    auto parsed = Address::parse(callWithSsid.trimmed());
    m_myCall = parsed ? *parsed : Address{};
    m_link->setLocalAddress(m_myCall);
    emit stateChanged();
}

QString TncTerminal::myCall() const
{
    return m_myCall.isValid() ? m_myCall.toString() : QString();
}

bool TncTerminal::hasMyCall() const
{
    return m_myCall.isValid();
}

void TncTerminal::setEscapeChar(QChar c)
{
    if (!c.isNull())
        m_escape = c;
}

void TncTerminal::setRetryTimeoutMs(int t1) { m_link->setRetryTimeoutMs(t1); }
void TncTerminal::setMaxRetries(int n2) { m_link->setMaxRetries(n2); }
void TncTerminal::setPaclen(int bytes) { m_link->setPaclen(bytes); }

bool TncTerminal::isConnected() const
{
    return m_link->isConnected();
}

QString TncTerminal::peerCall() const
{
    const Address peer = m_link->remoteAddress();
    return peer.isValid() ? peer.toString() : QString();
}

QString TncTerminal::statusSummary() const
{
    if (m_link->isConnected())
        return QStringLiteral("Connected to %1").arg(peerCall());
    if (m_connecting)
        return QStringLiteral("Connecting to %1...").arg(peerCall());
    if (!m_myCall.isValid())
        return QStringLiteral("Set MYCALL to begin");
    return QStringLiteral("Disconnected — %1 ready").arg(myCall());
}

namespace {
QString humanBytes(quint64 n)
{
    if (n < 1024)
        return QStringLiteral("%1").arg(n);
    if (n < 1024 * 1024)
        return QStringLiteral("%1k").arg(n / 1024.0, 0, 'f', 1);
    return QStringLiteral("%1M").arg(n / (1024.0 * 1024.0), 0, 'f', 1);
}
} // namespace

QString TncTerminal::linkStats() const
{
    QString stats = QStringLiteral("TX %1  RX %2")
        .arg(humanBytes(m_txBytes), humanBytes(m_rxBytes));
    if (m_link->state() != Ax25Connection::State::Disconnected) {
        const auto& s = m_link->stats();
        stats = QStringLiteral("V(S)=%1 V(R)=%2  retry %3/%4  drop %5 resent %6  %7")
            .arg(m_link->sendSeq())
            .arg(m_link->recvSeq())
            .arg(m_link->retries())
            .arg(m_link->maxRetries())
            .arg(s.iDropped)
            .arg(s.iResent)
            .arg(stats);
    }
    return stats;
}

bool TncTerminal::isLogging() const
{
    return m_logFile != nullptr;
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

void TncTerminal::onAirFrame(const QByteArray& rawNoFcs)
{
    auto frame = Frame::decode(rawNoFcs);
    if (frame)
        m_link->onFrameReceived(*frame);
}

void TncTerminal::submitLine(const QString& line)
{
    if (m_mode == Mode::Converse) {
        // A lone escape character returns to the command prompt without dropping
        // the link, exactly like a hardware TNC's command character.
        if (line.size() == 1 && line.at(0) == m_escape) {
            enterCommandMode();
            return;
        }
        sendToPeer(line);
        return;
    }
    handleCommand(line);
}

void TncTerminal::enterCommandMode()
{
    if (m_mode == Mode::Command)
        return;
    setMode(Mode::Command);
    emitLine(QStringLiteral("*** Command mode (link still up — BYE to disconnect)"));
}

void TncTerminal::disconnectLink()
{
    if (m_link->state() == Ax25Connection::State::Disconnected) {
        emitLine(QStringLiteral("*** Not connected"));
        return;
    }
    emitLine(QStringLiteral("*** Disconnecting from %1...").arg(peerCall()));
    m_link->disconnect();
}

void TncTerminal::reset()
{
    m_connecting = false;
    m_failureReported = false;
    m_link->reset();
    setMode(Mode::Command);
}

// ---------------------------------------------------------------------------
// Command mode
// ---------------------------------------------------------------------------

void TncTerminal::handleCommand(const QString& rawLine)
{
    const QString line = rawLine.trimmed();
    if (line.isEmpty())
        return;

    const int sp = line.indexOf(QLatin1Char(' '));
    const QString verb = (sp < 0 ? line : line.left(sp)).toUpper();
    const QString args = (sp < 0 ? QString() : line.mid(sp + 1).trimmed());

    if (verb == QLatin1String("C") || verb == QLatin1String("CONNECT")) {
        if (m_link->state() != Ax25Connection::State::Disconnected) {
            emitLine(QStringLiteral("*** Already %1 — BYE first")
                .arg(m_link->isConnected() ? QStringLiteral("connected")
                                           : QStringLiteral("connecting")));
            return;
        }
        if (!m_myCall.isValid()) {
            emitLine(QStringLiteral("*** Set MYCALL first (MYCALL <yourcall>)"));
            return;
        }
        // Syntax: CONNECT <call> [VIA <digi1>[,<digi2>...]]   (or space-separated)
        QString destText = args;
        QVector<ax25::Address> via;
        const QStringList tokens = args.split(QRegularExpression(QStringLiteral("[\\s,]+")),
                                              Qt::SkipEmptyParts);
        if (!tokens.isEmpty()) {
            destText = tokens.first();
            int i = 1;
            // An explicit VIA/V keyword is optional; any extra calls are digis.
            if (i < tokens.size()
                && (tokens.at(i).compare(QLatin1String("VIA"), Qt::CaseInsensitive) == 0
                    || tokens.at(i).compare(QLatin1String("V"), Qt::CaseInsensitive) == 0)) {
                ++i;
            }
            for (; i < tokens.size() && via.size() < 8; ++i) {
                auto hop = Address::parse(tokens.at(i));
                if (!hop) {
                    emitLine(QStringLiteral("*** Invalid digipeater: %1").arg(tokens.at(i)));
                    return;
                }
                via.append(*hop);
            }
        }
        auto peer = Address::parse(destText);
        if (!peer) {
            emitLine(QStringLiteral("*** Usage: CONNECT <call> [VIA <digi>[,<digi>...]]"));
            return;
        }
        m_connecting = true;
        m_failureReported = false;
        QString viaText;
        if (!via.isEmpty()) {
            QStringList vs;
            for (const auto& h : via)
                vs << h.toString();
            viaText = QStringLiteral(" via %1").arg(vs.join(QLatin1Char(',')));
        }
        emitLine(QStringLiteral("*** Connecting to %1%2 as %3...")
            .arg(peer->toString(), viaText, myCall()));
        emit connectRequested(peer->toString()); // GUI ensures the modem RX tap is on
        emit stateChanged();
        m_link->connectTo(*peer, via);
        return;
    }

    if (verb == QLatin1String("D") || verb == QLatin1String("DISC")
        || verb == QLatin1String("DISCONNECT") || verb == QLatin1String("B")
        || verb == QLatin1String("BYE")) {
        disconnectLink();
        return;
    }

    if (verb == QLatin1String("CONV") || verb == QLatin1String("CONVERSE")
        || verb == QLatin1String("K")) {
        if (!m_link->isConnected()) {
            emitLine(QStringLiteral("*** Not connected — CONNECT first"));
            return;
        }
        setMode(Mode::Converse);
        emitLine(QStringLiteral("*** Converse mode — type to send; '%1' returns to command mode")
            .arg(m_escape));
        return;
    }

    if (verb == QLatin1String("S") || verb == QLatin1String("STATUS")) {
        cmdStatus();
        return;
    }

    if (verb == QLatin1String("MYCALL") || verb == QLatin1String("MY")) {
        if (args.isEmpty()) {
            emitLine(QStringLiteral("MYCALL %1")
                .arg(m_myCall.isValid() ? myCall() : QStringLiteral("(unset)")));
            return;
        }
        auto call = Address::parse(args);
        if (!call) {
            emitLine(QStringLiteral("*** Invalid callsign: %1").arg(args));
            return;
        }
        setMyCall(args);
        emitLine(QStringLiteral("*** MYCALL set to %1").arg(myCall()));
        return;
    }

    if (verb == QLatin1String("MHEARD") || verb == QLatin1String("MH")
        || verb == QLatin1String("JHEARD") || verb == QLatin1String("J")) {
        cmdMheard();
        return;
    }

    if (verb == QLatin1String("LOG")) {
        if (args.compare(QLatin1String("OFF"), Qt::CaseInsensitive) == 0)
            setLogging(false);
        else if (args.compare(QLatin1String("ON"), Qt::CaseInsensitive) == 0)
            setLogging(true);
        else
            setLogging(!isLogging()); // bare LOG toggles
        return;
    }

    if (verb == QLatin1String("ESCAPE") || verb == QLatin1String("ESC")) {
        if (args.size() == 1) {
            setEscapeChar(args.at(0));
            emitLine(QStringLiteral("*** Escape character set to '%1'").arg(m_escape));
        } else {
            emitLine(QStringLiteral("*** Escape character is '%1'").arg(m_escape));
        }
        return;
    }

    if (verb == QLatin1String("HELP") || verb == QLatin1String("H")
        || verb == QLatin1String("?")) {
        emitLine(QStringLiteral("Commands:"));
        emitLine(QStringLiteral("  CONNECT <call> [VIA <digi>,...]  (C)  connect to a station / BBS"));
        emitLine(QStringLiteral("  CONV             (K)  return to converse mode (when connected)"));
        emitLine(QStringLiteral("  STATUS           (S)  show connection stats"));
        emitLine(QStringLiteral("  BYE              (B,D,DISCONNECT)  hang up the link"));
        emitLine(QStringLiteral("  MHEARD           (MH)  stations heard on frequency"));
        emitLine(QStringLiteral("  MYCALL <call>    set/show your callsign"));
        emitLine(QStringLiteral("  LOG [ON|OFF]     toggle/set session transcript logging"));
        emitLine(QStringLiteral("  ESCAPE <char>    set/show the command-mode escape char"));
        emitLine(QStringLiteral("  HELP             (?)  this list"));
        emitLine(QStringLiteral("Once connected, type to send; '%1' alone returns here.")
            .arg(m_escape));
        return;
    }

    emitLine(QStringLiteral("*** ? (unknown command — type HELP)"));
}

// ---------------------------------------------------------------------------
// Converse mode
// ---------------------------------------------------------------------------

void TncTerminal::sendToPeer(const QString& line)
{
    if (!m_link->isConnected()) {
        emitLine(QStringLiteral("*** Not connected"));
        return;
    }
    // AX.25 BBSes are CR-terminated. Echo locally so the operator sees what they
    // sent (the link is half-duplex; the peer does not echo).
    QByteArray payload = line.toLatin1();
    payload.append('\r');
    m_txBytes += static_cast<quint64>(payload.size());
    m_link->sendData(payload);
    emitLine(line);
    emit stateChanged(); // refresh TX byte counter
}

// ---------------------------------------------------------------------------
// Link callbacks
// ---------------------------------------------------------------------------

void TncTerminal::onLinkConnected(const Address& peer)
{
    m_connecting = false;
    m_failureReported = false;
    m_txBytes = 0;
    m_rxBytes = 0;
    emitLine(QStringLiteral("*** CONNECTED to %1").arg(peer.toString()));
    setMode(Mode::Converse);
}

void TncTerminal::onLinkDisconnected(const Address& peer, bool byPeer)
{
    m_connecting = false;
    if (!m_failureReported) {
        emitLine(QStringLiteral("*** DISCONNECTED from %1%2")
            .arg(peer.toString(),
                 byPeer ? QStringLiteral(" (by peer)") : QString()));
    }
    m_failureReported = false;
    setMode(Mode::Command);
}

void TncTerminal::onLinkConnectFailed(const Address& peer, const QString& reason)
{
    m_connecting = false;
    m_failureReported = true; // suppress the redundant DISCONNECTED line that follows
    emitLine(QStringLiteral("*** CONNECT to %1 FAILED: %2").arg(peer.toString(), reason));
}

void TncTerminal::onLinkData(const QByteArray& data)
{
    m_rxBytes += static_cast<quint64>(data.size());
    // Normalise the peer's line endings (bare CR or CRLF) to '\n' for the pane.
    QString text = QString::fromLatin1(data);
    text.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    if (!text.isEmpty())
        emitOutput(text);
    emit stateChanged(); // refresh RX byte counter
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void TncTerminal::setMode(Mode mode)
{
    if (m_mode == mode) {
        emit stateChanged();
        return;
    }
    m_mode = mode;
    emit stateChanged();
}

void TncTerminal::onLinkActivity(const QString& msg)
{
    emit activity(msg); // always available to external consumers (e.g. a debug log)
    if (m_verbose)
        emitLine(QStringLiteral("\xc2\xb7 %1").arg(msg)); // '·' prefix, inline in transcript
    // Every protocol event (RX/TX frame, retransmit, drop) may move the counters,
    // so refresh the GUI's live status readout. Packet rates are low, so this is
    // cheap.
    emit stateChanged();
}

void TncTerminal::emitLine(const QString& text)
{
    // A *** session line (or a verbose debug line) must start on its own line so
    // it never overprints a BBS prompt that arrived without a trailing newline.
    if (!m_atLineStart)
        emitOutput(QStringLiteral("\n"));
    emitOutput(text + QLatin1Char('\n'));
}

void TncTerminal::emitOutput(const QString& text)
{
    if (text.isEmpty())
        return;
    if (m_logFile && m_logFile->isOpen()) {
        m_logFile->write(text.toUtf8());
        m_logFile->flush();
    }
    emit output(text);
    m_atLineStart = text.endsWith(QLatin1Char('\n'));
}

void TncTerminal::cmdStatus()
{
    const auto& s = m_link->stats();
    const QString state = m_link->isConnected()
        ? QStringLiteral("Connected (%1)")
              .arg(m_mode == Mode::Converse ? QStringLiteral("converse") : QStringLiteral("command"))
        : (m_connecting ? QStringLiteral("Connecting")
                        : QStringLiteral("Disconnected"));

    emitLine(QStringLiteral("--- STATUS -------------------------------"));
    emitLine(QStringLiteral("  MyCall  : %1").arg(m_myCall.isValid() ? myCall()
                                                                     : QStringLiteral("(unset)")));
    emitLine(QStringLiteral("  Peer    : %1").arg(peerCall().isEmpty() ? QStringLiteral("-")
                                                                       : peerCall()));
    emitLine(QStringLiteral("  State   : %1").arg(state));
    emitLine(QStringLiteral("  Seq     : V(S)=%1 V(R)=%2  unacked %3  send queue %4 B")
        .arg(m_link->sendSeq()).arg(m_link->recvSeq())
        .arg(m_link->unacked()).arg(m_link->sendQueueBytes()));
    emitLine(QStringLiteral("  Data    : TX %1 B   RX %2 B").arg(m_txBytes).arg(m_rxBytes));
    emitLine(QStringLiteral("  Packets : I sent %1  resent %2  rcvd %3  dropped %4")
        .arg(s.iSent).arg(s.iResent).arg(s.iRcvd).arg(s.iDropped));
    emitLine(QStringLiteral("  Acks    : RR rcvd %1   REJ sent %2  REJ rcvd %3  RNR %4")
        .arg(s.rrRcvd).arg(s.rejSent).arg(s.rejRcvd).arg(s.rnrRcvd));
    emitLine(QStringLiteral("  Retries : %1/%2   T1 timeouts %3   deferred-acks %4")
        .arg(m_link->retries()).arg(m_link->maxRetries()).arg(s.t1Timeouts).arg(s.t2Acks));
    emitLine(QStringLiteral("  Errors  : FRMR %1   bad-N(R) %2").arg(s.frmrRcvd).arg(s.invalidNr));
    emitLine(QStringLiteral("------------------------------------------"));
}

void TncTerminal::cmdMheard()
{
    if (!m_heard || m_heard->isEmpty()) {
        emitLine(QStringLiteral("*** Nothing heard yet."));
        return;
    }
    const auto stations = m_heard->stations(25);
    emitLine(QStringLiteral("CALLSIGN   LAST HEARD     LAST BEACON"));
    for (const auto& s : stations) {
        QString beacon = s.lastBeacon;
        if (beacon.size() > 40)
            beacon = beacon.left(39) + QChar(0x2026); // ellipsis
        emitLine(QStringLiteral("%1  %2  %3")
            .arg(s.station.toString().leftJustified(9),
                 s.utc.toString(QStringLiteral("MM/dd HH:mm")),
                 beacon));
    }
    emitLine(QStringLiteral("(%1 station(s) heard, UTC)").arg(m_heard->size()));
}

void TncTerminal::setLogging(bool on)
{
    if (on == isLogging())
        return;
    if (!on) {
        if (m_logFile) {
            emitLine(QStringLiteral("*** Session logging stopped (%1)")
                .arg(m_logFile->fileName()));
            m_logFile->close();
            delete m_logFile;
            m_logFile = nullptr;
        }
        return;
    }
    if (m_logDir.isEmpty()) {
        emitLine(QStringLiteral("*** Logging unavailable (no log directory configured)"));
        return;
    }
    QDir().mkpath(m_logDir);
    const QString stamp =
        QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    const QString path =
        QDir(m_logDir).filePath(QStringLiteral("terminal-%1.log").arg(stamp));
    auto* file = new QFile(path);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        emitLine(QStringLiteral("*** Could not open log file: %1").arg(path));
        delete file;
        return;
    }
    m_logFile = file;
    emitLine(QStringLiteral("*** Session logging to %1").arg(path));
}

} // namespace AetherSDR
