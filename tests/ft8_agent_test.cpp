// The FT8 receiver on the radio (tools/ft8-agent): its status and settings
// JSON parse; malformed replies are refused and out-of-range spots dropped;
// another agent's status is refused outright; the settings body round-trips
// through the receiver's own keys; only LAN or tailnet addresses are tried;
// and each bundled package matches its pin. Socket-free; writes only to a
// QTemporaryDir.
//
// The fixtures are REAL replies captured from a FLEX-6300 on 2026-11-08,
// not invented ones -- including the first live decode, which is also the
// spot that exposed the RR73 bug (see tools/ft8-agent/agent.c).

#include "core/DxSpot.h"
#include "core/Ft8Agent.h"
#include "core/LegacyWaveformPackage.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <cstdio>

#include <zlib.h>

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
    check(st->sliceIndex == -1, "a receiver before 0.1.3 names no slice index");
    {
        const auto with = [](const char* value) {
            return ft8::parseStatus(QByteArray(kStatus).replace(
                "\"slice\":true,", QByteArray("\"slice\":true,\"slice_index\":") + value + ","));
        };
        const auto five = with("5");
        check(five && five->sliceIndex == 5, "the receiver's slice index comes through");
        const auto zero = with("0");
        check(zero && zero->sliceIndex == 0, "slice index 0 is an index");
        for (const char* bad : {"-1", "64", "\"3\"", "null"}) {
            const auto b = with(bad);
            check(b && b->sliceIndex == -1, (QByteArray("an unusable slice index is none: ") + bad).constData());
        }
        const auto stale = ft8::parseStatus(QByteArray(kStatus).replace(
            "\"slice\":true,", "\"slice\":false,\"slice_index\":5,"));
        check(stale && stale->sliceIndex == -1, "an index without a slice is ignored");
        check(Ft8Config().daxChannel != 4, "the default DAX channel is not the WSPR receiver's");
    }
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
    const auto malformedCall = ft8::parseStatus(R"({"agent":"AetherFT8","callsign":"BAD CALL"})");
    check(malformedCall && malformedCall->callsign.isEmpty(),
          "a malformed receiver callsign is discarded without losing status");
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
{"t":1794194655,"band":"20m","score":25,"dt":1.0,"hz":14075503,"call":"BAD CALL","grid":"FN42","msg":"x"},
{"t":1794194655,"band":"20m","score":25,"dt":1.0,"hz":14075503,"call":"9Z4BM","grid":"ZZ42","msg":"x"},
{"t":9223372036854775807,"band":"20m","score":25,"dt":1.0,"hz":14075503,"call":"9Z4BM","grid":"FN42","msg":"x"},
{"t":0,"band":"20m","score":1,"dt":0.0,"hz":14075000,"call":"NOTIME","grid":"","msg":"z"}]})";
    auto st = ft8::parseStatus(QByteArray(mixed));
    check(st.has_value(), "a status with bad spots still parses");
    if (!st) {
        return;
    }
    check(st->spots.size() == 1, "only the good spot is kept");
    if (st->spots.size() != 1) {
        return;
    }
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
    check(ft8::reachableRadio(false, QHostAddress(QStringLiteral("192.168.0.31"))),
          "the connected radio on the LAN is tried");
    check(!ft8::reachableRadio(true, QHostAddress(QStringLiteral("192.168.0.31"))),
          "over SmartLink a LAN address left from an earlier session is not tried");
}

// The container is a gzipped tar of an OCI layout whose image config, a JSON
// blob stored as it is, carries the labels the radio lists it by.
QByteArray gunzip(const QByteArray& in)
{
    z_stream z{};
    if (inflateInit2(&z, 15 + 16) != Z_OK) {
        return {};
    }
    QByteArray out(4 * 1024 * 1024, Qt::Uninitialized);
    z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.constData()));
    z.avail_in = static_cast<uInt>(in.size());
    z.next_out = reinterpret_cast<Bytef*>(out.data());
    z.avail_out = static_cast<uInt>(out.size());
    const int rc = inflate(&z, Z_FINISH);
    out.truncate(rc == Z_STREAM_END ? static_cast<qsizetype>(z.total_out) : 0);
    inflateEnd(&z);
    return out;
}

QJsonObject containerLabels(const QByteArray& tarGz)
{
    const QByteArray tar = gunzip(tarGz);
    for (qsizetype at = 0; at + 512 <= tar.size();) {
        const QByteArray name = tar.mid(at, 100);
        if (name.at(0) == '\0') {
            break;
        }
        bool ok = false;
        const qsizetype size = tar.mid(at + 124, 12).trimmed().replace('\0', "").toLongLong(&ok, 8);
        if (!ok || at + 512 + size > tar.size()) {
            break;
        }
        const QJsonObject config = QJsonDocument::fromJson(tar.mid(at + 512, size)).object()
                                       .value(QLatin1String("config")).toObject();
        if (config.contains(QLatin1String("Labels"))) {
            return config.value(QLatin1String("Labels")).toObject();
        }
        at += 512 + (size + 511) / 512 * 512;
    }
    return {};
}

