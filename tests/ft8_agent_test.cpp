// The FT8 receiver on the radio (tools/ft8-agent): its status and settings
// JSON parse; malformed replies are refused and out-of-range spots dropped;
// another agent's status is refused outright; the settings body round-trips
// through the receiver's own keys; and only LAN or tailnet addresses are
// tried. Socket-free.
//
// The fixtures are REAL replies captured from a FLEX-6300 on 2026-11-08,
// not invented ones -- including the first live decode, which is also the
// spot that exposed the RR73 bug (see tools/ft8-agent/agent.c).

#include "core/Ft8Agent.h"

#include <QCoreApplication>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void check(bool ok, const char* label)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", label);
    if (!ok) {
        ++g_failed;
    }
}

// A status reply as the receiver writes it (agent.c status_json), captured
// from the radio. The spot is the first live FT8 decode on a FLEX-6300.
const char* kStatus = R"({"agent":"AetherFT8","version":"0.1.0","state":"capturing",
"clock_source":"radio_ntp","clock_synced":true,"uptime_s":482,"callsign":"G0JKN/W3","grid":"",
"band":"20m","dial_hz":14074000,"slice":true,"audio":true,
"cycles":[{"t":1794194640,"band":"20m","dial_hz":14074000,"spots":0,"decode_ms":1899,
"audio_pct":100,"lost_packets":0,"audio_dbfs":-33.6},
{"t":1794194655,"band":"20m","dial_hz":14074000,"spots":1,"decode_ms":2275,
"audio_pct":100,"lost_packets":0,"audio_dbfs":-33.5}],
"spots":[{"t":1794194655,"band":"20m","score":25,"dt":1.0,"hz":14075503,"mode":"FT8",
"call":"9Z4BM","grid":"","msg":"W0SJE 9Z4BM RR73"}]})";

const char* kConfig = R"({"config":{"callsign":"G0JKN/W3","grid":"","band":"20m",
"rxant":"ANT2","dax_channel":2},"claimed":true})";

void testStatus()
{
    auto st = ft8::parseStatus(QByteArray(kStatus));
    check(st.has_value(), "a real status reply parses");
    if (!st) {
        return;
    }
    check(st->state == QLatin1String("capturing"), "state");
    check(st->band == QLatin1String("20m"), "band");
    check(st->dialHz == 14074000, "dial is the FT8 frequency, not WSPR's 14.0956");
    check(st->slice && st->audio, "slice and audio");
    // A FLEX-6000's kernel never clears STA_UNSYNC, so the agent falls back
    // to the radio's own NTP. That is normal there, not a degraded state.
    check(st->clockSource == QLatin1String("radio_ntp"), "clock source is reported");
    check(st->clockSynced, "clock synced");
    check(st->cycles.size() == 2, "both cycles");
    check(st->cycles.at(1).decodeMs == 2275, "decode time");
    // The measurement RFC #7114 turns on: this cycle overran its budget.
    check(st->cycles.at(1).decodeMs > kDecodeBudgetMs, "an over-budget decode is visible as such");
    check(st->cycles.at(0).decodeMs < kDecodeBudgetMs, "and an in-budget one is not");
    check(st->spots.size() == 1, "the spot");
    check(st->spots.at(0).call == QLatin1String("9Z4BM"), "the transmitting station");
    check(st->spots.at(0).grid.isEmpty(), "RR73 is a sign-off, so this spot has NO grid");
    check(st->spots.at(0).message == QLatin1String("W0SJE 9Z4BM RR73"), "the message as sent");
    check(st->spots.at(0).frequencyHz == 14075503, "the transmitter's frequency");
    check(st->spots.at(0).score == 25, "ft8_lib's candidate score");
}

// The window shows the last decode time against the cycle budget, so a
// status poll must come back WITH its cycles.
//
// This is here because the window first polled with `since = now + 3600`,
// copied from the WSPR window where that is a deliberate way to skip the
// spots. The agent applies that filter to the cycles as well, so every poll
// returned 0 cycles and the decode line never drew. The window looked
// perfectly healthy; the line was simply absent.
void testCyclesSurviveAPoll()
{
    auto st = ft8::parseStatus(QByteArray(kStatus));
    check(st.has_value() && !st->cycles.isEmpty(), "a status poll carries its cycles");
    if (!st || st->cycles.isEmpty()) {
        return;
    }
    // Whatever the window decides to show, it reads the newest cycle.
    check(st->cycles.last().start >= st->cycles.first().start, "cycles are in order, newest last");
    check(st->cycles.last().decodeMs > 0, "the newest cycle has a decode time to show");
}

void testStatusRefusals()
{
    check(!ft8::parseStatus(QByteArray("not json")), "garbage is refused");
    check(!ft8::parseStatus(QByteArray("[]")), "a JSON array is refused");
    check(!ft8::parseStatus(QByteArray("{}")), "an object without an agent is refused");
    // AetherWSPR answers the same shape on 48995. Reading its status as FT8
    // would show WSPR spots in the FT8 window, so refuse it outright.
    check(!ft8::parseStatus(QByteArray(R"({"agent":"AetherWSPR","state":"capturing"})")),
          "another agent's status is refused, not half-read");
}

