// #5594 (#5262 M1, #5554 §2.4): backends must ANNOUNCE capability revisions.
//
// The capability descriptor is what the aetherd control protocol's `welcome`
// serializes (#3849 step 3), so a backend that revises its RadioCapabilities
// without emitting IRadioBackend::capabilitiesChanged leaves every consumer —
// the desktop UI today, a protocol client tomorrow — holding whatever the
// backend happened to report at the connect edge, forever.
//
// This test pins the two backends whose descriptor genuinely moves mid-session
// and can be driven without hardware:
//
//   flex — the whole Flex capability table is derived from the model name
//          (capabilities() runs capabilitiesFor(caps.model)), and the model name
//          arrives in a `radio ...` status AFTER connect. decodeRadioStatus() is
//          the decode it lands in and is public, so it is driven directly here:
//          no socket, no fake radio, no timing.
//
//   rtl  — the opposite claim, and it is a claim rather than an omission: the
//          declaration is fixed for the life of a session, so the correct
//          behaviour is to emit NOTHING. Pinned so that a future revisable
//          field cannot be added without either an emission or a deliberate
//          edit here.
//
//   hl2  — the receiver ceiling FALLS when the operator zooms out, because the
//          EP6 link budget cannot carry four receivers at 384 kHz. Two halves
//          are pinned here: that the ceiling really does move across the spans
//          the operator can select (so the revision is real and not a story
//          about arithmetic that never changes), and that the announcement
//          guard fires exactly once per distinct ceiling (so a zoom SWEEP,
//          which crosses several rates in one drag, cannot become a republish
//          storm). Both are socket-free.
//
//          What is NOT pinned here is that Hl2Backend calls the guard at the two
//          points the rate is committed — the success path and the rollback in
//          applyPanBandwidth(). Driving that needs a connected radio, and the
//          fake-EP6 fixture that used to provide one is retired (see the
//          commented block in tests/tests.cmake and #5254). That call-site
//          placement and receiverCeiling() itself are review-verified; the
//          arithmetic below is reconstructed from its public helpers. This comment
//          says so rather than letting the file read as full coverage.

#include "TestSettingsProfile.h"
#include "core/backends/IRadioBackend.h"
#include "core/backends/flex/FlexBackend.h"
#include "core/backends/hl2/Hl2CapabilityAnnouncer.h"
#include "core/backends/hl2/MetisClient.h"
#include "core/backends/hl2/MetisProtocol.h"
#ifdef AETHER_BACKEND_RTL
#include "core/backends/rtl/RtlSdrBackend.h"
#endif

#include <QCoreApplication>
#include <QMap>
#include <QSignalSpy>
#include <QString>
#include <QVector>

#include <algorithm>
#include <cstdio>
#include <memory>

using namespace AetherSDR;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

