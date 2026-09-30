// The HL2 `Hardware` document, where it meets the settings store.
//
// WHY A SECOND TARGET AND NOT MORE OF hl2_hardware_options_test. That one is
// pure policy — no Qt, no store — and it stays that way. Everything here needs
// an AppSettings database on disk, and the three properties it pins are
// properties of the PERSISTENCE, not of the policy:
//
//   1. Every field survives a save/load round trip, including the two enums
//      whose zero value is meaningful, and clamping happens on the way in.
//   2. A field this build does not know about SURVIVES a write from this build
//      (Principle XIV — the document is persisted as a unit). save() reads with
//      featureExact() and read-modify-writes for exactly this reason, and
//      nothing else would notice if it stopped.
//   3. AN EMPTY radio_id IS NEVER THE TARGET OF A WRITE that came from a radio
//      whose identity is not yet known. RadioSettingsScope::isValid() is NOT
//      that guard — it only requires a non-empty FAMILY — so the empty-serial
//      case is checked here, on the store, rather than trusted to the one
//      `if` in Hl2Backend::applyHardwareOptions().
//   4. An older build must not replace a document with a newer schema.
//
// Socket-free and radio-free: nothing here binds, discovers or connects.
// (aethersdr/AetherSDR#5867 review follow-up.)

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/RadioSettingsScope.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/Hl2HardwareOptions.h"

#include <QCoreApplication>
#include <QJsonObject>
#include <QVariantMap>
#include <QLatin1String>
#include <QString>

#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    std::fprintf(stderr, "%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok)
        ++g_failures;
}

