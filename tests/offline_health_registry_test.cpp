// Health that survives disconnection, gated on a DECLARATION rather than on a
// family string.
//
// WHAT THIS REPLACED, and why the replacement is not cosmetic. The first
// version of this feature gated its two model-level entry points on
// `m_family != QLatin1String("hl2")`. `docs/HERMES.md`'s "For coding agents —
// keep bring-up inside the family backend" (jensenpat, f6f56458, merged in
// 1457d06d) forbids exactly that construct above the seam, and #5554 §2.8
// separately wants the `dynamic_cast<hl2::Hl2Backend*>` shape retired. Neither
// is a style note: a family test above the seam excludes anything that behaves
// the same way without carrying the name, which is the same defect #5618 fixed
// for extension namespaces.
//
// So the model asks OfflineHealthRegistry what the selected family declared,
// and `src/core/backends/hl2/Hl2TelemetryService.cpp` is what declares it.
//
// THE ASSERTION THAT MATTERS MOST IS THE FIRST ONE. A self-registering
// translation unit that nothing references can be dropped from a static archive
// with no diagnostic anywhere, and the feature then does not exist while every
// other test still passes. That failure is silent by construction, so it gets
// an explicit check rather than trust.
//
// SOCKET-FREE, and by construction rather than by choice of address. The
// earlier version of this file claimed it and was wrong: aiming the real HL2
// source through the model is a synchronous chain — `setOfflineHealthTarget()`
// calls `noteOfflineDemand()`, `Hl2TelemetryService::noteDemand()` applies the
// surface-visible cadence immediately, and `Hl2TelemetryPoller::applyCadence()`
// then binds a UDP socket and calls `onPollTimer()` inline, which writes a
// discovery datagram. TEST-NET-1 is unroutable, so it went to the default
// gateway and no further — but the declaration was false and the test bound a
// socket in ctest's default graph. Reported by ten9876 on #5642, measured under
// an LD_PRELOAD shim on bind/sendmsg.
//
// AGENTS.md's socket carve-out does not cover it either: that exempts tests
// where OUR OWN SERVER is the subject, reached over a socket. Here the subject
// is a registry gate and the socket is a side effect of using the whole stack
// to reach it, which is the case the same table routes to "inject the
// transport".
//
// So section 5 injects one. It re-declares the `hl2` family with a recording
// source that owns no socket, which tests MORE of the verb than aiming the real
// one did — the address it was handed and the demand it was told about are now
// assertions rather than side effects. Sections 1-3 run first and against the
// real declaration; section 3's freshly built service has no target, so its
// cadence is zero and it binds nothing.

#include "models/RadioModel.h"
#include "core/backends/OfflineHealthSource.h"

#include <QCoreApplication>
#include <QHostAddress>
#include <QString>
#include <QVariant>

#include <cstdio>
#include <memory>

using namespace AetherSDR;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok) ++g_failures;
}

// What the double recorded. OUTSIDE the double on purpose: `off` now destroys
// the source (the model takes the backend's borrow back through the seam
// first), so a test that read the counters off the object afterwards would be
// reading freed memory -- and that is exactly the release this section is here
// to prove happened.
struct RecorderState {
    QHostAddress target;
    int aims = 0;
    int demands = 0;
    bool destroyed = false;
};

// A source with no wire under it. Records what the model does to it, so the
// verb's contract is asserted rather than inferred from traffic.
class RecordingOfflineSource final : public IOfflineHealthSource {
public:
    explicit RecordingOfflineSource(RecorderState* st) : m_st(st) {}
    ~RecordingOfflineSource() override { m_st->destroyed = true; }