static QMap<QString, QString> radioStatus(std::initializer_list<std::pair<const char*, const char*>> kvs)
{
    QMap<QString, QString> m;
    for (const auto& kv : kvs)
        m.insert(QString::fromLatin1(kv.first), QString::fromLatin1(kv.second));
    return m;
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("backend-capability-revision"));
    if (!profile.isValid()) {
        return 1;
    }
    QCoreApplication app(argc, argv);

    // ---- flex: the model name is the capability input, and it lands late ----
    {
        FlexBackend backend;
        QSignalSpy caps(&backend, &IRadioBackend::capabilitiesChanged);
        QSignalSpy radio(&backend, &IRadioBackend::radioChanged);

        // Mirror RadioModel's same-thread delta application and model provider.
        // Capture inside the notification: reading afterwards would miss an
        // emission moved ahead of radioChanged, which exposes the old table.
        QString model;
        QVector<RadioCapabilities> observed;
        backend.setModelProvider([&model] { return model; });
        QObject::connect(&backend, &IRadioBackend::radioChanged, &backend,
                         [&model](const RadioDelta& delta) {
            if (delta.model) {
                model = *delta.model;
            }
        });
        QObject::connect(&backend, &IRadioBackend::capabilitiesChanged, &backend,
                         [&backend, &observed] {
            observed.append(backend.capabilities());
        });

        // A status with no model key revises nothing. This is the common case on
        // a live radio — `radio ...` repeats for callsign, nickname and the
        // audio gains — and it must stay silent or a zoom-sweep-shaped storm
        // comes back as a typing-shaped one.
        backend.decodeRadioStatus(radioStatus({{"callsign", "KK7GWY"},
                                               {"nickname", "shack"}}));
        check(radio.count() == 1, "a radio status without a model still decodes");
        check(caps.count() == 0,
              "a radio status carrying no model announces no capability revision");

        // The model arrives. THIS is the revision the whole issue is about: the
        // seeded table (built before the name was known) is now wrong.
        backend.decodeRadioStatus(radioStatus({{"model", "FLEX-8600"}}));
        check(caps.count() == 1, "the first model announces exactly one revision");
        check(observed.size() == 1 && observed[0].model == QStringLiteral("FLEX-8600")
                  && observed[0].maxSlices == 4 && observed[0].hasExtendedDsp,
              "the first notification exposes the new model and derived capabilities");

        // The radio repeats the same model on an unrelated edit. Nothing about
        // the capability table changed, so nothing may be announced.
        backend.decodeRadioStatus(radioStatus({{"model", "FLEX-8600"},
                                               {"nickname", "shack-2"}}));
        check(caps.count() == 1,
              "a repeated model announces nothing (change-guarded, not presence-guarded)");

        // A genuinely different model is a genuinely different table.
        backend.decodeRadioStatus(radioStatus({{"model", "FLEX-6400"}}));
        check(caps.count() == 2, "a changed model announces again");
        check(observed.size() == 2 && observed[1].model == QStringLiteral("FLEX-6400")
                  && observed[1].maxSlices == 2 && !observed[1].hasExtendedDsp,
              "a changed-model notification exposes the revised capability table");

        // Disconnect drops the baseline: a reconnect republishes capabilities
        // from scratch, so the previous session's announcement describes nothing
        // and the same radio has to be announced again.
        backend.clearExtensionHandles();
        model.clear();
        backend.decodeRadioStatus(radioStatus({{"model", "FLEX-6400"}}));
        check(caps.count() == 3,
              "after a disconnect the same model announces again");
        check(observed.size() == 3 && observed[2].model == QStringLiteral("FLEX-6400")
                  && observed[2].maxSlices == 2 && !observed[2].hasExtendedDsp,
              "a reconnect notification exposes the new session model");
    }

    // ---- flex: a declared capacity beats the model table (#5594 item 3) ----
    //
    // The radio states max_slices / max_panadapters in its discovery packet;
    // RadioModel hands them here so the capability DESCRIPTOR agrees with what
    // the model enforces, because RadioResourceAdapter serializes it onto the
    // control protocol. (The model-side half — parsing, precedence, fallback —
    // is pinned in radio_capacity_declaration_test.)
    {
        FlexBackend backend;
        QSignalSpy caps(&backend, &IRadioBackend::capabilitiesChanged);
        QString model = QStringLiteral("FLEX-8600");
        backend.setModelProvider([&model] { return model; });

        check(backend.capabilities().maxSlices == 4
                  && backend.capabilities().maxPanadapters == 4,
              "with nothing declared the model table supplies both counts");

        // Declared values that differ from the table, and from each other —
        // pan capacity is no longer assumed equal to slice capacity.
        caps.clear();
        backend.setRadioReportedCapacity(3, 2);
        check(backend.capabilities().maxSlices == 3,
              "a declared slice capacity beats the model table");
        check(backend.capabilities().maxPanadapters == 2,
              "a declared panadapter capacity beats the model table, and is not "
              "assumed equal to the slice count");
        check(caps.count() == 1, "a declared capacity announces exactly one revision");

        // The caller republishes on every capacity-bearing edge; only a real
        // change may announce.
        backend.setRadioReportedCapacity(3, 2);
        check(caps.count() == 1, "an unchanged capacity announces nothing");

        backend.setRadioReportedCapacity(3, 4);
        check(backend.capabilities().maxPanadapters == 4
                  && backend.capabilities().maxSlices == 3,
              "one field may move without disturbing the other");
        check(caps.count() == 2, "a moved capacity announces again");

        // "The radio did not say" is 0, and must not be read as a capacity of
        // zero — that would describe a radio that can do nothing.
        backend.setRadioReportedCapacity(0, 0);
        check(backend.capabilities().maxSlices == 3
                  && backend.capabilities().maxPanadapters == 4,
              "0 means 'not declared' and leaves the last known capacity alone");
        check(caps.count() == 2, "'not declared' announces nothing");

        // A different radio on the next session must not inherit these limits.
        backend.clearExtensionHandles();
        model = QStringLiteral("FLEX-6400");
        check(backend.capabilities().maxSlices == 2
                  && backend.capabilities().maxPanadapters == 2,
              "after a disconnect the capacity falls back to the new radio's "
              "model table rather than the previous radio's declaration");
    }

    // ---- rtl: a static declaration, asserted rather than assumed ----
    //
    // Optional backend (AETHER_BACKEND_RTL): skipped, not failed, where librtlsdr
    // is absent — the same condition tests.cmake uses to gate rtl_backend_test.
