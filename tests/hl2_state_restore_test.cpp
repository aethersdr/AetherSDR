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
        bogus.agcMode = QStringLiteral("medium");       // not the vocabulary
        bogus.agcThreshold = 4'000;                   // outside 0..100
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
        // AGC: DROPPED, not clamped or aliased. "medium" is close enough to
        // the real "med" that wdspAgcMode() would silently accept it as its
        // fallback — the whole reason isKnownAgcModeString() exists — and a
        // clamped 4000 would become an AGC-T of 100 nobody chose.
        check(snapshot.agcMode == QStringLiteral("med"),
              "an AGC mode outside the vocabulary is dropped, not aliased");
        check(snapshot.agcThreshold == 65,
              "an out-of-range AGC threshold is dropped, not clamped");
    }

    // ---- the AGC pair restores, independently ------------------------------
    // #4909: the operator's AGC lived only in the WDSP channel, so every
    // launch reopened it on med/65 and the setting was gone.
    {
        hl2::Hl2Backend backend;
        RestoredRadioState remembered;
        remembered.agcMode = QStringLiteral("slow");
        remembered.agcThreshold = 40;
        backend.applyRestoredState(remembered);
        RadioConnectRequest req;
        req.host = QStringLiteral("192.0.2.1");   // TEST-NET-1, never routable
        req.port = 1024;
        req.serial = QStringLiteral("AA:BB:CC:DD:EE:FF");
        backend.connectRadio(req);

        const RestoredRadioState snap = backend.currentOperatingState();
        check(snap.agcMode == QStringLiteral("slow") && snap.agcThreshold == 40,
              "a remembered AGC pair seeds the session (#4909)");
        backend.disconnectRadio();
    }
    {
        // Half a document: the threshold alone applies against the default
        // mode, matching the independent validation in applyRestoredState().
        hl2::Hl2Backend backend;
        RestoredRadioState thresholdOnly;
        thresholdOnly.agcThreshold = 0;   // the sentinel's whole point
        backend.applyRestoredState(thresholdOnly);
        RadioConnectRequest req;
        req.host = QStringLiteral("192.0.2.1");
        req.port = 1024;
        req.serial = QStringLiteral("AA:BB:CC:DD:EE:FF");
        backend.connectRadio(req);

        const RestoredRadioState snap = backend.currentOperatingState();
        check(snap.agcThreshold == 0,
              "a remembered AGC threshold of 0 is restored, not read as absent");
        check(snap.agcMode == QStringLiteral("med"),
              "the AGC mode keeps its default when the document has none");
        backend.disconnectRadio();
    }
    {
        // The operator's own change is CAPTURED — the half of the bug that
        // made the document empty in the first place.
        hl2::Hl2Backend backend;
        backend.applyRestoredState(RestoredRadioState{});
        RadioConnectRequest req;
        req.host = QStringLiteral("192.0.2.1");
        req.port = 1024;
        req.serial = QStringLiteral("AA:BB:CC:DD:EE:FF");
        backend.connectRadio(req);
        backend.setSliceAgc(0, QStringLiteral("fast"), 25);

        const RestoredRadioState snap = backend.currentOperatingState();
        check(snap.agcMode == QStringLiteral("fast") && snap.agcThreshold == 25,
              "an operator AGC change reaches the capture snapshot");
        backend.disconnectRadio();
    }
    {
        // A radio swap must not carry the previous radio's AGC: an empty
        // restore is a full reset, and buildReceivers() deliberately keeps
        // receiver state across the rebuild.
        hl2::Hl2Backend backend;
        RadioConnectRequest req;
        req.host = QStringLiteral("192.0.2.1");
        req.port = 1024;
        req.serial = QStringLiteral("AA:BB:CC:DD:EE:FF");

        RestoredRadioState radioA;
        radioA.agcMode = QStringLiteral("fast");
        radioA.agcThreshold = 12;
        backend.applyRestoredState(radioA);
        backend.connectRadio(req);
        backend.disconnectRadio();

        backend.applyRestoredState(RestoredRadioState{});   // radio B: no memory
        backend.connectRadio(req);
        const RestoredRadioState snap = backend.currentOperatingState();
        check(snap.agcMode == QStringLiteral("med") && snap.agcThreshold == 65,
              "a memoryless radio comes up on the defaults, never the previous "
              "radio's AGC");
        backend.disconnectRadio();
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

    // ---- unvisited band: baseline default, never inheritance --------------
    // (PR #4619 review, bot + Ozy311): set 40 on 20m (bootstraps the
    // baseline), raise 20m to 90, hop to never-visited 40m — the drive must
    // be the 40 baseline, NOT the 90 that happened to be live.
    {
        hl2::Hl2Backend backend;
        backend.applyRestoredState(RestoredRadioState{});   // no memory
        RadioConnectRequest req;
        req.host = QStringLiteral("192.0.2.1");
        req.port = 1024;
        req.serial = QStringLiteral("AA:BB:CC:DD:EE:FF");
        backend.connectRadio(req);

        backend.setSliceFrequency(0, 14'074'000.0);   // 20m
        backend.setTxPower(40);                       // bootstraps baseline 40
        backend.setTxPower(90);                       // 20m's own value
        backend.setSliceFrequency(0, 7'074'000.0);    // hop to unvisited 40m

        const RestoredRadioState snap = backend.currentOperatingState();
        const QJsonObject drive =
            snap.extension.value(QStringLiteral("txSetpoints"))
                .toObject()
                .value(QStringLiteral("driveByBand"))
                .toObject();
        check(drive.value(QStringLiteral("40m")).toInt() == 40,
              "an unvisited band gets the operator's baseline, not the "
              "previous band's live drive");
        check(drive.value(QStringLiteral("20m")).toInt() == 90,
              "the previous band keeps its own remembered drive");
        backend.disconnectRadio();
    }

    // ---- a truly baseline-less first hop is conservative ------------------
    {
        hl2::Hl2Backend backend;
        backend.applyRestoredState(RestoredRadioState{});
        RadioConnectRequest req;
        req.host = QStringLiteral("192.0.2.1");
        req.port = 1024;
        req.serial = QStringLiteral("AA:BB:CC:DD:EE:FF");
        backend.connectRadio(req);
        // No setTxPower yet — no baseline. Hop off the start band.
        backend.setSliceFrequency(0, 7'074'000.0);
        const QJsonObject drive =
            backend.currentOperatingState()
                .extension.value(QStringLiteral("txSetpoints"))
                .toObject()
                .value(QStringLiteral("driveByBand"))
                .toObject();
        check(drive.value(QStringLiteral("40m")).toInt() == 0,
              "with no baseline at all, a first band visit sets drive 0 — "
              "conservative once, never hot by inheritance");
        backend.disconnectRadio();
    }

    // ---- radio swap: an empty restore is a full reset ---------------------
    // (PR #4619 review, Ozy311 finding 1): same backend instance, radio A
    // with maps, then applyRestoredState({}) for radio B — nothing of A may
    // survive, including the LIVE members.
    {
        hl2::Hl2Backend backend;
        RestoredRadioState radioA;
        radioA.rfFrequencyHz = 7'074'000.0;
        radioA.sampleRateHz = 192'000;
        radioA.extensionSchemaVersion = 1;
        radioA.extension = QJsonObject{
            {QStringLiteral("rfGain"),
             QJsonObject{{QStringLiteral("defaultDb"), 6},
                         {QStringLiteral("lnaDbByBand"),
                          QJsonObject{{QStringLiteral("40m"), 3}}}}},
            {QStringLiteral("txSetpoints"),
             QJsonObject{{QStringLiteral("defaultPercent"), 25},
                         {QStringLiteral("driveByBand"),
                          QJsonObject{{QStringLiteral("40m"), 25}}}}}};
        backend.applyRestoredState(radioA);

        backend.applyRestoredState(RestoredRadioState{});   // radio B: no memory
        const RestoredRadioState snap = backend.currentOperatingState();
        const QJsonObject rfGain =
            snap.extension.value(QStringLiteral("rfGain")).toObject();
        check(rfGain.value(QStringLiteral("lnaDbByBand"))
                      .toObject()
                      .isEmpty(),
              "radio A's per-band LNA map does not survive the swap");
        check(rfGain.value(QStringLiteral("defaultDb")).toInt() == 20,
              "the LNA default resets to the virgin construction value");
        check(!snap.extension.value(QStringLiteral("txSetpoints"))
                       .toObject()
                       .contains(QStringLiteral("defaultPercent")),
              "radio A's drive baseline does not survive the swap");
        check(snap.sampleRateHz == 48'000,
              "radio A's restored rate resets to the construction default — "
              "radio B never inherits A's span (PR #4619 review)");
    }

    // ---- a corrupt mode string is dropped at the boundary -----------------
    // (PR #4619 review, Ozy311 finding 5)
    {
        hl2::Hl2Backend backend;
        RestoredRadioState corrupt;
        corrupt.mode = QStringLiteral("QRM");   // <= 8 chars, but not a mode
        backend.applyRestoredState(corrupt);
        check(backend.currentOperatingState().mode
                  != QStringLiteral("QRM"),
              "a plausible-length garbage mode never reaches Receiver::mode");
        RestoredRadioState genuine;
        genuine.mode = QStringLiteral("cw");    // case-insensitive, real
        backend.applyRestoredState(genuine);
        // (Applied at pushInitialState on hardware; boundary acceptance is
        // what's provable here: it survived validation into the stash.)
    }

    // ---- the connect-time power push is an echo, not an overwrite ---------
    // (PR #4619 bench, nigelfenton; Ozy311 finding 1): connectRadio seeds the
    // drive from the start band's memory and echoes it upward, so
    // RadioModel's connect push arrives value-identical — and setTxPower's
    // change-gate declines to record it. The stored map must survive.
    {
        hl2::Hl2Backend backend;
        RestoredRadioState remembered;
        remembered.rfFrequencyHz = 7'100'000.0;   // 40m
        remembered.extensionSchemaVersion = 1;
        remembered.extension = QJsonObject{
            {QStringLiteral("txSetpoints"),
             QJsonObject{{QStringLiteral("defaultPercent"), 100},
                         {QStringLiteral("driveByBand"),
                          QJsonObject{{QStringLiteral("40m"), 12}}}}}};
        backend.applyRestoredState(remembered);

        RadioConnectRequest req;
        req.host = QStringLiteral("192.0.2.1");
        req.port = 1024;
        req.serial = QStringLiteral("00:1C:C0:A2:13:DD");
        backend.connectRadio(req);

        // The seed itself: the snapshot's current-band stamp is 12, and the
        // upward echo carried it (TransmitModel gets seeded pre-push).
        check(backend.currentOperatingState()
                      .extension.value(QStringLiteral("txSetpoints"))
                      .toObject()
                      .value(QStringLiteral("driveByBand"))
                      .toObject()
                      .value(QStringLiteral("40m"))
                      .toInt()
                  == 12,
              "connect seeds the drive from the start band's memory");

        // RadioModel's push, replayed exactly: same value, operator path.
        backend.setTxPower(12);
        const QJsonObject after =
            backend.currentOperatingState()
                .extension.value(QStringLiteral("txSetpoints"))
                .toObject();
        check(after.value(QStringLiteral("driveByBand"))
                      .toObject()
                      .value(QStringLiteral("40m"))
                      .toInt()
                  == 12,
              "the value-identical connect push does not overwrite the map");
        check(after.value(QStringLiteral("defaultPercent")).toInt() == 100,
              "the echo does not re-bootstrap the baseline");

        // A REAL operator change still records.
        backend.setTxPower(25);
        check(backend.currentOperatingState()
                      .extension.value(QStringLiteral("txSetpoints"))
                      .toObject()
                      .value(QStringLiteral("driveByBand"))
                      .toObject()
                      .value(QStringLiteral("40m"))
                      .toInt()
                  == 25,
              "a genuine operator change still records into the band");
        backend.disconnectRadio();
    }

    // ---- a virgin connect echo never claims the baseline ------------------
    // (Ozy311: the model-default push at 100 made the 'deliberate 0' for
    // unvisited bands unreachable in the real app.)
    {
        hl2::Hl2Backend backend;
        backend.applyRestoredState(RestoredRadioState{});
        RadioConnectRequest req;
        req.host = QStringLiteral("192.0.2.1");
        req.port = 1024;
        req.serial = QStringLiteral("AA:BB:CC:DD:EE:FF");
        backend.connectRadio(req);
        backend.setTxPower(100);   // the model-default connect push, verbatim
        check(!backend.currentOperatingState()
                   .extension.value(QStringLiteral("txSetpoints"))
                   .toObject()
                   .contains(QStringLiteral("defaultPercent")),
              "a virgin connect's default-100 push never claims the baseline");
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

    // ---- the TX passband round-trips, and only once chosen ----------------
    //
    // The eSSB case the persistence exists for: set cuts, restart, and the
    // modulator must come back where the operator left it rather than at the
    // mode default while everything around it restores. (#4609 review)
    {
        hl2::Hl2Backend backend;
        // Nothing chosen yet: the document must NOT carry a passband, or the
        // next connect would read the mode-derived default as an operator
        // override and permanently suppress the per-mode derivation.
        const QJsonObject fresh = backend.currentOperatingState()
                                      .extension.value(QStringLiteral("txSetpoints"))
                                      .toObject();
        check(!fresh.contains(QStringLiteral("filterLowHz"))
                  && !fresh.contains(QStringLiteral("filterHighHz")),
              "an untouched TX passband is not persisted as an override");

        backend.setTxFilter(100, 4000);           // eSSB
        const QJsonObject captured = backend.currentOperatingState()
                                         .extension.value(QStringLiteral("txSetpoints"))
                                         .toObject();
        check(captured.value(QStringLiteral("filterLowHz")).toInt() == 100
                  && captured.value(QStringLiteral("filterHighHz")).toInt() == 4000,
              "an operator TX passband is captured into ext.txSetpoints");

        // Round-trip into a fresh backend, as a restart would.
        hl2::Hl2Backend restored;
        RestoredRadioState state;
        state.extensionSchemaVersion = 1;
        state.extension = QJsonObject{{QStringLiteral("txSetpoints"), captured}};
        restored.applyRestoredState(state);
        const QJsonObject back = restored.currentOperatingState()
                                     .extension.value(QStringLiteral("txSetpoints"))
                                     .toObject();
        check(back.value(QStringLiteral("filterLowHz")).toInt() == 100
                  && back.value(QStringLiteral("filterHighHz")).toInt() == 4000,
              "the restored TX passband survives into the next session");
    }

    // ---- the TX passband is a validation boundary too ---------------------
    {
        // Validated as a PAIR: a half-present or out-of-range document is
        // dropped WHOLE, leaving the mode derivation in charge, rather than
        // restoring one edge against the other's default — a passband the
        // operator never chose.
        struct Case { QJsonObject tx; const char* what; };
        const Case cases[] = {
            {QJsonObject{{QStringLiteral("filterLowHz"), 100}},
             "a TX passband missing its high edge is dropped"},
            {QJsonObject{{QStringLiteral("filterHighHz"), 4000}},
             "a TX passband missing its low edge is dropped"},
            {QJsonObject{{QStringLiteral("filterLowHz"), 4000},
                         {QStringLiteral("filterHighHz"), 100}},
             "an inverted TX passband is dropped"},
            {QJsonObject{{QStringLiteral("filterLowHz"), 100},
                         {QStringLiteral("filterHighHz"), 99000}},
             "a TX passband above the modulator's ceiling is dropped"},
            {QJsonObject{{QStringLiteral("filterLowHz"), -500},
                         {QStringLiteral("filterHighHz"), 2700}},
             "a negative TX low cut is dropped"},
        };
        for (const Case& c : cases) {
            hl2::Hl2Backend backend;
            RestoredRadioState state;
            state.extensionSchemaVersion = 1;
            state.extension = QJsonObject{{QStringLiteral("txSetpoints"), c.tx}};
            backend.applyRestoredState(state);
            const QJsonObject out = backend.currentOperatingState()
                                        .extension.value(QStringLiteral("txSetpoints"))
                                        .toObject();
            check(!out.contains(QStringLiteral("filterLowHz")), c.what);
        }
    }

    // ---- the restored passband is ANNOUNCED, not just applied --------------
    //
    // The modulator learns it from Hl2TxDsp::Config at connect, and nothing else
    // did: TransmitModel — and therefore the Phone applet's cut readout — went on
    // showing its own construction default until the operator happened to press a
    // cut button, so the number on screen disagreed with the transmitter. The
    // backend is authoritative about what it actually applied and has to say so
    // (#4609 review). Same normalized-delta echo as the per-band drive beside it.
    {
        hl2::Hl2Backend backend;
        int echoedLow = -1;
        int echoedHigh = -1;
        QObject::connect(&backend, &IRadioBackend::transmitChanged, &backend,
                         [&](const TransmitDelta& d) {
            if (d.txFilterLow)  echoedLow = *d.txFilterLow;
            if (d.txFilterHigh) echoedHigh = *d.txFilterHigh;
        });

        RestoredRadioState state;
        state.extensionSchemaVersion = 1;
        state.extension = QJsonObject{
            {QStringLiteral("txSetpoints"),
             QJsonObject{{QStringLiteral("filterLowHz"), 100},
                         {QStringLiteral("filterHighHz"), 4000}}}};
        backend.applyRestoredState(state);

        RadioConnectRequest req;
        req.host = QStringLiteral("192.0.2.1");   // TEST-NET-1, never routable
        req.port = 1024;
        req.serial = QStringLiteral("AA:BB:CC:DD:EE:FF");
        backend.connectRadio(req);

        check(echoedLow == 100 && echoedHigh == 4000,
              "the restored TX passband is echoed upward at connect");
        backend.disconnectRadio();
    }

    // ---- a same-family swap must not carry radio A's cuts onto radio B ----
    {
        hl2::Hl2Backend backend;
        backend.setTxFilter(100, 4000);
        // RadioModel calls this unconditionally on every engaged connect;
        // an empty state means "this radio has no memory".
        backend.applyRestoredState(RestoredRadioState{});
        const QJsonObject out = backend.currentOperatingState()
                                    .extension.value(QStringLiteral("txSetpoints"))
                                    .toObject();
        check(!out.contains(QStringLiteral("filterLowHz")),
              "a memoryless radio does not inherit the previous radio's TX cuts");
    }

    // ---- the flat AGC model, across more than one receiver -----------------
    //
    // Every other case here runs a single DDC, so the two halves of the flat
    // design were asserted nowhere: seedReceiverAgc() writing EVERY receiver,
    // and capture following the last receiver the operator touched rather than
    // the transmit one. A refactor that seeded only rx(m_txDdc), or that read
    // capture back off the TX receiver, passed the whole suite.
    {
        hl2::Hl2Backend backend;
        RestoredRadioState remembered;
        remembered.agcMode = QStringLiteral("slow");
        remembered.agcThreshold = 40;
        backend.applyRestoredState(remembered);

        // Capture before any operator action reports what was restored, not a
        // default — seedReceiverAgc() primes the remembered pair.
        const RestoredRadioState seeded = backend.currentOperatingState();
        check(seeded.agcMode == QStringLiteral("slow") && seeded.agcThreshold == 40,
              "a capture before any AGC change reports the restored pair");

        // The operator moves AGC on a NON-transmit slice. Capture must follow
        // the action; reading rx(m_txDdc) would report the untouched pair and
        // the change would be silently lost at the next launch.
        backend.setSliceAgc(/*sliceId=*/1, QStringLiteral("fast"), 30);
        const RestoredRadioState after = backend.currentOperatingState();
        check(after.agcMode == QStringLiteral("fast") && after.agcThreshold == 30,
              "capture follows the last AGC the operator set, not the TX receiver");

        // An unknown mode string is refused on the way in, so the capture side
        // can never store something the restore side would drop.
        backend.setSliceAgc(/*sliceId=*/1, QStringLiteral("medium"), 30);
        const RestoredRadioState bogusMode = backend.currentOperatingState();
        check(bogusMode.agcMode == QStringLiteral("fast"),
              "an unknown AGC mode string is refused rather than stored");
    }

    return g_failures == 0 ? 0 : 1;
}