void testSpotDropping()
{
    // One unreadable spot is dropped; the rest of the status survives. A
    // frequency of 0 was a real failure mode in the WSPR agent.
    const char* mixed = R"({"agent":"AetherFT8","state":"capturing","spots":[
{"t":1794194655,"band":"20m","score":25,"dt":1.0,"hz":0,"call":"BAD","grid":"","msg":"x"},
{"t":1794194655,"band":"20m","score":25,"dt":1.0,"hz":14075503,"call":"9Z4BM","grid":"FN42","msg":"y"},
{"t":0,"band":"20m","score":1,"dt":0.0,"hz":14075000,"call":"NOTIME","grid":"","msg":"z"}]})";
    auto st = ft8::parseStatus(QByteArray(mixed));
    check(st.has_value(), "a status with bad spots still parses");
    if (!st) {
        return;
    }
    check(st->spots.size() == 1, "only the good spot is kept");
    check(st->spots.at(0).call == QLatin1String("9Z4BM"), "and it is the right one");
    check(st->spots.at(0).grid == QLatin1String("FN42"), "a real grid is kept");
}

void testConfig()
{
    bool claimed = false;
    auto c = ft8::parseConfig(QByteArray(kConfig), &claimed);
    check(c.has_value(), "a real config reply parses");
    if (!c) {
        return;
    }
    check(claimed, "the receiver reports it is claimed");
    check(c->band == QLatin1String("20m"), "band");
    check(c->rxant == QLatin1String("ANT2"), "antenna");
    check(c->daxChannel == 2, "DAX channel");

    // A DAX channel outside 1-8 is not something the agent accepts.
    check(!ft8::parseConfig(QByteArray(R"({"config":{"band":"20m","dax_channel":0},"claimed":false})")),
          "DAX channel 0 is refused");
    check(!ft8::parseConfig(QByteArray(R"({"config":{"band":"11m","dax_channel":2},"claimed":false})")),
          "a band the receiver does not know is refused");
    check(!ft8::parseConfig(QByteArray(R"({"claimed":true})")), "a reply without settings is refused");
}

void testConfigRoundTrip()
{
    Ft8Config c;
    c.callsign = QStringLiteral("G0JKN/W3");
    c.grid = QStringLiteral("FM19");
    c.band = QStringLiteral("40m");
    c.rxant = QStringLiteral("ANT2");
    c.daxChannel = 3;

    // The body must use the receiver's own keys, or it answers 400.
    const QJsonObject o = QJsonDocument::fromJson(ft8::configJson(c)).object();
    check(o.value(QStringLiteral("dax_channel")).toInt() == 3, "dax_channel is the receiver's key");
    check(o.value(QStringLiteral("rxant")).toString() == QLatin1String("ANT2"), "rxant is the receiver's key");

    // And what we send must come back as what we sent.
    const QByteArray reply = "{\"config\":" + ft8::configJson(c) + ",\"claimed\":true}";
    auto back = ft8::parseConfig(reply);
    check(back.has_value() && *back == c, "settings round-trip through the receiver's keys");
}

void testAdminToken()
{
    const char* first = R"({"config":{"band":"20m","dax_channel":2},
"admin_token":"0123456789abcdef0123456789abcdef"})";
    check(ft8::adminToken(QByteArray(first)).size() == 32, "the first POST yields a token");
    check(ft8::adminToken(QByteArray(kConfig)).isEmpty(), "a later reply carries none");
    // Anything that is not a hex token is not one.
    check(ft8::adminToken(QByteArray(R"({"admin_token":"../../etc/passwd"})")).isEmpty(),
          "a token that is not hex is refused");
}

void testReachable()
{
    check(ft8::reachableAddress(QHostAddress(QStringLiteral("192.168.0.31"))), "the radio on the LAN");
    check(ft8::reachableAddress(QHostAddress(QStringLiteral("10.0.0.51"))), "a 10/8 LAN");
    check(ft8::reachableAddress(QHostAddress(QStringLiteral("100.125.126.37"))), "a tailnet address");
    check(!ft8::reachableAddress(QHostAddress(QStringLiteral("8.8.8.8"))), "a public address is not tried");
    check(!ft8::reachableAddress(QHostAddress()), "a null address is not tried");
}

void testBands()
{
    // ⚠ These must be the FT8 dials, not WSPR's. Getting this wrong puts the
    // receiver 21 kHz away from every signal it is looking for.
    check(ft8::bands().contains(QStringLiteral("20m")), "20 m is known");
    check(ft8::bands().contains(QStringLiteral("40m")), "40 m is known");
    check(!ft8::bands().contains(QStringLiteral("11m")), "11 m is not a band");
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testStatus();
    testCyclesSurviveAPoll();
    testStatusRefusals();
    testSpotDropping();
    testConfig();
    testConfigRoundTrip();
    testAdminToken();
    testReachable();
    testBands();
    std::printf("\n%s (%d failure%s)\n", g_failed ? "FAILED" : "ALL PASSED", g_failed,
                g_failed == 1 ? "" : "s");
    return g_failed ? 1 : 0;
}