#ifdef AETHER_BACKEND_RTL
    {
        AetherSDR::rtl::RtlSdrBackend backend;
        QSignalSpy caps(&backend, &IRadioBackend::capabilitiesChanged);

        // Never connected, so identity is empty and everything else is constant.
        const RadioCapabilities a = backend.capabilities();
        const RadioCapabilities b = backend.capabilities();
        check(a.family == QLatin1String("rtl"), "rtl reports its family");
        check(a.maxSlices == b.maxSlices && a.maxPanadapters == b.maxPanadapters
                  && a.canTransmit == b.canTransmit
                  && a.tuningMinHz == b.tuningMinHz && a.tuningMaxHz == b.tuningMaxHz
                  && a.sampleRatesHz == b.sampleRatesHz,
              "rtl's declaration does not vary between reads");
        check(!a.canTransmit, "rtl declares itself receive-only (Principle VI)");
        check(caps.count() == 0,
              "rtl announces no revision — its declaration is fixed per session");
    }
#else
    std::fprintf(stderr, "backend_capability_revision_test: RTL backend not built, "
                         "skipping its static-declaration case\n");
#endif

    // ---- hl2: the ceiling really moves, and the guard fires once per move ----
    {
        using namespace AetherSDR::hl2;

        // The arithmetic Hl2Backend::receiverCeiling() performs, composed from
        // the same two public functions it calls. A four-receiver board — what
        // the shipping hl2b5up_main gateware reports, and what the backend
        // assumes when discovery omits byte 0x13 — is honestly four receivers
        // at 48 kHz and three at 384 kHz.
        const auto ceilingAt = [](int boardMaxRx, int rateHz) {
            MetisClient::Params p;
            p.numRx = kMaxReceivers;
            p.boardMaxRx = boardMaxRx;
            const int board = MetisClient::effectiveNumRx(p);
            return std::min(board, maxReceiversAtRate(rateHz, board));
        };

        const int wide = ceilingAt(4, 48000);
        const int mid = ceilingAt(4, 96000);
        const int narrow = ceilingAt(4, 384000);
        check(wide == 4, "a 4-receiver board runs 4 receivers at 48 kHz");
        check(mid == wide, "96 kHz admits the same count as 48 kHz");
        check(narrow < wide,
              "384 kHz admits fewer receivers than 48 kHz — the ceiling really "
              "does move, so the revision this announces is a real one");

        // The guard. Seeded at the connect edge with what capabilities() already
        // published there, so the first zoom that does not move the ceiling
        // stays silent.
        ReceiverCeilingAnnouncer a;
        check(a.announced() == ReceiverCeilingAnnouncer::kNone,
              "a fresh announcer has nothing recorded");
        a.seed(wide);
        check(!a.shouldAnnounce(wide),
              "the span the connect edge already published announces nothing");
        check(!a.shouldAnnounce(mid),
              "a zoom that does not move the ceiling announces nothing");
        check(a.shouldAnnounce(narrow),
              "a zoom that lowers the ceiling announces");
        check(!a.shouldAnnounce(narrow),
              "re-requesting the span already running announces nothing");
        check(a.shouldAnnounce(wide),
              "zooming back in raises the ceiling and announces again — a client "
              "told 3 has to learn it may open a fourth receiver once more");

        // A whole sweep is ONE announcement per ceiling, not one per rate. This
        // is the storm the guard exists to prevent.
        ReceiverCeilingAnnouncer sweep;
        sweep.seed(wide);
        int announcements = 0;
        for (const int rate : {48000, 96000, 192000, 384000, 192000, 96000, 48000})
            if (sweep.shouldAnnounce(ceilingAt(4, rate)))
                ++announcements;
        check(announcements == 2,
              "a full zoom sweep out and back announces twice (down, then up), "
              "not once per rate");

        // Disconnect: the next session republishes from scratch, so the same
        // ceiling has to be announced again rather than suppressed.
        sweep.reset();
        check(sweep.shouldAnnounce(wide),
              "after a disconnect the same ceiling announces again");
    }

    if (g_failures == 0)
        std::fprintf(stderr, "backend_capability_revision_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
