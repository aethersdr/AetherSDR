// Tests for the QRZ callsign-lookup building blocks:
//   • Callsigns:: regex/normalization helpers
//   • CwCallsignSpotter "DE <call> <call>" stream detection (chunked
//     arrival, end-of-stream settle timer, re-spot suppression, garble)
//   • CallsignInfo JSON round-trip + TTL staleness (the 7-day cache rule)

#include "core/CallsignInfo.h"
#include "core/CallsignUtils.h"
#include "core/CtyDatParser.h"
#include "core/CwCallsignSpotter.h"
#include "core/MaidenheadLocator.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QSignalSpy>
#include <QTemporaryFile>
#include <QTimer>

#include <cmath>
#include <cstdio>

using namespace AetherSDR;

namespace {

int failures = 0;

void check(bool condition, const char* name)
{
    if (condition) {
        std::printf("[PASS] %s\n", name);
    } else {
        std::printf("[FAIL] %s\n", name);
        ++failures;
    }
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ── Callsigns helpers ───────────────────────────────────────────────
    check(Callsigns::isLikelyCallsign("KI6BCJ"),   "plain US callsign");
    check(Callsigns::isLikelyCallsign("W1AW"),     "short US callsign");
    check(Callsigns::isLikelyCallsign("kk7gwy"),   "lowercase normalizes");
    check(Callsigns::isLikelyCallsign("VE3ABC"),   "Canadian callsign");
    check(Callsigns::isLikelyCallsign("VP2E/K1AB"),"prefixed portable");
    check(Callsigns::isLikelyCallsign("K1AB/7"),   "portable district suffix");
    check(!Callsigns::isLikelyCallsign("5NN"),     "CW report rejected");
    check(!Callsigns::isLikelyCallsign("73"),      "numbers-only rejected");
    check(!Callsigns::isLikelyCallsign("QTH"),     "no-digit word rejected");
    check(!Callsigns::isLikelyCallsign(""),        "empty rejected");
    check(Callsigns::normalized(" ki6bcj ") == "KI6BCJ", "normalized trims + uppercases");

    // ── Spotter: basic doubled-call detection ───────────────────────────
    {
        CwCallsignSpotter spotter;
        QSignalSpy spy(&spotter, &CwCallsignSpotter::callsignSpotted);
        spotter.feedText("CQ CQ CQ DE KI6BCJ KI6BCJ K");
        check(spy.count() == 1, "doubled call after DE spots once");
        check(spy.count() == 1 && spy.at(0).at(0).toString() == "KI6BCJ",
              "spotted call is the callsign");
    }

    // ── Spotter: chunked (character-by-character) arrival ───────────────
    {
        CwCallsignSpotter spotter;
        QSignalSpy spy(&spotter, &CwCallsignSpotter::callsignSpotted);
        const QString stream = QStringLiteral("CQ DE W1AW W1AW K");
        for (const QChar& c : stream)
            spotter.feedText(QString(c));
        check(spy.count() == 1, "char-by-char stream spots once");
        check(spy.count() == 1 && spy.at(0).at(0).toString() == "W1AW",
              "chunked spotted call correct");
    }

    // ── Spotter: single (undoubled) call must NOT spot ──────────────────
    {
        CwCallsignSpotter spotter;
        QSignalSpy spy(&spotter, &CwCallsignSpotter::callsignSpotted);
        spotter.feedText("CQ CQ DE KI6BCJ K TU 73 ");
        check(spy.count() == 0, "single call after DE does not spot");
    }

    // ── Spotter: garbled repeat must NOT spot ───────────────────────────
    {
        CwCallsignSpotter spotter;
        QSignalSpy spy(&spotter, &CwCallsignSpotter::callsignSpotted);
        spotter.feedText("CQ DE KI6BCJ KI6BCX K ");
        check(spy.count() == 0, "mismatched repeat does not spot");
    }

    // ── Spotter: "MADE" false positive guard ────────────────────────────
    {
        CwCallsignSpotter spotter;
        QSignalSpy spy(&spotter, &CwCallsignSpotter::callsignSpotted);
        spotter.feedText("RIG HOMEMADE K1AB K1AB K ");
        check(spy.count() == 0, "DE inside a word does not spot");
    }

    // ── Spotter: re-spot suppression for the same call ──────────────────
    {
        CwCallsignSpotter spotter;
        QSignalSpy spy(&spotter, &CwCallsignSpotter::callsignSpotted);
        spotter.feedText("CQ DE W1AW W1AW K ");
        spotter.feedText("CQ DE W1AW W1AW K ");
        check(spy.count() == 1, "same call twice within window spots once");
        spotter.feedText("CQ DE KI6BCJ KI6BCJ K ");
        check(spy.count() == 2, "different call still spots");
    }

    // ── Spotter: ID at end of transmission (settle timer) ───────────────
    {
        CwCallsignSpotter spotter;
        QSignalSpy spy(&spotter, &CwCallsignSpotter::callsignSpotted);
        spotter.feedText("73 73 DE K1AB K1AB");  // stream ends here — no trailing char
        check(spy.count() == 0, "end-of-stream match held until settle");
        // The settle timer is 3 s; wait it out with a bounded event loop.
        QSignalSpy settled(&spotter, &CwCallsignSpotter::callsignSpotted);
        settled.wait(5000);
        check(spy.count() == 1 && spy.at(0).at(0).toString() == "K1AB",
              "end-of-stream ID spots after settle");
    }

    // ── Spotter: clear() drops a partial match ───────────────────────────
    {
        CwCallsignSpotter spotter;
        QSignalSpy spy(&spotter, &CwCallsignSpotter::callsignSpotted);
        spotter.feedText("CQ DE W1");
        spotter.clear();                 // slice change mid-ID
        spotter.feedText("AW W1AW K ");
        check(spy.count() == 0, "clear() between chunks prevents cross-station splice");
    }

    // ── CallsignInfo: JSON round-trip ────────────────────────────────────
    {
        CallsignInfo info;
        info.call = "KI6BCJ";
        info.firstName = "Pat";
        info.lastName = "Jensen";
        info.nameFmt = "Pat Jensen";
        info.city = "San Jose";
        info.state = "CA";
        info.country = "United States";
        info.county = "Santa Clara";
        info.grid = "CM97";
        info.licenseClass = "E";
        info.imageUrl = "https://example.org/photo.jpg";
        info.latitude = 37.3;
        info.longitude = -121.9;
        info.hasLatLon = true;
        info.lotw = true;
        info.fetchedUtc = QDateTime::currentSecsSinceEpoch();

        const CallsignInfo back = CallsignInfo::fromJson(info.toJson());
        check(back.call == info.call, "round-trip call");
        check(back.displayName() == "Pat Jensen", "displayName uses name_fmt");
        check(back.displayLocation() == "San Jose, CA, United States",
              "displayLocation joins city/state/country");
        check(back.grid == "CM97" && back.county == "Santa Clara",
              "round-trip grid + county");
        check(back.hasLatLon && back.latitude == info.latitude
              && back.longitude == info.longitude, "round-trip lat/lon");
        check(back.lotw && !back.eqsl, "round-trip QSL flags");
        check(back.fetchedUtc == info.fetchedUtc, "round-trip fetch time");
    }

    // ── CallsignInfo: display fallbacks ──────────────────────────────────
    {
        CallsignInfo info;
        info.call = "W1AW";
        info.firstName = "Hiram";
        info.lastName = "Maxim";
        check(info.displayName() == "Hiram Maxim", "displayName falls back to fname+name");
        info.firstName.clear();
        info.lastName.clear();
        check(info.displayName() == "W1AW", "displayName falls back to call");
        check(info.displayLocation().isEmpty(), "empty location joins to empty");
    }

    // ── CtyDatParser: entity lat/lon for the prefix-fallback card ───────
    {
        // Two entities in AD1C cty.dat format.  NOTE: the file stores
        // longitude WEST-positive; the parser must flip it east-positive.
        QTemporaryFile f;
        f.open();
        f.write("United States:            5:  8:  NA:   37.53:    91.67:     5.0:  K:\n"
                "    K,W,N,AA,AB,KI6,=W1AW;\n"
                "England:                 14: 27:  EU:   52.77:     1.47:     0.0:  G:\n"
                "    G,M,2E;\n");
        f.close();

        CtyDatParser cty;
        check(cty.loadFromFile(f.fileName()), "cty.dat sample loads");
        const DxccEntity* us = cty.entityForCallsign("KI6BCJ");
        check(us && us->name == "United States", "KI6BCJ resolves to United States");
        check(us && us->hasLatLon && std::abs(us->latitude - 37.53) < 0.01
              && std::abs(us->longitude - (-91.67)) < 0.01,
              "US lat/lon parsed, longitude flipped east-positive");
        check(us && us->continent == "NA" && us->cqZone == 5,
              "US continent + CQ zone parsed");
        const DxccEntity* uk = cty.entityForCallsign("G4ABC");
        check(uk && uk->name == "England" && uk->hasLatLon,
              "G4ABC resolves to England with lat/lon");
        check(cty.entityForCallsign("ZZ9ZZZ") == nullptr
              || !cty.entityForCallsign("ZZ9ZZZ"),
              "unknown prefix resolves to null");
    }

    // ── Distance + bearing sanity (San Jose CM97 → ARRL FN31) ───────────
    {
        double km = 0.0, brg = 0.0;
        check(MaidenheadLocator::gridDistance("CM97", "FN31", km, brg),
              "grid distance computes");
        check(km > 4000 && km < 4400, "CM97→FN31 distance ≈ 4200 km");
        check(brg > 55 && brg < 90, "CM97→FN31 initial bearing ≈ ENE");
    }

    // ── TTL staleness — the 7-day cache rule ────────────────────────────
    {
        constexpr qint64 kTtl = 7 * 24 * 3600;
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        CallsignInfo info;
        info.call = "W1AW";
        info.fetchedUtc = now - 3 * 24 * 3600;
        check(!info.isOlderThan(kTtl, now), "3-day-old entry is fresh");
        info.fetchedUtc = now - 8 * 24 * 3600;
        check(info.isOlderThan(kTtl, now), "8-day-old entry is stale");
        info.fetchedUtc = 0;
        check(info.isOlderThan(kTtl, now), "never-fetched entry is stale");
    }

    std::printf(failures ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", failures);
    return failures ? 1 : 0;
}
