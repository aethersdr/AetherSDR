#include "core/N1MMSpotParser.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QString>
#include <cmath>
#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) ++g_failed;
}

void expectTrue(const char* name, bool ok)
{
    report(name, ok);
}

void expectEqual(const char* name, const QString& got, const QString& want)
{
    if (got == want) {
        report(name, true);
    } else {
        std::printf("[FAIL] %s — got \"%s\", want \"%s\"\n",
                    name, qUtf8Printable(got), qUtf8Printable(want));
        ++g_failed;
    }
}

void expectNear(const char* name, double got, double want, double eps = 1e-6)
{
    if (std::fabs(got - want) < eps) {
        report(name, true);
    } else {
        std::printf("[FAIL] %s — got %f, want %f\n", name, got, want);
        ++g_failed;
    }
}

QByteArray n1mmPacket(const QString& dxcall, const QString& freq,
                      const QString& action = "add",
                      const QString& status = "single mult",
                      const QString& mode = "CW",
                      const QString& spottercall = "K2PO/7-#",
                      const QString& comment = "CW 9 DB 18 WPM CQ AK")
{
    QString xml = QString(
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<spot>\n"
        "  <app>N1MM</app>\n"
        "  <StationName>CONTEST-PC</StationName>\n"
        "  <dxcall>%1</dxcall>\n"
        "  <frequency>%2</frequency>\n"
        "  <spottercall>%3</spottercall>\n"
        "  <timestamp>2026-05-21 17:19:37</timestamp>\n"
        "  <action>%4</action>\n"
        "  <mode>%5</mode>\n"
        "  <comment>%6</comment>\n"
        "  <status>%7</status>\n"
        "  <statuslist>%7</statuslist>\n"
        "</spot>\n")
        .arg(dxcall, freq, spottercall, action, mode, comment, status);
    return xml.toUtf8();
}

void testWellFormedAdd()
{
    N1mmSpot spot;
    QString action;
    const bool ok = N1MMSpotParser::parsePacket(
        n1mmPacket("AL3CDE", "7061.2"), spot, action);
    expectTrue("well-formed add parses", ok);
    expectEqual("add action", action, "add");
    expectEqual("dxcall", spot.dxCall, "AL3CDE");
    expectNear("kHz->MHz conversion", spot.freqMhz, 7.0612);
    expectEqual("mode uppercased", spot.mode, "CW");
    expectEqual("spottercall", spot.spotterCall, "K2PO/7-#");
    expectEqual("comment", spot.comment, "CW 9 DB 18 WPM CQ AK");
    expectEqual("statusRaw", spot.statusRaw, "single mult");
    expectEqual("statusFlag normalized to mult", spot.statusFlag, "mult");
    expectTrue("timestamp parsed", spot.timestamp.isValid());
}

void testTimestampAndStationName()
{
    // Both feed user-visible output: the timestamp drives the panadapter
    // marker's age/fade, and stationName becomes the "N1MM-<StationName>"
    // source string SmartSDR uses.
    N1mmSpot spot;
    QString action;
    expectTrue("packet parses", N1MMSpotParser::parsePacket(
                   n1mmPacket("AL3CDE", "7061.2"), spot, action));
    expectEqual("stationName captured", spot.stationName, "CONTEST-PC");
    expectEqual("timestamp parsed as UTC",
                spot.timestamp.toUTC().toString("yyyy-MM-dd HH:mm:ss"),
                "2026-05-21 17:19:37");

    // A malformed or absent timestamp must leave the field invalid rather
    // than yielding a bogus date — callers fall back to "now" on !isValid().
    N1mmSpot noTs;
    expectTrue("packet without timestamp parses", N1MMSpotParser::parsePacket(
                   "<spot><dxcall>W1ABC</dxcall><frequency>14025.0</frequency></spot>",
                   noTs, action));
    expectTrue("absent timestamp is invalid", !noTs.timestamp.isValid());

    N1mmSpot badTs;
    expectTrue("packet with unparseable timestamp still parses",
               N1MMSpotParser::parsePacket(
                   "<spot><dxcall>W1ABC</dxcall><frequency>14025.0</frequency>"
                   "<timestamp>not-a-date</timestamp></spot>", badTs, action));
    expectTrue("unparseable timestamp is invalid", !badTs.timestamp.isValid());
}

