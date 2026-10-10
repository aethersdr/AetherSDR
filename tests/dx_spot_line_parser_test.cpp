#include "core/DxSpotLineParser.h"

#include <QString>
#include <cmath>
#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) {
        ++g_failed;
    }
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

void expectInt(const char* name, int got, int want)
{
    if (got == want) {
        report(name, true);
    } else {
        std::printf("[FAIL] %s — got %d, want %d\n", name, got, want);
        ++g_failed;
    }
}

void expectNear(const char* name, double got, double want, double eps = 1e-9)
{
    if (std::fabs(got - want) < eps) {
        report(name, true);
    } else {
        std::printf("[FAIL] %s — got %f, want %f\n", name, got, want);
        ++g_failed;
    }
}

DxSpot parse(const QString& line, bool goCluster, bool* ok = nullptr)
{
    DxSpot spot;
    const bool parsed = DxSpotLineParser::parseSpotLine(line, spot, goCluster);
    if (ok) {
        *ok = parsed;
    }
    return spot;
}

// The servers that predate GoCluster must parse exactly as before, apart
// from the comment's inner padding being collapsed.
void testGenericServers()
{
    bool ok = false;

    DxSpot s = parse("DX de W3LPL:     14025.0  JA1ABC       CW big signal       1824Z", false, &ok);
    expectTrue("dxspider: parses", ok);
    expectEqual("dxspider: spotter", s.spotterCall, "W3LPL");
    expectNear("dxspider: freq MHz", s.freqMhz, 14.025);
    expectEqual("dxspider: dx call", s.dxCall, "JA1ABC");
    expectEqual("dxspider: comment", s.comment, "CW big signal");
    expectTrue("dxspider: time", s.utcTime == QTime(18, 24));
    expectTrue("dxspider: no snr", !s.hasSnr);
    expectTrue("dxspider: no GoCluster fields",
               s.dxGrid.isEmpty() && s.confidence.isNull() && s.pathGlyph.isNull());

    // DXSpider's set/dxgrid locator and AR-Cluster's state follow the time.
    s = parse("DX de K1TTT:     21025.0  VP8LP        up 2                           1305Z FN32", false, &ok);
    expectTrue("dxspider grid after time: parses", ok);
    expectEqual("dxspider grid after time: comment", s.comment, "up 2");
    expectTrue("dxspider grid after time: time", s.utcTime == QTime(13, 5));

    s = parse("DX de N6TV:      7012.0  K1XYZ        599 tnx QSO               0230Z CA", false, &ok);
    expectTrue("ar-cluster: parses", ok);
    expectEqual("ar-cluster: comment", s.comment, "599 tnx QSO");

    s = parse("DX de W1AW:      28074.0  ZL1ABC       FT8   loud   in   NE          2101Z", false, &ok);
    expectTrue("cc-cluster: parses", ok);
    expectEqual("cc-cluster: padding collapsed", s.comment, "FT8 loud in NE");

    s = parse("DX de W3LPL-#:   14025.0  JA1ABC       CW    22 dB  25 WPM  CQ      1824Z", false, &ok);
    expectTrue("rbn cw: parses", ok);
    expectEqual("rbn cw: padding collapsed", s.comment, "CW 22 dB 25 WPM CQ");
    expectTrue("rbn cw: snr read", s.hasSnr);
    expectInt("rbn cw: snr", s.snr, 22);

    s = parse("DX de KM3T-#:    14074.0  W1AW         FT8   -12 dB  CQ          1824Z", false, &ok);
    expectTrue("rbn ft8: parses", ok);
    expectInt("rbn ft8: negative snr", s.snr, -12);

    s = parse("dx de w3lpl:     3525.5  ja1abc       cw                          0001z", false, &ok);
    expectTrue("lowercase: parses", ok);
    expectNear("lowercase: freq MHz", s.freqMhz, 3.5255);

    s = parse("DX de W3LPL:     14025.0  JA1ABC       QSL via FN31 V              1824Z", false, &ok);
    expectTrue("comment ending grid+letter: parses", ok);
    expectEqual("comment ending grid+letter: untouched", s.comment, "QSL via FN31 V");
    expectTrue("comment ending grid+letter: no GoCluster fields",
               s.dxGrid.isEmpty() && s.confidence.isNull());

    parse("Hello KK7GWY, this is N2WQ-2", false, &ok);
    expectTrue("non-spot line rejected", !ok);
    parse("DX de W3LPL:     0.0  JA1ABC       CW                          1824Z", false, &ok);
    expectTrue("zero frequency rejected", !ok);
}

