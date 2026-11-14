#include "core/hfgateway/ModemHostProtocol.h"

#include <QByteArray>
#include <QList>
#include <QString>

#include <cstdio>

using namespace AetherSDR::ModemHost;

namespace {

int g_failed = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) {
        ++g_failed;
    }
}

NotificationType nt(const char* line) { return parseNotification(QString::fromLatin1(line)).type; }

// The framed lines as text, with "<rejected>" standing in for a rejected one
// so its position is checked too.
QList<QString> texts(const QList<Line>& lines)
{
    QList<QString> out;
    for (const Line& l : lines) {
        out.append(l.rejected ? QStringLiteral("<rejected>") : l.text);
    }
    return out;
}
CommandType ct(const char* line) { return parseCommand(QString::fromLatin1(line)).type; }

}  // namespace

int main()
{
    // ── Framing ─────────────────────────────────────────────────────────────
    {
        LineSplitter s;
        const QList<QString> a = texts(s.feed(QByteArray("OK\rPENDING\rCONN")));
        const QList<QString> b = texts(s.feed(QByteArray("ECTED N0CALL N1CALL 2300\r")));
        report("joined lines split in one read", a == QList<QString>{"OK", "PENDING"});
        report("a line split across reads is reassembled", b == QList<QString>{"CONNECTED N0CALL N1CALL 2300"});
    }
    {
        LineSplitter s;
        report("CRLF framing tolerated", texts(s.feed(QByteArray("OK\r\nWRONG\r\n"))) == QList<QString>{"OK", "WRONG"});
        report("empty lines dropped", texts(s.feed(QByteArray("\r\r\rOK\r"))) == QList<QString>{"OK"});
    }
    {
        LineSplitter s;
        QByteArray huge(kMaxLineBytes + 10, 'A');
        huge.append('\r');
        huge.append("OK\r");
        const QList<QString> out = texts(s.feed(QByteArray("VERSION\r") + huge));
        report("an oversized line is rejected in its place",
               out == QList<QString>{"VERSION", "<rejected>", "OK"});
        report("the oversized line is counted", s.rejectedLines() == 1);
        s.reset();
        report("reset() zeroes the count", s.rejectedLines() == 0);
    }
    {
        LineSplitter s;
        QByteArray atLimit(kMaxLineBytes, 'A');
        atLimit.append('\r');
        report("a line exactly at the cap is accepted", s.feed(atLimit).size() == 1);
    }
    {
        LineSplitter s;
        QByteArray bad("PTT O");
        bad.append('\x01');
        bad.append("N\rOK\r");
        const QList<QString> out = texts(s.feed(bad));
        report("a line with a control byte is rejected in its place", out == QList<QString>{"<rejected>", "OK"});
        report("the control-byte line is counted", s.rejectedLines() == 1);
        QByteArray high("BUSY ON");
        high.append('\xC3');
        high.append('\r');
        report("a line with a non-ASCII byte is rejected", texts(s.feed(high)) == QList<QString>{"<rejected>"});
    }

    // ── Validation helpers ──────────────────────────────────────────────────
    report("plain callsign valid", isValidCallsign("N0CALL"));
    report("callsign with SSID valid", isValidCallsign("AB1CD-7") && isValidCallsign("AB1CD-15"));
    report("portable suffix valid", isValidCallsign("N0CALL/P") && isValidCallsign("VE3/N0CALL"));
    report("tactical name without a digit valid", isValidCallsign("NOCALL"));
    report("15 characters valid (Mercury's limit)", isValidCallsign("ABCDEFGHIJKLMN1"));
    report("16 characters rejected", !isValidCallsign("ABCDEFGHIJKLMNO1"));
    report("lower case rejected (callers upper-case)", !isValidCallsign("n0call"));
    report("leading or trailing separator rejected", !isValidCallsign("-N0CALL") && !isValidCallsign("N0CALL-")
                                                         && !isValidCallsign("/N0CALL"));
    report("other characters rejected", !isValidCallsign("N0 CALL") && !isValidCallsign("N0CALL.") 
                                            && !isValidCallsign("N0CALL_1"));
    report("empty rejected", !isValidCallsign(""));
    report("known bandwidths", isKnownBandwidth(500) && isKnownBandwidth(1200) && isKnownBandwidth(2300)
                                   && isKnownBandwidth(2750));
    report("unknown bandwidth rejected", !isKnownBandwidth(2400) && !isKnownBandwidth(0));

    // ── Modem -> host notifications ─────────────────────────────────────────
    report("OK", nt("OK") == NotificationType::Ok);
    report("WRONG", nt("WRONG") == NotificationType::Wrong);
    report("IAMALIVE", nt("IAMALIVE") == NotificationType::IAmAlive);
    report("PENDING", nt("PENDING") == NotificationType::Pending);
    report("CANCELPENDING", nt("CANCELPENDING") == NotificationType::CancelPending);
    report("DISCONNECTED", nt("DISCONNECTED") == NotificationType::Disconnected);
    report("DISCONNECTED with a reason", nt("DISCONNECTED ABORT") == NotificationType::Disconnected);
    {
        const Notification on = parseNotification("PTT ON");
        const Notification off = parseNotification("PTT OFF");
        report("PTT ON / OFF", on.type == NotificationType::Ptt && on.on && off.type == NotificationType::Ptt
                                   && !off.on);
    }
    report("PTT token matched exactly", nt("PTT ONX") == NotificationType::Unknown
                                            && nt("PTT") == NotificationType::Unknown
                                            && nt("PTT ON NOW") == NotificationType::Unknown
                                            && nt("XPTT ON") == NotificationType::Unknown);
    {
        const Notification b = parseNotification("BUSY ON");
        report("BUSY ON", b.type == NotificationType::Busy && b.on);
        report("BUSY garbage rejected", nt("BUSY MAYBE") == NotificationType::Unknown);
    }
    {
        const Notification c = parseNotification("CONNECTED N0CALL N1CALL-10 2750");
        report("CONNECTED fields", c.type == NotificationType::Connected && c.source == "N0CALL"
                                       && c.destination == "N1CALL-10" && c.bandwidthHz == 2750);
        report("CONNECTED with an unknown bandwidth rejected",
               nt("CONNECTED N0CALL N1CALL 2400") == NotificationType::Unknown);
        report("CONNECTED with a bad callsign rejected",
               nt("CONNECTED N0CALL bad 2300") == NotificationType::Unknown);
        report("CONNECTED with missing fields rejected", nt("CONNECTED N0CALL N1CALL") == NotificationType::Unknown);
    }
    {
        const Notification b = parseNotification("BUFFER 1234");
        report("BUFFER", b.type == NotificationType::Buffer && b.bytes == 1234);
        report("BUFFER 0", parseNotification("BUFFER 0").bytes == 0);
        report("BUFFER above the cap rejected", nt("BUFFER 99999999") == NotificationType::Unknown);
        report("BUFFER negative rejected", nt("BUFFER -5") == NotificationType::Unknown);
        report("BUFFER non-numeric rejected", nt("BUFFER 12x") == NotificationType::Unknown);
    }
    {
        const Notification s = parseNotification("SN 8.2");
        report("SN", s.type == NotificationType::SignalToNoise && s.snDb > 8.19 && s.snDb < 8.21);
        report("SN negative", parseNotification("SN -12.5").snDb < -12.4);
        report("SN out of range rejected", nt("SN 99") == NotificationType::Unknown);
        report("SN garbage rejected", nt("SN nan") == NotificationType::Unknown
                                          && nt("SN 1e9") == NotificationType::Unknown);
    }
    {
        const Notification b = parseNotification("BITRATE (17) 2400 BPS");
        report("BITRATE", b.type == NotificationType::Bitrate && b.bitrateCode == 17 && b.bitsPerSecond == 2400);
        report("BITRATE malformed rejected", nt("BITRATE 17 2400 BPS") == NotificationType::Unknown
                                                 && nt("BITRATE (17) 2400") == NotificationType::Unknown);
    }
    {
        const Notification r = parseNotification("REGISTERED N0CALL");
        report("REGISTERED", r.type == NotificationType::Registered && r.callsign == "N0CALL");
        report("REGISTERED bad callsign rejected", nt("REGISTERED x") == NotificationType::Unknown);
    }
    {
        const Notification v = parseNotification("VERSION Data2G 0.5.0");
        report("VERSION text", v.type == NotificationType::Version && v.text == "Data2G 0.5.0");
    }
    {
        const Notification q = parseNotification("CQFRAME N0CALL 500");
        report("CQFRAME", q.type == NotificationType::CqFrame && q.source == "N0CALL" && q.bandwidthHz == 500);
    }
    {
        const Notification m = parseNotification("MONITOR DATAC16 CALL FROM=K7EK BW=2300 SID=42 SNR=18.1");
        report("MONITOR CALL", m.type == NotificationType::Monitor && m.monitorMode == "DATAC16"
                                   && m.monitorKind == "CALL" && m.monitorFrom == "K7EK" && m.source == "K7EK"
                                   && m.bandwidthHz == 2300 && m.monitorSnDb > 18.0);
        const Notification a = parseNotification("MONITOR DATAC16 ACCEPT BW=2300 SID=42 START=3 SNR=17.9");
        report("MONITOR ACCEPT has no FROM", a.type == NotificationType::Monitor && a.monitorKind == "ACCEPT"
                                                 && a.monitorFrom.isEmpty());
        const Notification bad = parseNotification("MONITOR DATAC16 CQ FROM=bad_call BW=999 SNR=900");
        report("MONITOR invalid fields left empty", bad.type == NotificationType::Monitor && bad.monitorFrom.isEmpty()
                                                        && bad.bandwidthHz == 0 && bad.monitorSnDb == 0.0);
        report("MONITOR query reply is not a frame", nt("MONITOR ON") == NotificationType::Unknown);
        report("MONITOR odd mode word rejected", nt("MONITOR data;16 CALL") == NotificationType::Unknown);
        report("cmd MONITOR", cmdMonitor(true) == "MONITOR ON\r" && cmdMonitor(false) == "MONITOR OFF\r");
    }
    report("unknown line surfaced as Unknown with raw", parseNotification("FOO BAR").raw == "FOO BAR"
                                                             && nt("FOO BAR") == NotificationType::Unknown);
    report("lower-case token not matched", nt("ok") == NotificationType::Unknown);

    // ── Commands the gateway sends ──────────────────────────────────────────
    report("MYCALL one", cmdMyCall({"N0CALL"}) == "MYCALL N0CALL\r");
    report("MYCALL five", cmdMyCall({"N0CALL", "N0CALL-1", "N0CALL-2", "N0CALL-3", "N0CALL-4"})
                              == "MYCALL N0CALL N0CALL-1 N0CALL-2 N0CALL-3 N0CALL-4\r");
    report("MYCALL six refused", cmdMyCall({"N0CALL", "N0CALL-1", "N0CALL-2", "N0CALL-3", "N0CALL-4", "N0CALL-5"})
                                     .isEmpty());
    report("MYCALL empty refused", cmdMyCall({}).isEmpty());
    report("MYCALL invalid refused", cmdMyCall({"N0CALL", "bad call"}).isEmpty());
    report("LISTEN ON / OFF", cmdListen(true) == "LISTEN ON\r" && cmdListen(false) == "LISTEN OFF\r");
    report("DISCONNECT / ABORT", cmdDisconnect() == "DISCONNECT\r" && cmdAbort() == "ABORT\r");
    report("queries", cmdVersion() == "VERSION\r" && cmdBuffer() == "BUFFER\r" && cmdSn() == "SN\r");

    // ── Host -> modem commands (the gateway as server) ──────────────────────
    {
        const Command m = parseCommand("MYCALL N0CALL N0CALL-1");
        report("MYCALL parsed", m.type == CommandType::MyCall && m.callsigns == QStringList{"N0CALL", "N0CALL-1"});
        report("MYCALL without calls invalid", ct("MYCALL") == CommandType::Invalid);
        report("MYCALL bad call invalid", ct("MYCALL N0CALL N0_CALL") == CommandType::Invalid);
        report("MYCALL too many invalid",
               ct("MYCALL N0CALL N0CALL-1 N0CALL-2 N0CALL-3 N0CALL-4 N0CALL-5") == CommandType::Invalid);
    }
    report("LISTEN ON/OFF/CQ", ct("LISTEN ON") == CommandType::ListenOn && ct("LISTEN OFF") == CommandType::ListenOff
                                   && ct("LISTEN CQ") == CommandType::ListenCq);
    report("LISTEN garbage invalid", ct("LISTEN MAYBE") == CommandType::Invalid
                                         && ct("LISTEN") == CommandType::Invalid);
    {
        const Command c = parseCommand("CONNECT N0CALL N1CALL");
        report("CONNECT parsed", c.type == CommandType::Connect && c.source == "N0CALL" && c.destination == "N1CALL");
        report("CONNECT bad args invalid", ct("CONNECT N0CALL") == CommandType::Invalid
                                               && ct("CONNECT N0CALL N0_CALL") == CommandType::Invalid);
    }
    {
        const Command b = parseCommand("BW2300");
        report("BW2300", b.type == CommandType::Bandwidth && b.bandwidthHz == 2300);
        report("BW unknown invalid", ct("BW2400") == CommandType::Invalid && ct("BWX") == CommandType::Invalid);
    }
    report("CQFRAME parsed", parseCommand("CQFRAME N0CALL 500").bandwidthHz == 500);
    report("transmit-class commands recognised", ct("TUNE 1") == CommandType::Tune
                                                     && ct("ENCRYPTION ON") == CommandType::Encryption
                                                     && ct("CLEANTXBUFFER") == CommandType::CleanTxBuffer);
    report("simple commands", ct("DISCONNECT") == CommandType::Disconnect && ct("ABORT") == CommandType::Abort
                                  && ct("VERSION") == CommandType::Version && ct("BUFFER") == CommandType::Buffer
                                  && ct("SN") == CommandType::Sn && ct("COMPRESSION TEXT") == CommandType::Compression
                                  && ct("PUBLIC ON") == CommandType::Public && ct("CHAT OFF") == CommandType::Chat);
    report("unknown command", ct("FROB") == CommandType::Unknown && ct("") == CommandType::Unknown);
    {
        const Command m = parseCommand("mycall n0call");
        report("commands are case-insensitive", m.type == CommandType::MyCall && m.callsigns == QStringList{"N0CALL"}
                                                    && ct("listen on") == CommandType::ListenOn
                                                    && ct("Bw2300") == CommandType::Bandwidth);
    }

    // ── Notifications the gateway sends to its back end ─────────────────────
    report("fmt simple", fmtOk() == "OK\r" && fmtWrong() == "WRONG\r" && fmtIAmAlive() == "IAMALIVE\r"
                             && fmtPending() == "PENDING\r" && fmtCancelPending() == "CANCELPENDING\r"
                             && fmtDisconnected() == "DISCONNECTED\r");
    report("fmt PTT/BUSY", fmtPtt(true) == "PTT ON\r" && fmtBusy(false) == "BUSY OFF\r");
    report("fmt CONNECTED", fmtConnected("N0CALL", "N1CALL", 2300) == "CONNECTED N0CALL N1CALL 2300\r");
    report("fmt CONNECTED refuses invalid", fmtConnected("N0CALL", "bad", 2300).isEmpty()
                                                && fmtConnected("N0CALL", "N1CALL", 2400).isEmpty());
    report("fmt BUFFER", fmtBuffer(0) == "BUFFER 0\r" && fmtBuffer(-1).isEmpty()
                             && fmtBuffer(kMaxBufferBytes + 1).isEmpty());
    report("fmt SN", fmtSn(8.25) == "SN 8.3\r" || fmtSn(8.25) == "SN 8.2\r");
    report("fmt SN refuses out of range", fmtSn(100.0).isEmpty());
    report("fmt VERSION strips control bytes", fmtVersion(QStringLiteral("Aether\x01SDR")) == "VERSION AetherSDR\r");
    report("fmt VERSION refuses empty", fmtVersion(QStringLiteral("\x01")).isEmpty());

    // Round trip: what the server formats, the client parses back.
    {
        const Notification c = parseNotification(QString::fromLatin1(fmtConnected("N0CALL", "N1CALL", 500)).trimmed());
        report("round trip CONNECTED", c.type == NotificationType::Connected && c.bandwidthHz == 500);
        const Notification b = parseNotification(QString::fromLatin1(fmtBuffer(42)).trimmed());
        report("round trip BUFFER", b.type == NotificationType::Buffer && b.bytes == 42);
    }

    if (g_failed != 0) {
        std::printf("\n%d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("\nall checks passed\n");
    return 0;
}