void testWellFormedDelete()
{
    N1mmSpot spot;
    QString action;
    const bool ok = N1MMSpotParser::parsePacket(
        n1mmPacket("AL3CDE", "7061.2", "delete", ""), spot, action);
    expectTrue("well-formed delete parses", ok);
    expectEqual("delete action", action, "delete");
    expectEqual("delete dxcall", spot.dxCall, "AL3CDE");
    expectNear("delete freq kHz->MHz", spot.freqMhz, 7.0612);
}

void testActionDefaultsToAdd()
{
    // <action> omitted entirely — build a minimal packet without it.
    const QByteArray xml =
        "<spot><dxcall>W1ABC</dxcall><frequency>14025.0</frequency></spot>";
    N1mmSpot spot;
    QString action;
    const bool ok = N1MMSpotParser::parsePacket(xml, spot, action);
    expectTrue("packet without <action> parses", ok);
    expectEqual("missing action defaults to add", action, "add");
}

void testMissingOptionalFields()
{
    const QByteArray xml =
        "<spot><dxcall>K5ABC</dxcall><frequency>14200.0</frequency></spot>";
    N1mmSpot spot;
    QString action;
    const bool ok = N1MMSpotParser::parsePacket(xml, spot, action);
    expectTrue("minimal packet parses", ok);
    expectEqual("minimal packet mode empty", spot.mode, "");
    expectEqual("minimal packet comment empty", spot.comment, "");
    expectEqual("minimal packet statusFlag empty", spot.statusFlag, "");
    expectEqual("minimal packet spottercall empty", spot.spotterCall, "");
}

void testMissingDxcallFails()
{
    N1mmSpot spot;
    QString action;
    const QByteArray xml = "<spot><frequency>14025.0</frequency></spot>";
    expectTrue("missing dxcall rejected", !N1MMSpotParser::parsePacket(xml, spot, action));
}

void testMissingOrInvalidFrequencyFails()
{
    N1mmSpot spot;
    QString action;
    expectTrue("missing frequency rejected",
               !N1MMSpotParser::parsePacket("<spot><dxcall>W1ABC</dxcall></spot>", spot, action));
    expectTrue("zero frequency rejected",
               !N1MMSpotParser::parsePacket(
                   "<spot><dxcall>W1ABC</dxcall><frequency>0</frequency></spot>", spot, action));
    expectTrue("non-numeric frequency rejected",
               !N1MMSpotParser::parsePacket(
                   "<spot><dxcall>W1ABC</dxcall><frequency>abc</frequency></spot>", spot, action));
}

void testInvalidActionFails()
{
    N1mmSpot spot;
    QString action;
    expectTrue("unrecognized action rejected",
               !N1MMSpotParser::parsePacket(
                   n1mmPacket("AL3CDE", "7061.2", "modify"), spot, action));
}

void testMalformedXmlFails()
{
    N1mmSpot spot;
    QString action;
    expectTrue("truncated XML rejected",
               !N1MMSpotParser::parsePacket("<spot><dxcall>AL3CDE</dxcall>", spot, action));
    expectTrue("not-xml-at-all rejected",
               !N1MMSpotParser::parsePacket("this is not xml", spot, action));
    expectTrue("empty datagram rejected",
               !N1MMSpotParser::parsePacket("", spot, action));
    expectTrue("wrong root element rejected",
               !N1MMSpotParser::parsePacket(
                   "<notaspot><dxcall>AL3CDE</dxcall><frequency>7061.2</frequency></notaspot>",
                   spot, action));
}

void testTruncatedMidFrequencyRejected()
{
    // The gap the earlier truncation tests missed: they only passed because
    // their fixtures also lacked a <frequency>. A datagram cut mid-number has
    // a valid dxcall AND digits, so without an xml.hasError() check it parses
    // as a real spot at the wrong frequency — "140" → 0.140 MHz, wrong band,
    // wrong spotKey(). onReadyRead()'s 64 KB cap and UDP fragment loss both
    // produce this shape.
    N1mmSpot spot;
    QString action;
    expectTrue("truncated mid-frequency rejected",
               !N1MMSpotParser::parsePacket(
                   "<spot><dxcall>W1ABC</dxcall><frequency>140", spot, action));
    expectTrue("truncated mid-frequency with closing tag missing rejected",
               !N1MMSpotParser::parsePacket(
                   "<spot><dxcall>W1ABC</dxcall><frequency>14025.0", spot, action));
    // Truncated after a complete, well-formed frequency element is still a
    // torn document — the reader flags it and we drop it rather than guess.
    expectTrue("truncated after complete frequency rejected",
               !N1MMSpotParser::parsePacket(
                   "<spot><dxcall>W1ABC</dxcall><frequency>14025.0</frequency>",
                   spot, action));
}

