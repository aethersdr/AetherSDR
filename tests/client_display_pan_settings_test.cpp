// FFT FPS and the dBm range in the per-radio ClientDisplay document, against
// the real settings store: round trip, ownership, refusals, schema, coexistence
// with other tables, the pending-write key.
// SOURCE TEXT, last block only: the order and presence of the save and restore
// calls inside MainWindow methods, which link into no test.

#include "TestSettingsProfile.h"
#include "core/ClientDisplaySettings.h"
#include "core/DbmRangePlausibility.h"
#include "core/backends/RadioCapabilities.h"
#include "core/backends/flex/FlexBackend.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/icom/IcomCivBackend.h"
#include "gui/DeferredSettingsWrites.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonObject>

#include <cmath>
#include <cstdio>
#include <limits>

using namespace AetherSDR;
using CDS = AetherSDR::ClientDisplaySettings;

static int g_failed = 0;
static void check(bool ok, const char* name)
{
    if (!ok) {
        ++g_failed;
        std::fprintf(stderr, "FAIL: %s\n", name);
    }
}

static QString readSource(const char* relative)
{
    QFile file(QStringLiteral(AETHER_SOURCE_DIR) + QLatin1Char('/')
               + QLatin1String(relative));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

static bool sameRange(const std::optional<CDS::DbmRange>& got, float min, float max)
{
    return got && got->minDbm == min && got->maxDbm == max;
}

int main(int argc, char** argv)
{
    // Before QCoreApplication: the backends constructed below read AppSettings.
    TestSettingsProfile profile(QStringLiteral("client-display-pan-settings"));
    if (!profile.isValid()) { return 1; }
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();

    const QString feature = QStringLiteral("ClientDisplay");
    const RadioSettingsScope a(QStringLiteral("hl2"), QStringLiteral("A"));
    const RadioSettingsScope b(QStringLiteral("hl2"), QStringLiteral("B"));
    const RadioSettingsScope flex(QStringLiteral("flex"), QStringLiteral("A"));
    const RadioSettingsScope unknown(QStringLiteral("hl2"), {});

    // 1. Round trip, per pan slot and per radio. The observed values.
    check(!CDS::fftFps(a, 0, true) && !CDS::dbmRange(a, 0, true),
          "nothing stored reads as unconfigured, not as a default");
    CDS::saveFftFps(a, 0, true, 12);
    CDS::saveDbmRange(a, 0, true, -122.19f, -2.19f);
    check(CDS::fftFps(a, 0, true) == 12, "FFT FPS 12 comes back as 12");
    check(sameRange(CDS::dbmRange(a, 0, true), -122.19f, -2.19f),
          "the dBm range -122.19..-2.19 comes back exactly");

    CDS::saveFftFps(a, 1, true, 30);
    check(CDS::fftFps(a, 1, true) == 30 && CDS::fftFps(a, 0, true) == 12,
          "pan slots are separate");
    check(!CDS::dbmRange(a, 1, true),
          "a slot that stored one field has not stored the other");
    check(!CDS::fftFps(b, 0, true) && !CDS::dbmRange(b, 0, true),
          "radio identities are separate");
    check(!CDS::fftFps(a, -1, true), "a negative pan slot reads nothing");

    // The boundaries of the Display panel's sliders are themselves storable.
    CDS::saveFftFps(b, 2, true, CDS::kFftFpsMin);
    check(CDS::fftFps(b, 2, true) == CDS::kFftFpsMin, "the slider minimum round-trips");
    CDS::saveFftFps(b, 2, true, CDS::kFftFpsMax);
    check(CDS::fftFps(b, 2, true) == CDS::kFftFpsMax, "the slider maximum round-trips");
    check(b.removeFeature(feature), "fixture row removed");

    // 2. A radio that owns its display state: no write, no restore.
    CDS::saveFftFps(flex, 0, false, 12);
    CDS::saveDbmRange(flex, 0, false, -122.19f, -2.19f);
    check(flex.featureExact(feature).isEmpty(),
          "a radio-owned FFT FPS and dBm range are never saved");
    check(!CDS::fftFps(a, 0, false) && !CDS::dbmRange(a, 0, false),
          "and never restored, even from a document that holds them");
    CDS::saveFftFps(unknown, 0, true, 12);
    CDS::saveDbmRange(unknown, 0, true, -122.19f, -2.19f);
    check(unknown.featureExact(feature).isEmpty(),
          "an unknown identity never writes the family default");

    // 3. Who owns the dBm range.
    check(CDS::clientOwnsDbmRange(true, true), "shaped locally + absolute bins: client");
    check(!CDS::clientOwnsDbmRange(true, false),
          "shaped locally, bins not absolute: the backend publishes the range");
    check(!CDS::clientOwnsDbmRange(false, true), "not shaped locally: the radio");
    check(!CDS::clientOwnsDbmRange(false, false), "neither: the radio");
    {
        // shapedLocally is `backend present and not a Flex` in RadioModel; the
        // second term is read off the real declarations.
        hl2::Hl2Backend hl2Backend;
        check(CDS::clientOwnsDbmRange(true, hl2Backend.capabilities().panBinsAbsolute()),
              "HL2: the client owns the dBm range");
        FlexBackend flexBackend;
        check(!CDS::clientOwnsDbmRange(false, flexBackend.capabilities().panBinsAbsolute()),
              "Flex: the radio owns the dBm range - unchanged");
        check(!CDS::clientOwnsDbmRange(true, flexBackend.capabilities().panBinsAbsolute()),
              "Flex: and would not even if it were shaped locally");
        icom::IcomCivBackend icomBackend;
        check(!CDS::clientOwnsDbmRange(true, icomBackend.capabilities().panBinsAbsolute()),
              "Icom: shaped locally, but its backend publishes the range");
    }

    // 4. Refused on the way in ...
    CDS::saveFftFps(a, 0, true, CDS::kFftFpsMin - 1);
    CDS::saveFftFps(a, 0, true, CDS::kFftFpsMax + 1);
    CDS::saveDbmRange(a, 0, true, -1882.0f, -1792.0f);   // the IC-9700 ratchet
    CDS::saveDbmRange(a, 0, true, -100.0f, -95.0f);      // 5 dB: below the minimum
    CDS::saveDbmRange(a, 0, true, -40.0f, -130.0f);      // inverted
    CDS::saveDbmRange(a, 0, true, std::numeric_limits<float>::quiet_NaN(), -40.0f);
    check(CDS::fftFps(a, 0, true) == 12
              && sameRange(CDS::dbmRange(a, 0, true), -122.19f, -2.19f),
          "out-of-range writes leave the stored values alone");
    // ... and on the way out: a row damaged or hand-edited behind our back.
    {
        QJsonObject doc = a.featureExact(feature);
        doc.insert(QStringLiteral("fftFps"),
                   QJsonObject{{QStringLiteral("0"), 12.5},
                               {QStringLiteral("1"), 400},
                               {QStringLiteral("2"), QStringLiteral("12")}});
        doc.insert(QStringLiteral("dbmRanges"),
                   QJsonObject{
                       {QStringLiteral("0"), QJsonObject{{QStringLiteral("min"), -1882.0},
                                                         {QStringLiteral("max"), -1792.0}}},
                       {QStringLiteral("1"), QJsonObject{{QStringLiteral("min"), -130.0}}},
                       {QStringLiteral("2"), QStringLiteral("-130..-40")}});
        check(a.setFeature(feature, 1, doc), "damaged fixture stored");
        check(!CDS::fftFps(a, 0, true), "a fractional FPS is rejected");
        check(!CDS::fftFps(a, 1, true), "an out-of-range FPS is rejected");
        check(!CDS::fftFps(a, 2, true), "a string FPS is rejected");
        check(!CDS::dbmRange(a, 0, true), "an implausible stored range is rejected");
        check(!CDS::dbmRange(a, 1, true), "a range missing its max is rejected");
        check(!CDS::dbmRange(a, 2, true), "a range that is not an object is rejected");
        check(a.removeFeature(feature), "damaged fixture removed");
    }

    // 5. A newer schema is not ours to read or replace.
    {
        const QJsonObject future{
            {QStringLiteral("fftFps"), QJsonObject{{QStringLiteral("0"), 30}}}};
        check(b.setFeature(feature, 2, future), "future fixture stored");
        CDS::saveFftFps(b, 0, true, 12);
        CDS::saveDbmRange(b, 0, true, -122.19f, -2.19f);
        check(!CDS::fftFps(b, 0, true), "future schema not interpreted");
        check(b.featureExact(feature) == future, "future schema not overwritten");
        check(b.removeFeature(feature), "future fixture removed");
    }

    // 6. One document, several tables, no schema bump.
    {
        // The waterfall rate plus a table this build does not read: neither
        // writer may drop the other's rows.
        const QJsonObject foreignAverages{
            {QStringLiteral("0"), QJsonObject{{QStringLiteral("average"), 40},
                                              {QStringLiteral("weighted"), true}}}};
        const QJsonObject older{
            {QStringLiteral("waterfallRates"), QJsonObject{{QStringLiteral("0"), 60}}},
            {QStringLiteral("fftAverages"), foreignAverages}};
        check(b.setFeature(feature, 1, older), "older-build fixture stored");
        CDS::saveFftFps(b, 0, true, 12);
        CDS::saveDbmRange(b, 0, true, -122.19f, -2.19f);
        check(CDS::waterfallRate(b, 0, true) == 60,
              "the waterfall rate survives the new tables being written");
        check(b.featureExact(feature).value(QStringLiteral("fftAverages")).toObject()
                  == foreignAverages,
              "a table this build does not know survives too, untouched");
        // And the other way: the waterfall writer, unchanged, keeps ours.
        CDS::saveWaterfallRate(b, 0, true, 73);
        check(CDS::waterfallRate(b, 0, true) == 73, "waterfall rate rewritten");
        check(CDS::fftFps(b, 0, true) == 12
                  && sameRange(CDS::dbmRange(b, 0, true), -122.19f, -2.19f),
              "the new tables survive the waterfall rate being written");
        int version = 0;
        b.featureExact(feature, &version);
        check(version == 1, "the document is still schema 1");
    }

    // 7. Two fields of one pan inside one deferral window.
    {
        const RadioSettingsScope c(QStringLiteral("hl2"), QStringLiteral("C"));
        const QString rateKey = CDS::pendingWriteKey(c, 0, "waterfallRate");
        const QString fpsKey = CDS::pendingWriteKey(c, 0, "fftFps");
        check(rateKey != fpsKey, "two fields of one pan have two keys");
        check(CDS::pendingWriteKey(c, 0, "fftFps") != CDS::pendingWriteKey(c, 1, "fftFps"),
              "two pans have two keys");
        check(CDS::pendingWriteKey(c, 0, "fftFps") != CDS::pendingWriteKey(a, 0, "fftFps"),
              "two radios have two keys");
        DeferredSettingsWrites pending;
        pending.schedule(rateKey, [c] { CDS::saveWaterfallRate(c, 0, true, 60); });
        pending.schedule(fpsKey, [c] { CDS::saveFftFps(c, 0, true, 12); });
        pending.schedule(fpsKey, [c] { CDS::saveFftFps(c, 0, true, 14); });
        pending.flush();
        check(CDS::waterfallRate(c, 0, true) == 60,
              "the waterfall rate was not replaced by the FPS edit beside it");
        check(CDS::fftFps(c, 0, true) == 14,
              "and the later of two FPS edits is the one stored");
    }

    // The shared plausibility predicate, at the edges the store relies on.
    check(dbmRangeLooksPlausible(-130.0f, -40.0f), "the pan model's default is plausible");
    check(!dbmRangeLooksPlausible(-202.0f, -112.0f), "past -180 dBm is not");
    check(!dbmRangeLooksPlausible(-100.0f, -95.0f), "5 dB of range is not");

    // 8. SOURCE TEXT: call order inside MainWindow methods no test can construct.
    {
        const QString wiring = readSource("src/gui/MainWindow_Wiring.cpp");
        const QString session = readSource("src/gui/MainWindow_Session.cpp");
        check(!wiring.isEmpty() && !session.isEmpty(), "the two sources were read");

        // Restore, then seed: the request reads the widget.
        const qsizetype restoreFps = wiring.indexOf(
            QStringLiteral("sw->setFftFps(*savedFps);"));
        const qsizetype seedRates = wiring.indexOf(
            QStringLiteral("m_radioModel.requestPanDisplayRates(panId, sw->fftFps(),"));
        check(restoreFps > 0
                  && wiring.contains(QStringLiteral("ClientDisplaySettings::fftFps(")),
              "pan wiring restores FFT FPS into the widget");
        check(seedRates > 0, "pan wiring still seeds the shaper");
        check(restoreFps < seedRates, "and restores BEFORE it seeds");

        // The slider, the clone and the reset each save.
        check(wiring.count(QStringLiteral("scheduleClientFftFpsSave(")) == 4,
              "definition + slider + clone + reset call the FFT FPS save");
        check(wiring.contains(QStringLiteral(
                  "scheduleClientFftFpsSave(sw->panIndex(), v);")),
              "the FPS slider saves the operator's value");
        check(wiring.indexOf(QStringLiteral("scheduleClientFftFpsSave(sw->panIndex(), v);"))
                  < wiring.indexOf(QStringLiteral("if (m_adaptiveThrottleActive)\n            return;")),
              "before the adaptive-throttle return, so the cap cannot drop it");

        // The dBm range: both operator gestures adopt, both primes restore.
        check(wiring.count(QStringLiteral("adoptClientOwnedDbmRange(applet->panId()")) == 3,
              "scale request, drag (no echo) and drag (echo) adopt the range");
        const qsizetype restoreRange = wiring.indexOf(
            QStringLiteral("restoreClientOwnedDbmRange(pan, sw->panIndex());"));
        check(restoreRange > 0
                  && wiring.indexOf(QStringLiteral(
                         "sw->setDbmRange(pan->minDbm(), pan->maxDbm());"),
                         restoreRange) - restoreRange < 120,
              "pan wiring restores the range into the model, then primes from it");
        const qsizetype sessionRestore = session.indexOf(
            QStringLiteral("restoreClientOwnedDbmRange(pan, sw->panIndex());"));
        check(sessionRestore > 0
                  && session.indexOf(QStringLiteral(
                         "sw->setDbmRange(pan->minDbm(), pan->maxDbm());"),
                         sessionRestore) - sessionRestore < 120,
              "the reconnect path does the same");
        check(wiring.contains(QStringLiteral(
                  "ClientDisplaySettings::clientOwnsDbmRange(")),
              "ownership of the range is the shared predicate");
    }

    if (g_failed) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failed);
    }
    return g_failed ? 1 : 0;
}