// GoCluster's shipped 76-column layout: tail is the last 15 columns.
void testGoCluster()
{
    bool ok = false;

    DxSpot s = parse("DX de W1ABC:     14074.00  K1XYZ       FT8 -12 dB             > FN31 V 1830Z", true, &ok);
    expectTrue("gocluster: parses", ok);
    expectEqual("gocluster: spotter", s.spotterCall, "W1ABC");
    expectNear("gocluster: freq MHz", s.freqMhz, 14.074);
    expectEqual("gocluster: dx call", s.dxCall, "K1XYZ");
    expectEqual("gocluster: comment loses the tail", s.comment, "FT8 -12 dB");
    expectEqual("gocluster: grid", s.dxGrid, "FN31");
    expectTrue("gocluster: confidence", s.confidence == QLatin1Char('V'));
    expectTrue("gocluster: path glyph", s.pathGlyph == QLatin1Char('>'));
    expectTrue("gocluster: time", s.utcTime == QTime(18, 30));
    expectTrue("gocluster: snr read", s.hasSnr);
    expectInt("gocluster: snr", s.snr, -12);

    s = parse("DX de N2WQ-2-#:    7025.0  JA1ABC      CW 23 dB 25 WPM CQ              0102Z", true, &ok);
    expectTrue("gocluster blank tail: parses", ok);
    expectEqual("gocluster blank tail: comment", s.comment, "CW 23 dB 25 WPM CQ");
    expectTrue("gocluster blank tail: no grid", s.dxGrid.isEmpty());
    expectTrue("gocluster blank tail: no confidence", s.confidence.isNull());
    expectTrue("gocluster blank tail: no glyph", s.pathGlyph.isNull());
    expectTrue("gocluster blank tail: time", s.utcTime == QTime(1, 2));
    expectInt("gocluster blank tail: snr", s.snr, 23);

    s = parse("DX de KM3T-#:     14074.0  W1AW        FT8 +3 dB              = fn42 ? 2359Z", true, &ok);
    expectTrue("gocluster derived grid: parses", ok);
    expectEqual("gocluster derived grid: kept lowercase", s.dxGrid, "fn42");
    expectTrue("gocluster unverified: confidence", s.confidence == QLatin1Char('?'));
    expectInt("gocluster: plus snr", s.snr, 3);

    s = parse("DX de W1ABC:       1840.0  VP8LP                              > GD18 C 1200Z", true, &ok);
    expectTrue("gocluster empty comment: parses", ok);
    expectEqual("gocluster empty comment: comment", s.comment, "");
    expectEqual("gocluster empty comment: dx call", s.dxCall, "VP8LP");
    expectTrue("gocluster empty comment: corrected", s.confidence == QLatin1Char('C'));

    // A gated line that isn't GoCluster-shaped falls back to the generic parse.
    s = parse("DX de W3LPL:     14025.0  JA1ABC       CW big signal       1824Z", true, &ok);
    expectTrue("gocluster fallback: parses", ok);
    expectEqual("gocluster fallback: comment", s.comment, "CW big signal");
    expectTrue("gocluster fallback: no GoCluster fields",
               s.dxGrid.isEmpty() && s.confidence.isNull() && s.pathGlyph.isNull());

    // Trailing whitespace (a stripped BEL) does not hide the anchored tail.
    s = parse("DX de KM3T-#:     14074.0  W1AW        FT8 +3 dB              = fn42 ? 2359Z  ", true, &ok);
    expectTrue("gocluster trailing space: parses", ok);
    expectTrue("gocluster trailing space: confidence", s.confidence == QLatin1Char('?'));
    expectEqual("gocluster trailing space: comment", s.comment, "FT8 +3 dB");

    // Same line, gate off: the tail stays in the comment, like before.
    s = parse("DX de W1ABC:     14074.00  K1XYZ       FT8 -12 dB             > FN31 V 1830Z", false, &ok);
    expectTrue("gocluster line ungated: parses", ok);
    expectEqual("gocluster line ungated: comment", s.comment, "FT8 -12 dB > FN31 V");
    expectTrue("gocluster line ungated: no confidence", s.confidence.isNull());

    expectTrue("banner: N2WQ greeting",
               DxSpotLineParser::isGoClusterBanner("N2WQ-2 GoCluster DX Cluster"));
    expectTrue("banner: default welcome", DxSpotLineParser::isGoClusterBanner("GoCluster"));
    expectTrue("banner: DXSpider is not", !DxSpotLineParser::isGoClusterBanner(
                   "Hello, this is W3LPL-2 running DXSpider V1.57 build 594"));
}

