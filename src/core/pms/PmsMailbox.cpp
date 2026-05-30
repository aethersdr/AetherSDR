#include "core/pms/PmsMailbox.h"

#include "core/AppSettings.h"
#include "core/tnc/Ax25Connection.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStorageInfo>
#include <QTimer>

#include <algorithm>

namespace AetherSDR {

using ax25::Address;
using ax25::Frame;
using ax25::FrameType;

namespace {

QString lineEnding() { return QStringLiteral("\r"); }

bool sameCall(const QString& a, const QString& b)
{
    return a.compare(b, Qt::CaseInsensitive) == 0;
}

} // namespace

PmsMailbox::PmsMailbox(QObject* parent)
    : QObject(parent)
{
    m_link = new Ax25Connection(this);
    m_link->setRetryTimeoutMs(6000);
    m_link->setMaxRetries(8);
    m_link->setPaclen(128);

    connect(m_link, &Ax25Connection::sendFrame, this, &PmsMailbox::transmitFrame);
    connect(m_link, &Ax25Connection::activity, this, &PmsMailbox::activity);
    connect(m_link, &Ax25Connection::connected, this, &PmsMailbox::onLinkConnected);
    connect(m_link, &Ax25Connection::disconnected, this, &PmsMailbox::onLinkDisconnected);
    connect(m_link, &Ax25Connection::dataReceived, this, &PmsMailbox::onLinkData);

    m_beaconTimer = new QTimer(this);
    connect(m_beaconTimer, &QTimer::timeout, this, &PmsMailbox::sendBeaconNow);
}

PmsMailbox::~PmsMailbox()
{
    if (m_loaded)
        saveHeard();
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

Address PmsMailbox::localAddress() const
{
    // Mid-session, surface the address the caller actually dialed; otherwise the
    // configured primary listen address.
    if (m_connected)
        return m_link->localAddress();
    return m_listen;
}

void PmsMailbox::setEnabled(bool on)
{
    if (m_enabled == on)
        return;
    m_enabled = on;
    if (on) {
        if (!m_loaded)
            loadAll();
        m_link->setLocalAddress(m_listen);
        m_link->setAliasAddress(m_alias);
        if (m_beaconEnabled)
            setBeaconIntervalMinutes(m_beaconIntervalMin); // (re)starts the timer
        emit activity(QStringLiteral("PMS enabled as %1%2.")
            .arg(m_listen.toString(),
                 m_alias.isValid() ? QStringLiteral(" (alias %1)").arg(m_alias.toString())
                                   : QString()));
    } else {
        m_link->reset();
        m_beaconTimer->stop();
        saveHeard();
        emit activity(QStringLiteral("PMS disabled."));
    }
    emit stateChanged();
}

void PmsMailbox::setListenCallsign(const QString& callWithSsid)
{
    const auto parsed = ax25::Address::parse(callWithSsid);
    const ax25::Address addr = parsed.value_or(ax25::Address{});
    if (addr == m_listen && addr.isValid() == m_listen.isValid())
        return;
    m_listen = addr;
    m_link->setLocalAddress(m_listen);
    emit stateChanged();
}

void PmsMailbox::setAliasCallsign(const QString& callWithSsid)
{
    const QString trimmed = callWithSsid.trimmed();
    const ax25::Address addr = trimmed.isEmpty()
        ? ax25::Address{}
        : ax25::Address::parse(trimmed).value_or(ax25::Address{});
    if (addr == m_alias && addr.isValid() == m_alias.isValid())
        return;
    m_alias = addr;
    m_link->setAliasAddress(m_alias);
    emit stateChanged();
}

void PmsMailbox::setBeaconEnabled(bool on)
{
    m_beaconEnabled = on;
    if (m_enabled && on)
        setBeaconIntervalMinutes(m_beaconIntervalMin);
    else
        m_beaconTimer->stop();
}

void PmsMailbox::setBeaconIntervalMinutes(int minutes)
{
    m_beaconIntervalMin = qBound(1, minutes, 24 * 60);
    if (m_enabled && m_beaconEnabled)
        m_beaconTimer->start(m_beaconIntervalMin * 60 * 1000);
}

void PmsMailbox::setRetryTimeoutMs(int t1) { m_link->setRetryTimeoutMs(t1); }
void PmsMailbox::setMaxRetries(int n2) { m_link->setMaxRetries(n2); }
void PmsMailbox::setPaclen(int bytes) { m_link->setPaclen(bytes); }

// ---------------------------------------------------------------------------
// Stats
// ---------------------------------------------------------------------------

QStringList PmsMailbox::lastCallers(int n) const
{
    QStringList out;
    for (int i = m_callers.size() - 1; i >= 0 && out.size() < n; --i) {
        out << QStringLiteral("%1  %2")
                   .arg(m_callers.at(i).call,
                        m_callers.at(i).utc.toString(QStringLiteral("yyyy-MM-dd HH:mm")));
    }
    return out;
}

QStringList PmsMailbox::heardSummary(int n) const
{
    QVector<Heard> sorted = m_heard;
    std::sort(sorted.begin(), sorted.end(), [](const Heard& a, const Heard& b) {
        return a.utc > b.utc;
    });
    QStringList out;
    for (int i = 0; i < sorted.size() && i < n; ++i) {
        out << QStringLiteral("%1  %2")
                   .arg(sorted.at(i).call.leftJustified(9),
                        sorted.at(i).utc.toString(QStringLiteral("yyyy-MM-dd HH:mm")));
    }
    return out;
}

qint64 PmsMailbox::freeDiskBytes() const
{
    return QStorageInfo(storageDir()).bytesAvailable();
}

QString PmsMailbox::storageDir() const
{
    // AETHER_PMS_DIR overrides the location (used by tests for isolation; also a
    // handy ops hook for pointing the mailbox store elsewhere).
    const QByteArray override = qgetenv("AETHER_PMS_DIR");
    if (!override.isEmpty())
        return QString::fromUtf8(override);
    const QString base = QFileInfo(AppSettings::instance().filePath()).absolutePath();
    return base + QStringLiteral("/pms");
}

bool PmsMailbox::isCallerConnected() const { return m_connected; }
QString PmsMailbox::connectedCaller() const
{
    return m_connected ? m_caller.toString() : QString();
}

// ---------------------------------------------------------------------------
// Frame intake
// ---------------------------------------------------------------------------

void PmsMailbox::onAirFrame(const QByteArray& rawNoFcs)
{
    auto frame = Frame::decode(rawNoFcs);
    if (!frame)
        return;

    // Don't log our own transmissions (primary or alias) in the heard list.
    const bool isUs = (m_listen.isValid() && frame->src == m_listen)
        || (m_alias.isValid() && frame->src == m_alias);
    if (!isUs)
        recordHeard(*frame);

    // Diagnostic: every decoded frame, with the address-match decision spelled
    // out. This is the key instrument for connect troubleshooting — it tells us
    // whether a SABM was decoded at all and whether its dest matched the listen
    // or alias address (decode problem vs address mismatch vs not-for-us).
    if (m_enabled) {
        const bool destMatch = (m_listen.isValid() && frame->dest == m_listen)
            || (m_alias.isValid() && frame->dest == m_alias);
        QString which = QStringLiteral("none");
        if (m_listen.isValid() && frame->dest == m_listen)
            which = QStringLiteral("listen");
        else if (m_alias.isValid() && frame->dest == m_alias)
            which = QStringLiteral("alias");
        emit activity(QStringLiteral(
            "PMS RX %1 %2>%3 — dest match=%4 (%5); listen=%6 alias=%7")
            .arg(ax25::frameTypeName(frame->type),
                 frame->src.toString(),
                 frame->dest.toString(),
                 destMatch ? QStringLiteral("yes") : QStringLiteral("no"),
                 which,
                 m_listen.isValid() ? m_listen.toString() : QStringLiteral("(unset)"),
                 m_alias.isValid() ? m_alias.toString() : QStringLiteral("(none)")));
    }

    // Let the data link decide if the frame is addressed to us; it matches both
    // the primary listen address and the optional vanity alias.
    if (m_enabled)
        m_link->onFrameReceived(*frame);
}

void PmsMailbox::disconnectCaller()
{
    if (m_connected)
        m_link->disconnect();
}

void PmsMailbox::recordHeard(const Frame& frame)
{
    if (!m_loaded)
        loadAll();
    const QString call = frame.src.toString();
    if (call.isEmpty())
        return;

    const QString via = frame.via.isEmpty()
        ? QString()
        : [&] {
              QStringList v;
              for (const Address& a : frame.via)
                  v << a.toString();
              return v.join(QLatin1Char(','));
          }();

    bool isNew = true;
    for (Heard& h : m_heard) {
        if (sameCall(h.call, call)) {
            h.utc = QDateTime::currentDateTimeUtc();
            h.dest = frame.dest.toString();
            h.via = via;
            ++h.count;
            isNew = false;
            break;
        }
    }
    if (isNew) {
        Heard h;
        h.call = call;
        h.dest = frame.dest.toString();
        h.via = via;
        h.utc = QDateTime::currentDateTimeUtc();
        m_heard.append(h);
        // Cap the heard list to a sane size (drop the oldest).
        if (m_heard.size() > 200) {
            std::sort(m_heard.begin(), m_heard.end(),
                      [](const Heard& a, const Heard& b) { return a.utc > b.utc; });
            m_heard.resize(200);
        }
        saveHeard();
        emit stateChanged();
    }
}

void PmsMailbox::recordCaller(const Address& peer)
{
    Caller c;
    c.call = peer.toString();
    c.utc = QDateTime::currentDateTimeUtc();
    m_callers.append(c);
    if (m_callers.size() > 500)
        m_callers.remove(0, m_callers.size() - 500);
    saveCallers();
}

// ---------------------------------------------------------------------------
// Connection lifecycle
// ---------------------------------------------------------------------------

void PmsMailbox::onLinkConnected(const Address& peer)
{
    m_connected = true;
    m_caller = peer;
    m_lineBuffer.clear();
    m_compose = Compose::None;
    m_draftLines.clear();
    recordCaller(peer);
    m_pendingOut.clear();
    sendGreeting(peer);
    flushReplies();
    emit stateChanged();
}

void PmsMailbox::onLinkDisconnected(const Address& peer, bool byPeer)
{
    Q_UNUSED(peer);
    Q_UNUSED(byPeer);
    m_connected = false;
    m_caller = Address{};
    m_lineBuffer.clear();
    m_pendingOut.clear();
    m_compose = Compose::None;
    m_draftLines.clear();
    saveHeard();
    emit stateChanged();
}

void PmsMailbox::onLinkData(const QByteArray& data)
{
    m_lineBuffer += QString::fromLatin1(data);
    // Treat CR and LF as line separators; collapse a CRLF pair.
    m_lineBuffer.replace(QLatin1Char('\n'), QLatin1Char('\r'));
    int idx;
    while ((idx = m_lineBuffer.indexOf(QLatin1Char('\r'))) >= 0) {
        const QString line = m_lineBuffer.left(idx);
        m_lineBuffer.remove(0, idx + 1);
        processLine(line);
    }
    // Ctrl-Z (end of message) may arrive without a trailing CR.
    if (m_compose == Compose::Body && m_lineBuffer.contains(QChar(0x1a))) {
        m_lineBuffer.remove(QChar(0x1a));
        finishCompose(true);
    }
    flushReplies();
}

// ---------------------------------------------------------------------------
// Output helpers
// ---------------------------------------------------------------------------

void PmsMailbox::reply(const QString& text)
{
    // Queue the line(s); a whole command's output is handed to the data link in
    // one go by flushReplies() so it coalesces into as few I-frames (and thus
    // PTT keyings) as possible. Embedded newlines become separate CR lines.
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString& line : lines)
        m_pendingOut += line + lineEnding();
}

void PmsMailbox::flushReplies()
{
    if (m_pendingOut.isEmpty())
        return;
    if (m_connected)
        m_link->sendData(m_pendingOut.toLatin1());
    m_pendingOut.clear();
}

void PmsMailbox::sendGreeting(const Address& peer)
{
    reply(QStringLiteral("[AetherMailbox-%1]").arg(m_version));
    if (!m_welcome.trimmed().isEmpty())
        reply(m_welcome.trimmed());
    reply(QStringLiteral("%1 message(s) on file.").arg(m_messages.size()));
    reply(QStringLiteral("Hello %1, welcome to the %2 AetherMailbox.")
              .arg(peer.call, localAddress().toString()));
    sendPrompt();
}

void PmsMailbox::sendPrompt()
{
    reply(QStringLiteral("ENTER COMMAND: B,H,I,J,K,L,R,S,U >"));
}

// ---------------------------------------------------------------------------
// Command dispatch
// ---------------------------------------------------------------------------

void PmsMailbox::processLine(const QString& line)
{
    if (m_compose != Compose::None) {
        if (m_compose == Compose::Subject) {
            m_draft.subject = line.trimmed();
            m_compose = Compose::Body;
            reply(QStringLiteral(
                "ENTER MESSAGE %1 - END WITH /EX OR CTRL-Z ON A LINE BY ITSELF:")
                      .arg(m_nextId));
            return;
        }
        // Body
        const QString trimmed = line.trimmed();
        if (trimmed.compare(QStringLiteral("/EX"), Qt::CaseInsensitive) == 0
            || line.contains(QChar(0x1a))) {
            finishCompose(true);
            return;
        }
        m_draftLines.append(line);
        return;
    }
    handleCommand(line);
}

void PmsMailbox::handleCommand(const QString& line)
{
    const QString trimmed = line.trimmed();
    if (trimmed.isEmpty()) {
        sendPrompt();
        return;
    }

    const int sp = trimmed.indexOf(QLatin1Char(' '));
    const QString cmd = (sp < 0 ? trimmed : trimmed.left(sp)).toUpper();
    const QString args = (sp < 0 ? QString() : trimmed.mid(sp + 1).trimmed());

    bool promptAfter = true;
    if (cmd == QLatin1String("B") || cmd == QLatin1String("BYE")) {
        reply(QStringLiteral("73! Disconnecting."));
        flushReplies(); // get the sign-off out before we leave Connected state
        m_link->disconnect();
        promptAfter = false;
    } else if (cmd == QLatin1String("H") || cmd == QLatin1String("?")
               || cmd == QLatin1String("HELP")) {
        cmdHelp();
    } else if (cmd == QLatin1String("I") || cmd == QLatin1String("INFO")) {
        cmdInfo();
    } else if (cmd == QLatin1String("J") || cmd == QLatin1String("JHEARD")) {
        cmdJheard(args);
    } else if (cmd == QLatin1String("K") || cmd == QLatin1String("KILL")) {
        cmdKill(args);
    } else if (cmd == QLatin1String("LM")) {
        cmdList(args, /*mineOnly=*/true);
    } else if (cmd == QLatin1String("L") || cmd == QLatin1String("LIST")) {
        cmdList(args, /*mineOnly=*/false);
    } else if (cmd == QLatin1String("R") || cmd == QLatin1String("READ")) {
        cmdRead(args);
    } else if (cmd == QLatin1String("SB")) {
        cmdSendBegin(args, QLatin1Char('B'));
        promptAfter = false; // compose drives its own prompts
    } else if (cmd == QLatin1String("S") || cmd == QLatin1String("SP")
               || cmd == QLatin1String("SEND")) {
        cmdSendBegin(args, QLatin1Char('P'));
        promptAfter = false;
    } else if (cmd == QLatin1String("U") || cmd == QLatin1String("USERS")) {
        cmdUsers();
    } else {
        reply(QStringLiteral("? Unknown command '%1'. Type H for help.").arg(cmd));
    }

    if (promptAfter && m_connected && m_compose == Compose::None)
        sendPrompt();
}

void PmsMailbox::cmdHelp()
{
    reply(QStringLiteral(
        "AetherMailbox commands:\n"
        " B(ye)        Disconnect from the mailbox\n"
        " H(elp)       This help\n"
        " I(nfo)       Mailbox info / welcome\n"
        " J(heard)     Stations heard recently (find other PMS/BBS)\n"
        " K(ill) n     Delete message number n\n"
        " L(ist)       List all messages\n"
        " LM           List messages addressed to you\n"
        " R(ead) n     Read message number n\n"
        " S(end) call  Send a private message (also SP call)\n"
        " SB cat       Send a bulletin (use ALL for everyone)\n"
        " U(sers)      Show who is connected\n"
        "End a message you are entering with /EX or Ctrl-Z on its own line."));
}

void PmsMailbox::cmdInfo()
{
    reply(QStringLiteral("AetherMailbox %1 at %2.")
              .arg(m_version, localAddress().toString()));
    if (!m_welcome.trimmed().isEmpty())
        reply(m_welcome.trimmed());
    reply(QStringLiteral("%1 message(s), %2 station(s) heard.")
              .arg(m_messages.size())
              .arg(m_heard.size()));
}

void PmsMailbox::cmdList(const QString& args, bool mineOnly)
{
    Q_UNUSED(args);
    if (m_messages.isEmpty()) {
        reply(QStringLiteral("No messages."));
        return;
    }
    reply(QStringLiteral("MSG#  ST  TO        FROM      DATE         SUBJECT"));
    // Newest first.
    for (int i = m_messages.size() - 1; i >= 0; --i) {
        const Message& m = m_messages.at(i);
        if (mineOnly && !sameCall(m.to, m_caller.toString()) && !sameCall(m.to, m_caller.call))
            continue;
        reply(QStringLiteral("%1  %2%3  %4  %5  %6  %7")
                  .arg(QString::number(m.id).rightJustified(4))
                  .arg(m.type)
                  .arg(m.read ? QLatin1Char('Y') : QLatin1Char('N'))
                  .arg(m.to.leftJustified(8).left(8),
                       m.from.leftJustified(8).left(8),
                       m.utc.toString(QStringLiteral("MM/dd HH:mm")),
                       m.subject));
    }
}

void PmsMailbox::cmdRead(const QString& args)
{
    bool ok = false;
    const int n = args.trimmed().toInt(&ok);
    if (!ok) {
        reply(QStringLiteral("USAGE: R <message-number>"));
        return;
    }
    for (Message& m : m_messages) {
        if (m.id != n)
            continue;
        if (!callerMayAccess(m)) {
            reply(QStringLiteral("Message %1 is private; not authorized.").arg(n));
            return;
        }
        reply(QStringLiteral("MSG #%1  %2  TO: %3  FROM: %4  %5")
                  .arg(m.id)
                  .arg(m.type)
                  .arg(m.to, m.from, m.utc.toString(QStringLiteral("yyyy-MM-dd HH:mm")) + QStringLiteral("Z")));
        reply(QStringLiteral("SUBJECT: %1").arg(m.subject));
        reply(m.body.isEmpty() ? QStringLiteral("(no text)") : m.body);
        reply(QStringLiteral("---"));
        if (!m.read && (sameCall(m.to, m_caller.toString()) || sameCall(m.to, m_caller.call))) {
            m.read = true;
            saveMessages();
        }
        return;
    }
    reply(QStringLiteral("Message %1 not found.").arg(n));
}

void PmsMailbox::cmdKill(const QString& args)
{
    bool ok = false;
    const int n = args.trimmed().toInt(&ok);
    if (!ok) {
        reply(QStringLiteral("USAGE: K <message-number>"));
        return;
    }
    for (int i = 0; i < m_messages.size(); ++i) {
        if (m_messages.at(i).id != n)
            continue;
        const Message& m = m_messages.at(i);
        const bool mayKill = sameCall(m.from, m_caller.toString())
            || sameCall(m.from, m_caller.call)
            || sameCall(m.to, m_caller.toString())
            || sameCall(m.to, m_caller.call)
            || sameCall(m.to, QStringLiteral("ALL"));
        if (!mayKill) {
            reply(QStringLiteral("Not authorized to kill message %1.").arg(n));
            return;
        }
        m_messages.remove(i);
        saveMessages();
        reply(QStringLiteral("Message %1 killed.").arg(n));
        emit stateChanged();
        return;
    }
    reply(QStringLiteral("Message %1 not found.").arg(n));
}

void PmsMailbox::cmdSendBegin(const QString& args, QChar type)
{
    QString to = args.trimmed();
    if (type == QLatin1Char('P')) {
        // First token is the destination callsign.
        const int sp = to.indexOf(QLatin1Char(' '));
        if (sp >= 0)
            to = to.left(sp);
        if (to.isEmpty()) {
            reply(QStringLiteral("USAGE: S <callsign>   (or S ALL for everyone)"));
            sendPrompt();
            return;
        }
    } else { // Bulletin
        if (to.isEmpty())
            to = QStringLiteral("ALL");
        const int sp = to.indexOf(QLatin1Char(' '));
        if (sp >= 0)
            to = to.left(sp);
    }

    m_draft = Message{};
    m_draft.type = type;
    m_draft.to = to.toUpper();
    m_draft.from = m_caller.toString();
    m_draftLines.clear();
    m_compose = Compose::Subject;
    reply(QStringLiteral("SUBJECT:"));
}

void PmsMailbox::finishCompose(bool save)
{
    if (save) {
        m_draft.id = m_nextId++;
        m_draft.utc = QDateTime::currentDateTimeUtc();
        m_draft.body = m_draftLines.join(QStringLiteral("\n"));
        m_draft.read = false;
        m_messages.append(m_draft);
        saveMessages();
        reply(QStringLiteral("MESSAGE %1 SAVED.").arg(m_draft.id));
        emit stateChanged();
    } else {
        reply(QStringLiteral("Message aborted."));
    }
    m_compose = Compose::None;
    m_draftLines.clear();
    sendPrompt();
}

void PmsMailbox::cmdJheard(const QString& args)
{
    Q_UNUSED(args);
    if (m_heard.isEmpty()) {
        reply(QStringLiteral("Nothing heard yet."));
        return;
    }
    QVector<Heard> sorted = m_heard;
    std::sort(sorted.begin(), sorted.end(),
              [](const Heard& a, const Heard& b) { return a.utc > b.utc; });
    reply(QStringLiteral("CALLSIGN   LAST HEARD (UTC)   VIA"));
    const int limit = std::min<int>(sorted.size(), 25);
    for (int i = 0; i < limit; ++i) {
        const Heard& h = sorted.at(i);
        reply(QStringLiteral("%1  %2  %3")
                  .arg(h.call.leftJustified(9),
                       h.utc.toString(QStringLiteral("MM/dd HH:mm")),
                       h.via));
    }
}

void PmsMailbox::cmdUsers()
{
    reply(QStringLiteral("Connected: %1").arg(m_caller.toString()));
}

bool PmsMailbox::callerMayAccess(const Message& msg) const
{
    if (msg.type == QLatin1Char('B'))
        return true;
    if (sameCall(msg.to, QStringLiteral("ALL")))
        return true;
    if (sameCall(msg.to, m_caller.toString()) || sameCall(msg.to, m_caller.call))
        return true;
    if (sameCall(msg.from, m_caller.toString()) || sameCall(msg.from, m_caller.call))
        return true;
    return false;
}

// ---------------------------------------------------------------------------
// Beacon
// ---------------------------------------------------------------------------

void PmsMailbox::sendBeaconNow()
{
    if (!m_enabled || !m_beaconEnabled)
        return;
    Address dest;
    dest.call = m_beaconDest.trimmed().toUpper().isEmpty()
        ? QStringLiteral("BEACON")
        : m_beaconDest.trimmed().toUpper();
    dest.ssid = 0;

    const QString text = QStringLiteral("%1  (connect to %2)")
        .arg(m_beaconText.trimmed(), localAddress().toString());
    const Frame frame = Frame::makeUI(dest, localAddress(), {}, text.toLatin1());
    emit transmitFrame(frame.encode());
    emit activity(QStringLiteral("PMS beacon sent: %1").arg(text));
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

QString PmsMailbox::messagesPath() const { return storageDir() + QStringLiteral("/messages.json"); }
QString PmsMailbox::callersPath() const { return storageDir() + QStringLiteral("/callers.json"); }
QString PmsMailbox::heardPath() const { return storageDir() + QStringLiteral("/heard.json"); }

void PmsMailbox::ensureStorageDir() const
{
    QDir().mkpath(storageDir());
}

void PmsMailbox::loadAll()
{
    m_loaded = true;
    m_messages.clear();
    m_callers.clear();
    m_heard.clear();
    m_nextId = 1;

    // Messages
    QFile mf(messagesPath());
    if (mf.open(QIODevice::ReadOnly)) {
        const QJsonObject root = QJsonDocument::fromJson(mf.readAll()).object();
        m_nextId = root.value(QStringLiteral("nextId")).toInt(1);
        for (const QJsonValue& v : root.value(QStringLiteral("messages")).toArray()) {
            const QJsonObject o = v.toObject();
            Message m;
            m.id = o.value(QStringLiteral("id")).toInt();
            m.type = o.value(QStringLiteral("type")).toString(QStringLiteral("P")).at(0);
            m.to = o.value(QStringLiteral("to")).toString();
            m.from = o.value(QStringLiteral("from")).toString();
            m.subject = o.value(QStringLiteral("subject")).toString();
            m.body = o.value(QStringLiteral("body")).toString();
            m.utc = QDateTime::fromString(o.value(QStringLiteral("utc")).toString(), Qt::ISODate);
            m.read = o.value(QStringLiteral("read")).toBool();
            m_messages.append(m);
            m_nextId = std::max(m_nextId, m.id + 1);
        }
    }

    // Callers
    QFile cf(callersPath());
    if (cf.open(QIODevice::ReadOnly)) {
        const QJsonObject root = QJsonDocument::fromJson(cf.readAll()).object();
        for (const QJsonValue& v : root.value(QStringLiteral("callers")).toArray()) {
            const QJsonObject o = v.toObject();
            Caller c;
            c.call = o.value(QStringLiteral("call")).toString();
            c.utc = QDateTime::fromString(o.value(QStringLiteral("utc")).toString(), Qt::ISODate);
            m_callers.append(c);
        }
    }

    // Heard
    QFile hf(heardPath());
    if (hf.open(QIODevice::ReadOnly)) {
        const QJsonObject root = QJsonDocument::fromJson(hf.readAll()).object();
        for (const QJsonValue& v : root.value(QStringLiteral("heard")).toArray()) {
            const QJsonObject o = v.toObject();
            Heard h;
            h.call = o.value(QStringLiteral("call")).toString();
            h.dest = o.value(QStringLiteral("dest")).toString();
            h.via = o.value(QStringLiteral("via")).toString();
            h.utc = QDateTime::fromString(o.value(QStringLiteral("utc")).toString(), Qt::ISODate);
            h.count = o.value(QStringLiteral("count")).toInt(1);
            m_heard.append(h);
        }
    }
}

void PmsMailbox::saveMessages() const
{
    ensureStorageDir();
    QJsonArray arr;
    for (const Message& m : m_messages) {
        QJsonObject o;
        o.insert(QStringLiteral("id"), m.id);
        o.insert(QStringLiteral("type"), QString(m.type));
        o.insert(QStringLiteral("to"), m.to);
        o.insert(QStringLiteral("from"), m.from);
        o.insert(QStringLiteral("subject"), m.subject);
        o.insert(QStringLiteral("body"), m.body);
        o.insert(QStringLiteral("utc"), m.utc.toString(Qt::ISODate));
        o.insert(QStringLiteral("read"), m.read);
        arr.append(o);
    }
    QJsonObject root;
    root.insert(QStringLiteral("nextId"), m_nextId);
    root.insert(QStringLiteral("messages"), arr);
    QFile f(messagesPath());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QJsonDocument(root).toJson());
}

void PmsMailbox::saveCallers() const
{
    ensureStorageDir();
    QJsonArray arr;
    for (const Caller& c : m_callers) {
        QJsonObject o;
        o.insert(QStringLiteral("call"), c.call);
        o.insert(QStringLiteral("utc"), c.utc.toString(Qt::ISODate));
        arr.append(o);
    }
    QJsonObject root;
    root.insert(QStringLiteral("callers"), arr);
    QFile f(callersPath());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QJsonDocument(root).toJson());
}

void PmsMailbox::saveHeard() const
{
    ensureStorageDir();
    QJsonArray arr;
    for (const Heard& h : m_heard) {
        QJsonObject o;
        o.insert(QStringLiteral("call"), h.call);
        o.insert(QStringLiteral("dest"), h.dest);
        o.insert(QStringLiteral("via"), h.via);
        o.insert(QStringLiteral("utc"), h.utc.toString(Qt::ISODate));
        o.insert(QStringLiteral("count"), h.count);
        arr.append(o);
    }
    QJsonObject root;
    root.insert(QStringLiteral("heard"), arr);
    QFile f(heardPath());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QJsonDocument(root).toJson());
}

} // namespace AetherSDR