    void setOfflineTarget(const QHostAddress& addr) override
    {
        m_st->target = addr;
        ++m_st->aims;
    }
    [[nodiscard]] bool hasOfflineTarget() const override
    {
        return !m_st->target.isNull();
    }
    void noteOfflineDemand() override { ++m_st->demands; }
    [[nodiscard]] IRadioBackend::HealthSnapshot offlineHealthRows() const override
    {
        IRadioBackend::HealthSnapshot h;
        // `order` is what isEmpty() reads, so a row that is not in it does not
        // exist as far as any consumer is concerned.
        h.order << QStringLiteral("telemetrySource");
        h.values.insert(QStringLiteral("telemetrySource"),
                        QVariant(QStringLiteral("recording double")));
        h.labels.insert(QStringLiteral("telemetrySource"),
                        QStringLiteral("Telemetry source"));
        return h;
    }

private:
    RecorderState* m_st;
};

// Stands in for a hypothetical SECOND family's offline source. Distinguishable
// from the HL2 one by a row no other source publishes, so "which instrument is
// serving this session" is an assertion rather than an inference.
class SecondFamilySource final : public IOfflineHealthSource {
public:
    void setOfflineTarget(const QHostAddress& addr) override { m_target = addr; }
    [[nodiscard]] bool hasOfflineTarget() const override { return !m_target.isNull(); }
    void noteOfflineDemand() override {}
    [[nodiscard]] IRadioBackend::HealthSnapshot offlineHealthRows() const override
    {
        IRadioBackend::HealthSnapshot h;
        h.order << QStringLiteral("whoAmI");
        h.labels.insert(QStringLiteral("whoAmI"), QStringLiteral("Which source"));
        h.values.insert(QStringLiteral("whoAmI"), QVariant(QStringLiteral("second-family")));
        return h;
    }
    QHostAddress m_target;
};

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ---- 1. the declaration exists at all ----
    check(OfflineHealthRegistry::declaredFor(QStringLiteral("hl2")),
          "hl2 declared an offline health source (registrar was linked in)");
    check(OfflineHealthRegistry::declaredFor(QStringLiteral("HL2")),
          "the lookup is case-insensitive, like every other family key");

    // ---- 2. and no other family claims one ----
    for (const char* fam : {"flex", "icom", "sim", "anan", "rtl", "nonesuch"}) {
        check(!OfflineHealthRegistry::declaredFor(QString::fromLatin1(fam)),
              "no offline source is declared for a family that never declared one");
    }
    check(!OfflineHealthRegistry::declaredFor(QString()),
          "an empty family declares nothing rather than matching everything");

    // ---- 3. create() answers with an object or with null, never a stub ----
    {
        auto none = OfflineHealthRegistry::create(QStringLiteral("sim"), &app);
        check(none == nullptr, "create() returns null for an undeclared family");
        auto some = OfflineHealthRegistry::create(QStringLiteral("hl2"), &app);
        check(some != nullptr, "create() builds one for a declared family");
        if (some) {
            check(!some->hasOfflineTarget(),
                  "a freshly built source is not aimed at anything");
            // Rows exist before any radio has answered — the point of the
            // class. What they must NOT do is claim a reading.
            check(!some->offlineHealthRows().isEmpty(),
                  "it answers with rows even with no target and no backend");
        }
    }

    // ---- 4. through the model: a family that declared nothing is refused ----
    {
        // A default RadioModel builds the Flex backend (family "flex").
        RadioModel m;
        check(!m.hasOfflineHealth(),
              "a Flex session constructs no offline source");
        check(m.setOfflineHealthTarget(QStringLiteral("flex"),
                                       QHostAddress(QStringLiteral("192.0.2.1")))
                  == RadioModel::OfflineAimResult::FamilyDeclaresNone,
              "and aiming at a Flex radio is refused — this is the cross-family "
              "leak that put real datagrams on the wire from a sim session");
        check(m.setOfflineHealthTarget(QStringLiteral("sim"),
                                       QHostAddress(QStringLiteral("192.0.2.1")))
                  == RadioModel::OfflineAimResult::FamilyDeclaresNone,
              "same for sim, and the REASON is reported rather than a bare no — "
              "a caller told 'you are connected' would retry what cannot work");
        check(!m.hasOfflineHealth(),
              "a refused aim constructs nothing, so no rows appear either");
        check(m.offlineHealthRows().isEmpty(),
              "and a family-agnostic health read stays backend-only");
    }

    // ---- 5. through the model: the declaring family is served, and released ----
    //
    // The `hl2` factory is REPLACED here, deliberately and for the rest of this
    // process, so the model builds the recording double instead of the real
    // telemetry service. Everything above this line ran against the real
    // declaration; nothing below it needs a radio, a socket or an address that
    // resolves. OfflineHealthRegistry::declare() warns on a re-declaration --
    // that warning is this line, and it is expected output.
    {
        RecorderState rec;
        OfflineHealthRegistry::declare(
            QStringLiteral("hl2"), [&rec](QObject*) {
                return std::unique_ptr<IOfflineHealthSource>(
                    new RecordingOfflineSource(&rec));
            });

        RadioModel m;
        if (!m.rebuildBackendForTest(QStringLiteral("hl2"))) {
            std::fprintf(stderr, "offline_health_registry_test: no hl2 backend "
                                 "in this build\n");
            return g_failures == 0 ? 0 : 1;
        }
        check(m.hasOfflineHealth(),
              "building the declaring family's backend constructs its source");
        check(!m.offlineHealthRows().isEmpty(),
              "and the rows are available to a health consumer");

        // THE AIM, with no wire under it. The address is a value the verb
        // hands to the source, and asserting that it arrived is a stronger
        // claim than observing a datagram: a datagram proves something was
        // sent, not that it went where the caller asked.
        // Measured as a DELTA, because a health read notes demand too
        // (offlineHealthRows() does it on every call) and an absolute count
        // here would be asserting how many times the test read rows.
        const int demandsBefore = rec.demands;
        check(m.setOfflineHealthTarget(QStringLiteral("hl2"),
                                       QHostAddress(QStringLiteral("192.0.2.1")))
                  == RadioModel::OfflineAimResult::Ok,
              "the declaring family accepts an aim");
        check(rec.target == QHostAddress(QStringLiteral("192.0.2.1")),
              "the source is handed the address the caller named");
        check(rec.demands == demandsBefore + 1,
              "and the aim asserts demand exactly once");
        check(!rec.destroyed, "the source stays alive while aimed");
        check(m.hasOfflineHealth(), "and the model still reports holding one");

        // `target off` must take the ROWS away too, not merely stop the
        // traffic. Leaving them standing was the defect: after "off" the
        // snapshot still described a poller that no longer had a radio, and
        // there was no way back to the snapshot the session started with.
        check(m.setOfflineHealthTarget(m.offlineHealthFamily(), QHostAddress())
                  == RadioModel::OfflineAimResult::Ok,
              "'off' is accepted");
        check(rec.target.isNull(),
              "and 'off' reaches the source as a null address, not as a "
              "second aim");
        check(rec.destroyed,
              "then the source is destroyed — 'stop AND let go', which needs "
              "the backend's borrow handed back through the seam first");

        // 'off' RELEASES, and the backend's borrow is taken back through the
        // seam first so nothing is left holding a destroyed object. The earlier
        // version bailed whenever a backend existed, which made the documented
        // "stop AND let go" unreachable in any real session: teardownBackend()
        // never runs on a plain disconnect, so m_backend is always non-null by
        // the time an operator wants to stop the probe (#5642 review).
        check(!m.hasOfflineHealth(),
              "'off' lets go of the source even though a backend was borrowing it");
        check(m.offlineHealthRows().isEmpty(),
              "so `health` is back to the snapshot the session started with");
        check(m.offlineHealthFamily().isEmpty(),
              "and nothing claims to hold a family's instrument any more");

        // The family switch releases it too, from the other direction.
        check(m.rebuildBackendForTest(QStringLiteral("hl2")),
              "re-arm by rebuilding the declaring family");
        check(m.hasOfflineHealth(), "which constructs a source again");
        check(m.rebuildBackendForTest(QStringLiteral("flex")),
              "switch to a family that declares no offline source");
        check(!m.hasOfflineHealth(),
              "the switch released it — rows do not survive into another family");
        check(m.offlineHealthRows().isEmpty(),
              "and the family-agnostic health read is backend-only again");
    }

    // ---- 5b. stopping something that was never started is not an error ---
    //
    // `off` names no radio, so there is no family to resolve and nothing for a
    // gate to have an opinion about. Reporting a refusal for an idempotent stop
    // would make a caller chase a problem it does not have.
    {
        RadioModel m;
        check(!m.hasOfflineHealth(), "nothing is held");
        check(m.setOfflineHealthTarget(m.offlineHealthFamily(), QHostAddress())
                  == RadioModel::OfflineAimResult::Ok,
              "'off' with nothing aimed is a no-op, not a refusal");
        check(!m.hasOfflineHealth(), "and it constructs nothing to stop");
    }

    // ---- 6. a SECOND declaring family gets its OWN instrument ------------
    //
    // The registry's whole claim is that a family which declares a source in
    // future needs no edit in src/models/. It did not hold: ensureOfflineHealth()
    // built one only `if (!m_offlineHealth)` and never asked whether the source
    // it was holding belonged to the family being asked about, so the second
    // declaring family was served the FIRST one's instrument and published its
    // rows — the same cross-family attribution leak the declaration gate closes
    // for families that declare nothing (#5642 review).
    //
    // Latent while only one family declares, which is exactly why it gets a
    // test now rather than when the second one lands.
    {
        OfflineHealthRegistry::declare(
            QStringLiteral("flex"), [](QObject*) {
                return std::unique_ptr<IOfflineHealthSource>(
                    new SecondFamilySource);
            });

        RadioModel m;
        check(m.rebuildBackendForTest(QStringLiteral("hl2")),
              "build the first declaring family");
        check(m.offlineHealthFamily() == QStringLiteral("hl2"),
              "the source is tagged with the family that built it");
        check(!m.offlineHealthRows().order.contains(QStringLiteral("whoAmI")),
              "and it is NOT the other family's source");

        check(m.rebuildBackendForTest(QStringLiteral("flex")),
              "switch to the second declaring family");
        check(m.hasOfflineHealth(),
              "which declares one too, so a source still exists");
        check(m.offlineHealthFamily() == QStringLiteral("flex"),
              "and the tag followed the switch");
        check(m.offlineHealthRows().values.value(QStringLiteral("whoAmI"))
                  == QVariant(QStringLiteral("second-family")),
              "the SECOND family's source serves the second family's session");
        check(!m.offlineHealthRows().order.contains(QStringLiteral("telemetrySource")),
              "and the FIRST family's rows do not leak into it");

        // The same question from the aim path, which resolves its family from
        // the radio at the address rather than from the session.
        check(m.setOfflineHealthTarget(QStringLiteral("hl2"),
                                       QHostAddress(QStringLiteral("192.0.2.1")))
                  == RadioModel::OfflineAimResult::Ok,
              "aiming at the OTHER family's radio is accepted while connected to "
              "neither — this is the verb working from a cold start");
        check(m.offlineHealthFamily() == QStringLiteral("hl2"),
              "and it rebuilt the aimed family's instrument, not the session's");
        check(!m.offlineHealthRows().order.contains(QStringLiteral("whoAmI")),
              "so the rows belong to the radio that was aimed at");

        // AND THE SESSION TAKING THE WIRE RECLAIMS IT. An aim at another
        // family's radio is legitimate while idle, and must not survive into a
        // session with a different radio: `health` has to describe the radio it
        // is talking to. rebuildBackendForTest() runs the same reclaim
        // connectToRadio() does.
        check(m.rebuildBackendForTest(QStringLiteral("flex")),
              "connect a session of the OTHER declaring family");
        check(m.offlineHealthFamily() == QStringLiteral("flex"),
              "the instrument is rebuilt for the family taking the wire");
        check(!m.offlineHealthRows().order.contains(QStringLiteral("telemetrySource")),
              "so the aimed radio's rows do not follow into this session");
    }

    if (g_failures == 0)
        std::fprintf(stderr, "offline_health_registry_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
