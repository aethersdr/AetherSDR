// HL2 state restore/capture (RFC #4603 PR 3) — the parts provable without
// hardware: the band-key table, applyRestoredState's validation boundary
// (Principle VII), the restored-rate/LNA seeding through connectRadio, and
// the capture snapshot (currentOperatingState) round-trip including per-band
// maps. The live link paths (pushInitialState's #4484 reconciliation, band
// hops applying remembered drive on a keyed-up radio) are the bench half —
// validated on real HL2 + Radioberry hardware (nigelfenton, PR #4614 thread).
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/Hl2Bands.h"

#include <QCoreApplication>
#include <QJsonObject>

#include <iostream>

using namespace AetherSDR;

namespace {

int g_failures = 0;

void check(bool condition, const char* label)
{
    std::cout << (condition ? "[ OK ] " : "[FAIL] ") << label << '\n';
    if (!condition) {
        ++g_failures;
    }
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("aether-hl2-state-restore-test"));
    if (!profile.isValid()) {
        std::cerr << "[FAIL] create temporary home\n";
        return 1;
    }
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();

    // ---- the band-key table is total and stable ---------------------------
    check(hl2::bandKeyForHz(1'840'000.0) == QStringLiteral("160m"),
          "1.84 MHz maps to 160m");
    check(hl2::bandKeyForHz(3'573'000.0) == QStringLiteral("80m"),
          "3.573 MHz maps to 80m");
    check(hl2::bandKeyForHz(7'074'000.0) == QStringLiteral("40m"),
          "7.074 MHz maps to 40m");
    check(hl2::bandKeyForHz(14'074'000.0) == QStringLiteral("20m"),
          "14.074 MHz maps to 20m");
    check(hl2::bandKeyForHz(28'074'000.0) == QStringLiteral("10m"),
          "28.074 MHz maps to 10m");
    check(hl2::bandKeyForHz(5'000'000.0) == QStringLiteral("60m"),
          "an in-gap frequency lands in its neighborhood's bucket (WWV -> 60m)");
    check(!hl2::bandKeyForHz(100'000.0).isEmpty()
              && !hl2::bandKeyForHz(38'400'000.0).isEmpty(),
          "both extremes of the HL2 tuning range map to a key");

    // ---- applyRestoredState is a validation boundary ----------------------
    {
        hl2::Hl2Backend backend;
        RestoredRadioState bogus;
        bogus.rfFrequencyHz = 99'000'000.0;             // outside 0.1..38.4 MHz
        bogus.mode = QStringLiteral("NOT-A-REAL-MODE-STRING");  // too long
        bogus.filterLowHz = 5'000.0;                    // low >= high
        bogus.filterHighHz = 100.0;
        bogus.sampleRateHz = 12'345;                    // snapped, not rejected
        bogus.extensionSchemaVersion = 1;
        bogus.extension = QJsonObject{
            {QStringLiteral("rfGain"),
             QJsonObject{{QStringLiteral("defaultDb"), 999},
                         {QStringLiteral("lnaDbByBand"),
                          QJsonObject{{QStringLiteral("40m"), -999}}}}},
            {QStringLiteral("txSetpoints"),
             QJsonObject{{QStringLiteral("defaultPercent"), 500},
                         {QStringLiteral("driveByBand"),
                          QJsonObject{{QStringLiteral("40m"), -5}}}}}};
        backend.applyRestoredState(bogus);

        const RestoredRadioState snapshot = backend.currentOperatingState();
        // The bogus frequency was dropped, so the default stands; the maps
        // were clamped to hardware limits rather than trusted.
        check(snapshot.rfFrequencyHz != 99'000'000.0,
              "an out-of-range restored frequency is dropped");
        const QJsonObject rfGain =
            snapshot.extension.value(QStringLiteral("rfGain")).toObject();
        check(rfGain.value(QStringLiteral("defaultDb")).toInt() <= 48,
              "a restored LNA default clamps to the AD9866's range");
        check(rfGain.value(QStringLiteral("lnaDbByBand"))
                      .toObject()
                      .value(QStringLiteral("40m"))
                      .toInt()
                  >= -12,
              "a restored per-band LNA clamps to the AD9866's range");
        const QJsonObject tx =
            snapshot.extension.value(QStringLiteral("txSetpoints")).toObject();
        check(tx.value(QStringLiteral("defaultPercent")).toInt() <= 100,
              "a restored drive default clamps to 0..100");
        check(tx.value(QStringLiteral("driveByBand"))
                      .toObject()
                      .value(QStringLiteral("40m"))
                      .toInt()
                  >= 0,
              "a restored per-band drive clamps to 0..100");
    }

    // ---- restored state seeds the session at connect ----------------------
    {
        hl2::Hl2Backend backend;
        RestoredRadioState remembered;
        remembered.rfFrequencyHz = 14'074'000.0;
        remembered.mode = QStringLiteral("USB");
        remembered.sampleRateHz = 192'000;
        remembered.extensionSchemaVersion = 1;
        remembered.extension = QJsonObject{
            {QStringLiteral("rfGain"),
             QJsonObject{{QStringLiteral("defaultDb"), 12},
                         {QStringLiteral("lnaDbByBand"),
                          QJsonObject{{QStringLiteral("20m"), 6}}}}},
            {QStringLiteral("txSetpoints"),
             QJsonObject{{QStringLiteral("driveByBand"),
                          QJsonObject{{QStringLiteral("20m"), 35}}}}}};
        backend.applyRestoredState(remembered);

        // Unroutable target: connectRadio() seeds every pre-link member
        // synchronously before any network I/O succeeds.
        RadioConnectRequest req;
        req.host = QStringLiteral("192.0.2.1");   // TEST-NET-1, never routable
        req.port = 1024;
        req.serial = QStringLiteral("AA:BB:CC:DD:EE:FF");
        backend.connectRadio(req);

        const RestoredRadioState snapshot = backend.currentOperatingState();
        check(snapshot.sampleRateHz == 192'000,
              "the restored sample rate seeds the session");
        check(snapshot.rfFrequencyHz == 14'074'000.0,
              "the restored frequency seeds the session");
        // The 20m band's remembered LNA (6 dB) is the session's live gain —
        // visible as the current band's entry in the capture snapshot.
        check(snapshot.extension.value(QStringLiteral("rfGain"))
                      .toObject()
                      .value(QStringLiteral("lnaDbByBand"))
                      .toObject()
                      .value(QStringLiteral("20m"))
                      .toInt()
                  == 6,
              "the start band's remembered LNA is applied at connect");
        backend.disconnectRadio();
    }

    // ---- an explicit param still beats restored state ---------------------
    {
        hl2::Hl2Backend backend;
        RestoredRadioState remembered;
        remembered.sampleRateHz = 192'000;
        backend.applyRestoredState(remembered);

        RadioConnectRequest req;
        req.host = QStringLiteral("192.0.2.1");
        req.port = 1024;
        req.serial = QStringLiteral("AA:BB:CC:DD:EE:FF");
        req.params.insert(QStringLiteral("sampleRateHz"), 48'000);
        backend.connectRadio(req);
        check(backend.currentOperatingState().sampleRateHz == 48'000,
              "an explicit automation/test param outranks restored state");
        backend.disconnectRadio();
    }

    return g_failures == 0 ? 0 : 1;
}