// Saved-log replay: provenance comes from the GoCluster marker the client
// logs at the start of the session, never from whether the banner survived
// the tail, and never from server text later in the log.
void testReplay()
{
    const QString verified =
        "DX de W1ABC:     14074.00  K1XYZ       FT8 -12 dB             > FN31 V 1830Z";
    const QString unverified =
        "DX de KM3T-#:     14074.0  W1AW        FT8 +3 dB              = fn42 ? 2359Z";
    const QString header = "--- Connected to cluster.n2wq.com:8300 at 2026-10-09 02:56:16 UTC ---";
    const QString marker = DxSpotLineParser::logGoClusterMarker();
    const QStringList goHead = {header, "N2WQ-2 GoCluster DX Cluster", marker,
                                "Hello KK7GWY, this is N2WQ-2"};

    // The head holds the marker; the 500-line tail holds only spots.
    QVector<DxSpot> spots = DxSpotLineParser::replayLog(goHead, {verified, unverified}, true);
    expectInt("replay banner outside tail: '?' hidden", spots.size(), 1);
    if (spots.size() == 1) {
        expectEqual("replay banner outside tail: kept call", spots[0].dxCall, "K1XYZ");
        expectEqual("replay banner outside tail: clean comment", spots[0].comment, "FT8 -12 dB");
    }

    spots = DxSpotLineParser::replayLog(goHead, {verified, unverified}, false);
    expectInt("replay hide off: both kept", spots.size(), 2);
    if (spots.size() == 2) {
        expectTrue("replay hide off: confidence kept", spots[1].confidence == QLatin1Char('?'));
    }

    // A small file: head and tail are the same lines.
    const QStringList whole = goHead + QStringList{verified, unverified};
    spots = DxSpotLineParser::replayLog(whole, whole, true);
    expectInt("replay whole file: '?' hidden", spots.size(), 1);

    // Server text in the tail that looks like a session header or the marker
    // cannot reset or set provenance.
    spots = DxSpotLineParser::replayLog(
        goHead, {verified, "--- Connected to evil:1 at 2026-10-09 04:00:00 UTC ---", unverified}, true);
    expectInt("replay forged header in tail: '?' still hidden", spots.size(), 1);
    const QStringList dxspiderHead = {"--- Connected to dxc.nc7j.com:7300 at 2026-10-09 04:00:00 UTC ---",
                                      "Hello KK7GWY, this is NC7J"};
    spots = DxSpotLineParser::replayLog(dxspiderHead, {marker, unverified}, true);
    expectInt("replay forged marker in tail: generic parse", spots.size(), 1);
    if (spots.size() == 1) {
        expectTrue("replay forged marker in tail: not GoCluster", spots[0].confidence.isNull());
    }

    // A marker after the first spot is server output, not the client's gate.
    spots = DxSpotLineParser::replayLog(dxspiderHead + QStringList{verified, marker}, {unverified}, true);
    expectInt("replay marker after first spot: ignored", spots.size(), 1);

    // Banner text alone (no marker) is not provenance.
    spots = DxSpotLineParser::replayLog({header, "N2WQ-2 GoCluster DX Cluster"}, {unverified}, true);
    expectInt("replay without marker: generic parse", spots.size(), 1);

    spots = DxSpotLineParser::replayLog({}, {}, true);
    expectInt("replay empty log", spots.size(), 0);
}

void testReportSnr()
{
    int snr = 99;
    expectTrue("snr: cw", DxSpotLineParser::extractReportSnr("CW 23 dB 25 WPM CQ", snr) && snr == 23);
    expectTrue("snr: ft8 plus", DxSpotLineParser::extractReportSnr("FT8 +12 dB", snr) && snr == 12);
    expectTrue("snr: zero is a reading", DxSpotLineParser::extractReportSnr("RTTY 0 dB", snr) && snr == 0);
    expectTrue("snr: uppercase DB", DxSpotLineParser::extractReportSnr("CW 9 DB 18 WPM CQ", snr) && snr == 9);
    expectTrue("snr: not from free text",
               !DxSpotLineParser::extractReportSnr("big sig 20 dB over S9", snr));
    expectTrue("snr: not a bare number", !DxSpotLineParser::extractReportSnr("599 tnx", snr));
    expectTrue("snr: not a dBm-ish word", !DxSpotLineParser::extractReportSnr("CW 23 dBm", snr));
    expectTrue("snr: empty", !DxSpotLineParser::extractReportSnr("", snr));
}

} // namespace

int main()
{
    testGenericServers();
    testGoCluster();
    testReplay();
    testReportSnr();

    if (g_failed == 0) {
        std::printf("\nAll DX spot line parser tests passed.\n");
        return 0;
    }
    std::printf("\n%d DX spot line parser test(s) failed.\n", g_failed);
    return 1;
}
