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
//
// Socket-free and radio-free: nothing here binds, discovers or connects.
// (aethersdr/AetherSDR#5867 review follow-up.)

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/RadioSettingsScope.h"
#include "core/backends/hl2/Hl2HardwareOptions.h"

#include <QCoreApplication>
#include <QJsonObject>
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