void testBundledPackages()
{
    check(isFt8Waveform(QStringLiteral("AetherFT8")) && !isFt8Waveform(QStringLiteral("AetherWSPR")),
          "the receiver's row is recognized, and nothing else is");
    for (const Ft8Package::Pin& pin : {Ft8Package::kLegacy, Ft8Package::kContainer}) {
        QTemporaryDir dir;
        QString error;
        const QString path = ft8::writeBundledPackage(pin, dir.path(), &error);
        QFile f(path);
        const QByteArray bytes = f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
        check(!path.isEmpty() && bytes.size() == pin.size
                  && QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex() == QByteArray(pin.sha256),
              "a bundled package is written and matches its pin");
    }
    QTemporaryDir dir;
    QString error;
    const LegacyWaveformPackageInfo info =
        inspectLegacyWaveformPackageFile(ft8::writeBundledPackage(Ft8Package::kLegacy, dir.path(), &error));
    check(info.valid && info.waveformName == QLatin1String(kFt8WaveformName)
              && info.version == QLatin1String(Ft8Package::kVersion),
          "the legacy one is a valid waveform named AetherFT8, at the pinned version");
    {
        QTemporaryDir container;
        QFile f(ft8::writeBundledPackage(Ft8Package::kContainer, container.path(), &error));
        const QByteArray bytes = f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
        check(bytes.left(2) == QByteArray::fromHex("1f8b"), "the container one is a gzip archive");
        const QJsonObject labels = containerLabels(bytes);
        check(labels.value(QLatin1String("com.flexradio.waveform.name")).toString() == QLatin1String(kFt8WaveformName)
                  && labels.value(QLatin1String("com.flexradio.waveform.version")).toString()
                         == QLatin1String(Ft8Package::kVersion),
              "its image labels name AetherFT8 at the pinned version");
    }
    QTemporaryDir other;
    QString why;
    Ft8Package::Pin wrong = Ft8Package::kContainer;
    wrong.resource = ":/cty.dat";
    const QString none = ft8::writeBundledPackage(wrong, other.path(), &why);
    check(none.isEmpty() && !why.isEmpty()
              && !QFile::exists(QDir(other.path()).filePath(QString::fromLatin1(wrong.fileName))),
          "a resource that doesn't match the pin is refused and nothing is written");
}

void testConfigureLists()
{
    const QStringList radio{QStringLiteral("ANT1"), QStringLiteral("ANT2"), QStringLiteral("XVTR")};
    check(ft8::antennaChoices(radio, QString()) == radio, "Radio default selected: the radio's antennas");
    check(ft8::antennaChoices(radio, QStringLiteral("ANT2")) == radio, "a listed antenna adds nothing");
    check(ft8::antennaChoices(radio, QStringLiteral("RX_A")) == radio + QStringList{QStringLiteral("RX_A")},
          "an antenna the radio doesn't list is kept after the radio's own");
    check(ft8::antennaChoices({}, QStringLiteral("ANT2")) == QStringList{QStringLiteral("ANT2")},
          "it is kept while the radio's list is still empty");
    check(ft8::daxChoices(3) == QList<int>({1, 2, 3, 4}), "a saved channel 1 to 4 is already offered");
    check(ft8::daxChoices(6) == QList<int>({1, 2, 3, 4, 6}), "a saved channel 5 to 8 is added");
    check(ft8::daxChoices(0) == QList<int>({1, 2, 3, 4}) && ft8::daxChoices(9) == QList<int>({1, 2, 3, 4}),
          "a channel the receiver refuses is not");
}

void testSkippedCycles()
{
    const auto with = ft8::parseStatus(R"({"agent":"AetherFT8","version":"0.1.8","skipped_cycles":12})");
    const auto without = ft8::parseStatus(R"({"agent":"AetherFT8","version":"0.1.6"})");
    const auto bad = ft8::parseStatus(R"({"agent":"AetherFT8","skipped_cycles":"many"})");
    const auto negative = ft8::parseStatus(R"({"agent":"AetherFT8","skipped_cycles":-3})");
    check(with && with->skippedCycles == 12, "the count of skipped cycles is read");
    check(without && without->skippedCycles == -1, "a receiver that doesn't count them reads as unknown");
    const auto fraction = ft8::parseStatus(R"({"agent":"AetherFT8","skipped_cycles":2.5})");
    const auto tooLarge = ft8::parseStatus(R"({"agent":"AetherFT8","skipped_cycles":1e25})");
    check(bad && bad->skippedCycles == -1 && negative && negative->skippedCycles == -1
              && fraction && fraction->skippedCycles == -1 && tooLarge && tooLarge->skippedCycles == -1,
          "a skipped-cycle count must be a representable non-negative integer");
}

void testBands()
{
    // ⚠ These must be the FT8 dials, not WSPR's. Getting this wrong puts the
    // receiver 21 kHz away from every signal it is looking for.
    check(ft8::bands().contains(QStringLiteral("20m")), "20 m is known");
    check(ft8::bands().contains(QStringLiteral("40m")), "40 m is known");
    check(!ft8::bands().contains(QStringLiteral("11m")), "11 m is not a band");
}