void testOversizedDatagramDoesNotCrash()
{
    // Unterminated <dxcall> swallows 200 KB of filler and the document never
    // reaches <frequency>, so this must be rejected (no frequency captured)
    // deterministically, regardless of exactly where QXmlStreamReader gives
    // up on the truncated input. The real assertion is that this doesn't
    // crash/hang the process — if it did, the test binary itself would fail.
    QByteArray junk("<spot><dxcall>");
    junk.append(QByteArray(200000, 'X'));   // ~200 KB of filler, no closing tags
    N1mmSpot spot;
    QString action;
    const bool ok = N1MMSpotParser::parsePacket(junk, spot, action);
    expectTrue("oversized malformed datagram rejected without crashing", !ok);
}

void testStrayTextBetweenElements()
{
    // currentTag must reset on EndElement, or text sitting between one
    // element closing and the next opening gets appended to the element that
    // just closed — here "junk" would land on <frequency>, turning "14025.0"
    // into "14025.0junk" and failing toDouble() on an otherwise-valid packet.
    const QByteArray xml =
        "<spot><dxcall>W1ABC</dxcall><frequency>14025.0</frequency>junk"
        "<mode>CW</mode></spot>";
    N1mmSpot spot;
    QString action;
    const bool ok = N1MMSpotParser::parsePacket(xml, spot, action);
    expectTrue("stray text between elements still parses", ok);
    expectNear("stray text does not corrupt frequency", spot.freqMhz, 14.025);
    expectEqual("stray text does not corrupt mode", spot.mode, "CW");
    expectEqual("stray text does not corrupt dxcall", spot.dxCall, "W1ABC");
}

void testSpotKeyIdentity()
{
    const QString keyA = N1MMSpotParser::spotKey("AL3CDE", 7.0612);   // 40m
    const QString keyB = N1MMSpotParser::spotKey("AL3CDE", 7.0500);   // still 40m
    expectEqual("same call, same band -> same key (replacement)", keyA, keyB);

    const QString keyC = N1MMSpotParser::spotKey("AL3CDE", 14.0600);  // 20m
    expectTrue("same call, different band -> different key (new spot)", keyA != keyC);

    const QString keyD = N1MMSpotParser::spotKey(" al3cde ", 7.0612);
    expectEqual("callsign normalized (trim+uppercase) for key equality", keyA, keyD);

    const QString keyE = N1MMSpotParser::spotKey("W1ABC", 7.0612);
    expectTrue("different call, same band -> different key", keyA != keyE);
}

void testStatusFlagPriority()
{
    using N1MMSpotParser::normalizeStatusFlag;
    expectEqual("single mult -> mult", normalizeStatusFlag("single mult"), "mult");
    expectEqual("double mult -> mult", normalizeStatusFlag("double mult"), "mult");
    expectEqual("dupe -> dupe", normalizeStatusFlag("dupe"), "dupe");
    expectEqual("bust -> bust", normalizeStatusFlag("bust"), "bust");
    expectEqual("cq -> cq", normalizeStatusFlag("cq"), "cq");
    expectEqual("busy -> busy", normalizeStatusFlag("busy"), "busy");
    expectEqual("qtc -> qtc", normalizeStatusFlag("qtc"), "qtc");
    expectEqual("new qso -> new", normalizeStatusFlag("new qso"), "new");
    expectEqual("empty -> empty", normalizeStatusFlag(""), "");
    // Priority when multiple tokens are present: bust beats everything, since
    // a busted call is the most urgent thing to flag to the operator.
    expectEqual("bust beats dupe", normalizeStatusFlag("dupe bust"), "bust");
    expectEqual("dupe beats mult", normalizeStatusFlag("dupe single mult"), "dupe");
    expectEqual("mult beats cq", normalizeStatusFlag("cq single mult"), "mult");
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    testWellFormedAdd();
    testTimestampAndStationName();
    testWellFormedDelete();
    testActionDefaultsToAdd();
    testMissingOptionalFields();
    testMissingDxcallFails();
    testMissingOrInvalidFrequencyFails();
    testInvalidActionFails();
    testMalformedXmlFails();
    testTruncatedMidFrequencyRejected();
    testOversizedDatagramDoesNotCrash();
    testStrayTextBetweenElements();
    testSpotKeyIdentity();
    testStatusFlagPriority();

    return g_failed == 0 ? 0 : 1;
}