namespace {

Hl2HardwareOptions everyFieldMoved()
{
    Hl2HardwareOptions o;
    o.codec               = Hl2HardwareOptions::Codec::SquareSdr2;
    o.ditherBit           = true;
    o.randomBit           = true;
    o.filterBoard         = Hl2HardwareOptions::FilterBoard::None;   // NOT the default
    o.n2adrHpf            = true;
    o.atuGateware         = true;
    o.speakerLevelPercent = 37;
    return o;
}

}  // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("aether-hl2-hardware-document"));
    if (!profile.isValid())
        return 1;
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();

    const QString family = QStringLiteral("hl2");
    const QString serialA = QStringLiteral("AA:BB:CC:DD:EE:01");
    const QString serialB = QStringLiteral("AA:BB:CC:DD:EE:02");

    // ---- 1. round trip, every field, both enums off their defaults ---------
    {
        const RadioSettingsScope scope(family, serialA);
        const Hl2HardwareOptions written = everyFieldMoved();
        Hl2HardwareOptions::save(scope, written);
        const Hl2HardwareOptions read = Hl2HardwareOptions::load(scope);
        check(read == written, "every field survives a save/load round trip");
        // Spelled out as well as compared, so a failure names the field rather
        // than only saying the structs differ.
        check(read.codec == Hl2HardwareOptions::Codec::SquareSdr2, "codec round trips");
        check(read.filterBoard == Hl2HardwareOptions::FilterBoard::None,
              "filterBoard round trips as None, which is a MEANINGFUL zero and "
              "not the field's default");
        check(read.ditherBit && read.randomBit && read.n2adrHpf && read.atuGateware,
              "all four bools round trip");
        check(read.speakerLevelPercent == 37, "the speaker level round trips");
    }

    // ---- 2. an out-of-range level is clamped, not stored and re-read raw ---
    {
        const RadioSettingsScope scope(family, serialB);
        Hl2HardwareOptions o;
        o.speakerLevelPercent = 4000;
        Hl2HardwareOptions::save(scope, o);
        check(Hl2HardwareOptions::load(scope).speakerLevelPercent == 100,
              "a level above the range is clamped on the way into the store");
        o.speakerLevelPercent = -7;
        Hl2HardwareOptions::save(scope, o);
        check(Hl2HardwareOptions::load(scope).speakerLevelPercent == 0,
              "and a negative one is clamped too");
    }

    // ---- 3. a field this build does not know about survives our write -----
    //
    // The defect this catches is silent and one-directional: an older build
    // writing the document would DROP a newer build's field, and the operator
    // would only find out by going back to the newer build and finding the
    // setting gone. save() read-modify-writes with featureExact() to prevent
    // it; nothing else in the tree would notice if it started using feature().
    {
        const RadioSettingsScope scope(family, serialA);
        QJsonObject doc = scope.featureExact(QLatin1String(Hl2HardwareOptions::kFeature));
        check(!doc.isEmpty(), "section 1's document is still there to extend");
        doc[QStringLiteral("aFieldFromTheFuture")] = 1234;
        check(scope.setFeature(QLatin1String(Hl2HardwareOptions::kFeature),
                               Hl2HardwareOptions::kSchemaVersion, doc),
              "the unknown field is written alongside the known ones");

        Hl2HardwareOptions o = Hl2HardwareOptions::load(scope);
        o.speakerLevelPercent = 11;
        Hl2HardwareOptions::save(scope, o);

        const QJsonObject after =
            scope.featureExact(QLatin1String(Hl2HardwareOptions::kFeature));
        check(after.value(QStringLiteral("aFieldFromTheFuture")).toInt() == 1234,
              "a field this build does not know about survives a write from this build");
        check(after.value(QStringLiteral("speakerLevelPercent")).toInt() == 11,
              "and our own change landed in the same document");
    }

    // ---- 3b. a newer document remains read-only to this build ------------
    {
        const RadioSettingsScope scope(family, serialA);
        QJsonObject future = scope.featureExact(QLatin1String(Hl2HardwareOptions::kFeature));
        future[QStringLiteral("speakerLevelPercent")] = 73;
        const int futureVersion = Hl2HardwareOptions::kSchemaVersion + 1;
        check(scope.setFeature(QLatin1String(Hl2HardwareOptions::kFeature),
                               futureVersion, future),
              "a newer Hardware schema is stored for the downgrade check");

        Hl2HardwareOptions changed = everyFieldMoved();
        changed.speakerLevelPercent = 12;
        Hl2HardwareOptions::save(scope, changed);

        int storedVersion = 0;
        const QJsonObject after = scope.featureExact(
            QLatin1String(Hl2HardwareOptions::kFeature), &storedVersion);
        check(storedVersion == futureVersion && after == future,
              "an older build does not downgrade or change a newer Hardware document");
    }

    // ---- 4. the family-wide row is not written by accident ----------------
    //
    // An empty radioId reads and writes the family's DEFAULT row, which every
    // HL2 without a row of its own inherits. One operator's codec choice
    // reaching a second radio is exactly the failure this document exists to
    // prevent, so the empty-serial case is pinned rather than assumed.
    {
        const RadioSettingsScope empty(family, QString{});
        check(empty.isValid(),
              "an empty serial still passes isValid() — which is WHY it cannot be "
              "the guard");
        check(empty.featureExact(QLatin1String(Hl2HardwareOptions::kFeature)).isEmpty(),
              "no family-wide Hardware row exists after three per-radio writes");

        // And if one ever were written, a second radio would inherit it: the
        // read falls back exact -> family-wide. Pinned so that the guard in
        // Hl2Backend::applyHardwareOptions() has a stated consequence.
        const RadioSettingsScope fresh(family, QStringLiteral("AA:BB:CC:DD:EE:FF"));
        const Hl2HardwareOptions unconfigured = Hl2HardwareOptions::load(fresh);
        check(unconfigured == Hl2HardwareOptions{},
              "a radio that has never been configured gets the bare-board defaults, "
              "not another radio's declaration");
        check(unconfigured.filterBoard == Hl2HardwareOptions::FilterBoard::N2adrRxTx,
              "including the one default that is not the bare board — the N2ADR "
              "pattern this backend has always driven");
    }

    // ---- 4b. THE GUARD ITSELF, through the backend -------------------------
    //
    // Section 4 above shows that no family-wide row exists after three writes
    // that all carried a serial — which is a property of those writes, not of
    // the guard. @on8st deleted `if (m_radioSerial.isEmpty())` from
    // Hl2Backend::applyHardwareOptions() and this file still reported "all
    // checks passed" (#5867 review). So the guard is now driven, on the path
    // that actually reaches it: `hw.set` arrives through invokeExtension, and
    // backendDeclaresExtension() gates on the NAMESPACE, not on whether a radio
    // is attached — so a default-constructed backend answers it with
    // m_radioSerial still empty. That is the case I found in the original diff
    // and could not previously pin.
    //
    // Socket-free: nothing is connected, nothing binds, no peer.
    {
        const RadioSettingsScope familyWide(family, QString{});
        check(familyWide.featureExact(QLatin1String(Hl2HardwareOptions::kFeature)).isEmpty(),
              "no family-wide Hardware row before the unattached hw.set");

        hl2::Hl2Backend backend;      // never connected: m_radioSerial is empty
        backend.invokeExtension(QStringLiteral("hl2"), QStringLiteral("hw.set"), 0,
                                QVariantMap{
                                    {QStringLiteral("codec"), 2},
                                    {QStringLiteral("atuGateware"), true},
                                });

        check(familyWide.featureExact(QLatin1String(Hl2HardwareOptions::kFeature)).isEmpty(),
              "hw.set before any connect writes NO family-wide row — every HL2 "
              "without a row of its own would otherwise inherit it");

        // And the refusal is a refusal to PERSIST, not to apply: the session
        // still honours what the caller asked for. A reply proves the verb ran
        // rather than being dropped, which is what would make the check above
        // pass for the wrong reason.
        int reportedCodec = -1;
        QObject::connect(&backend, &IRadioBackend::extensionResult, &backend,
                         [&reportedCodec](quint64, const QVariant& r) {
            const QVariantMap m = r.toMap();
            if (m.contains(QStringLiteral("codec")))
                reportedCodec = m.value(QStringLiteral("codec")).toInt();
        });
        backend.invokeExtension(QStringLiteral("hl2"), QStringLiteral("hw.get"), 7, {});
        check(reportedCodec == 2,
              "and the verb did run — the session holds the declaration it was "
              "given, it simply does not write it anywhere");
    }

    // ---- 4c. THE SEED RULE'S CALL SITE, not just the rule ------------------
    //
    // testCodecChangeSeed() in hl2_hardware_options_test pins
    // ditherBitOnCodecChange() thoroughly. It does not pin the ONE place that
    // applies it — the `if (next.codec != m_hw.codec)` block in hw.set —
    // and @on8st deleted that block with all three targets staying green
    // (#5867 review). That is the same shape as the defect itself, one layer
    // down: the policy tested, its application not.
    //
    // THE ROUTE MATTERS, and this is the part I would have got wrong.
    // AK4951 -> None does NOT catch a deleted seed block, because without the
    // seed the AK4951 never turns the bit on in the first place, so None has
    // nothing to clear. Only a board that CARRIES the operator's speaker value
    // across — the SquareSDR 2 — and then a move to None shows the regression.
    // @on8st established that by running both halves under the mutation.
    {
        hl2::Hl2Backend backend;
        int wire = -1;
        QObject::connect(&backend, &IRadioBackend::extensionResult, &backend,
                         [&wire](quint64, const QVariant& r) {
            const QVariantMap m = r.toMap();
            if (m.contains(QStringLiteral("ditherBitOnWire")))
                wire = m.value(QStringLiteral("ditherBitOnWire")).toBool() ? 1 : 0;
        });
        const auto wireAfter = [&backend, &wire](const QVariantMap& set) {
            wire = -1;
            backend.invokeExtension(QStringLiteral("hl2"), QStringLiteral("hw.set"), 0, set);
            backend.invokeExtension(QStringLiteral("hl2"), QStringLiteral("hw.get"), 9, {});
            return wire;
        };

        // Declare the SquareSDR 2 and turn its loudspeaker on — a perfectly
        // ordinary thing for that board's owner to do.
        check(wireAfter(QVariantMap{{QStringLiteral("codec"), 2},
                                    {QStringLiteral("ditherBit"), true}}) == 1,
              "SquareSDR 2 with its loudspeaker on: the bit is high");

        // Now correct the declaration to a bare Hermes-Lite 2. On that board the
        // same bit is the band-voltage output on the CL2 jack, and it must not
        // arrive switched on because of a speaker setting made for another board.
        check(wireAfter(QVariantMap{{QStringLiteral("codec"), 0}}) == 0,
              "correcting SquareSDR 2 -> None leaves the band-voltage output OFF "
              "— the seed rule is APPLIED, not merely defined");

        // The AK4951 route as well, which is the one an operator is likeliest to
        // walk, even though it cannot catch a missing seed on its own.
        check(wireAfter(QVariantMap{{QStringLiteral("codec"), 1}}) == 1,
              "AK4951 seeds its speaker on through hw.set");
        check(wireAfter(QVariantMap{{QStringLiteral("codec"), 0}}) == 0,
              "and correcting AK4951 -> None leaves band volts off too");

        // AND AN EXPLICIT BIT STILL WINS, which is the documented exception:
        // a caller naming both is declaring a board and its speaker together.
        check(wireAfter(QVariantMap{{QStringLiteral("codec"), 0},
                                    {QStringLiteral("ditherBit"), true}}) == 1,
              "a caller that states ditherBit alongside the codec overrides the "
              "seed — by design, and only reachable from the bridge");
    }

    // ---- 5. an invalid scope is inert in both directions -------------------
    {
        const RadioSettingsScope none;
        check(!none.isValid(), "a default-constructed scope is invalid");
        check(Hl2HardwareOptions::load(none) == Hl2HardwareOptions{},
              "loading through an invalid scope yields the defaults, not an error state");
        Hl2HardwareOptions::save(none, everyFieldMoved());   // must not crash or write
        check(Hl2HardwareOptions::load(none) == Hl2HardwareOptions{},
              "and saving through it wrote nothing");
    }

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_hardware_document_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