void testPanadapterSpot()
{
    Ft8Spot s;
    s.cycleStart = 1794194655;   // 2026-11-09 03:24:15 UTC
    s.score = 25;
    s.frequencyHz = 14075503;
    s.call = QStringLiteral("9Z4BM");
    s.grid = QStringLiteral("FK90ab");
    s.message = QStringLiteral("W0SJE 9Z4BM RR73");
    const DxSpot d = ft8::toDxSpot(s, QStringLiteral("G0JKN"));
    check(d.dxCall == QStringLiteral("9Z4BM"), "the spot is the transmitting station");
    check(d.spotterCall == QStringLiteral("G0JKN"), "the spotter is the receiver's callsign");
    check(qAbs(d.freqMhz - 14.075503) < 1e-9, "the frequency is the transmitter's, in MHz");
    check(d.source == QStringLiteral("FT8"), "the source is FT8");
    // ⚠ The score is a correlation strength, not dB. If this ever flips, a
    // number that is not an SNR is drawn as one beside real WSPR dB figures.
    check(!d.hasSnr && d.snr == 0, "the score is NOT published as an SNR");
    check(d.comment.contains(QStringLiteral("score 25")), "the score is in the comment, named as a score");
    check(d.comment.contains(QStringLiteral("W0SJE 9Z4BM RR73")), "the message is in the comment");
    check(d.dxGrid == QStringLiteral("FK90"), "the grid is cut to four characters");
    check(d.utcTime == QTime(3, 24, 15), "the time is the cycle's start, in UTC");
}

void testSpotHubStyle()
{
    Ft8Spot s;
    s.call = QStringLiteral("9Z4BM");
    s.frequencyHz = 14075503;
    WsprSpotHubSettings style;
    const DxSpot plain = ft8::styled(ft8::toDxSpot(s, QStringLiteral("G0JKN")), style);
    check(plain.lifetimeSec == 30 * 60, "an FT8 spot lives as long as SpotHub says, 30 minutes by default");
    check(plain.color.isEmpty(), "with no color chosen it is left to the theme's FT8 color");
    style.color = QStringLiteral("#12AB34");
    style.lifetimeMinutes = 2;
    const DxSpot chosen = ft8::styled(ft8::toDxSpot(s, QStringLiteral("G0JKN")), style);
    check(chosen.lifetimeSec == 120, "a chosen lifetime is carried by the spot, in seconds");
    check(chosen.color == QStringLiteral("#12AB34"), "a chosen color is carried by the spot");
    check(chosen.dxCall == QStringLiteral("9Z4BM"), "styling changes nothing else");
    check(QLatin1String(kFt8DefaultColorToken) != QLatin1String(kWsprDefaultColorToken),
          "FT8 and WSPR spots do not share a default color");
    check(QLatin1String(kFt8SpotHubKey) != QLatin1String("WsprSpots"),
          "FT8 keeps its SpotHub settings apart from WSPR's");
}

void testFeedIsOptIn()
{
    const QHostAddress lan(QStringLiteral("192.168.0.47"));
    WsprSpotHubSettings s;
    check(!ft8::feedStartsWithRadio(s, true, false, lan), "the feed does not start by itself until it is turned on");
    s.autoStart = true;
    check(ft8::feedStartsWithRadio(s, true, false, lan), "with Auto-Start on it starts with a LAN radio");
    check(!ft8::feedStartsWithRadio(s, false, false, lan), "but not on a radio that runs no waveforms");
    check(!ft8::feedStartsWithRadio(s, true, true, lan), "and not over SmartLink");
    check(!ft8::feedStartsWithRadio(s, true, false, QHostAddress(QStringLiteral("8.8.8.8"))),
          "and not at a public address");
}

void testSpotsSince()
{
    QList<Ft8Spot> all;
    for (qint64 t : {100, 115, 115, 130}) {
        Ft8Spot s;
        s.cycleStart = t;
        all.append(s);
    }
    qint64 next = 0;
    check(ft8::spotsSince(all, 115, &next).size() == 3, "a cycle at the mark is included");
    check(next == 131, "the mark moves past the newest cycle, so it is not sent twice");
    check(ft8::spotsSince(all, next, &next).isEmpty(), "polling again sends nothing");
    check(next == 131, "and the mark stays where it was");
    check(ft8::spotsSince(all, 0, nullptr).size() == 4, "no mark to write is allowed");
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
    testBundledPackages();
    testConfigureLists();
    testSkippedCycles();
    testBands();
    testPanadapterSpot();
    testSpotHubStyle();
    testFeedIsOptIn();
    testSpotsSince();
    std::printf("\n%s (%d failure%s)\n", g_failed ? "FAILED" : "ALL PASSED", g_failed,
                g_failed == 1 ? "" : "s");
    return g_failed ? 1 : 0;
}
