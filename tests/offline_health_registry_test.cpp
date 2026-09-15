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

// A source with no wire under it. Records what the model does to it, so the
// verb's contract is asserted rather than inferred from traffic.
class RecordingOfflineSource final : public IOfflineHealthSource {
public:
    void setOfflineTarget(const QHostAddress& addr) override
    {
        m_target = addr;
        ++m_aims;
    }
    [[nodiscard]] bool hasOfflineTarget() const override
    {
        return !m_target.isNull();
    }
    void noteOfflineDemand() override { ++m_demands; }
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

    QHostAddress m_target;
    int m_aims = 0;
    int m_demands = 0;
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
        check(!m.setOfflineHealthTarget(QHostAddress(QStringLiteral("192.0.2.1"))),
              "and refuses to be aimed — this is the cross-family leak that put "
              "real datagrams on the wire from a sim session");
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
        RecordingOfflineSource* recorder = nullptr;
        OfflineHealthRegistry::declare(
            QStringLiteral("hl2"), [&recorder](QObject*) {
                auto made = std::make_unique<RecordingOfflineSource>();
                recorder = made.get();
                return made;
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
        const int demandsBefore = recorder ? recorder->m_demands : -1;
        check(m.setOfflineHealthTarget(QHostAddress(QStringLiteral("192.0.2.1"))),
              "the declaring family accepts an aim");
        check(recorder != nullptr,
              "and the source exists to receive it");
        if (recorder) {
            check(recorder->m_target
                      == QHostAddress(QStringLiteral("192.0.2.1")),
                  "the source is handed the address the caller named");
            check(recorder->m_demands == demandsBefore + 1,
                  "and the aim asserts demand exactly once");
        }
        check(m.hasOfflineHealth(), "and the source stays alive while aimed");

        // `target off` must take the ROWS away too, not merely stop the
        // traffic. Leaving them standing was the defect: after "off" the
        // snapshot still described a poller that no longer had a radio, and
        // there was no way back to the snapshot the session started with.
        check(m.setOfflineHealthTarget(QHostAddress()), "'off' is accepted");
        if (recorder)
            check(recorder->m_target.isNull(),
                  "and 'off' reaches the source as a null address, not as a "
                  "second aim");

        // A backend still exists here and holds a BORROWED pointer, so the
        // source must NOT be destroyed yet — nothing tells a backend its
        // borrowed pointer has gone.
        check(m.hasOfflineHealth(),
              "'off' does not destroy the source while a backend borrows it");

        // The family switch is what releases it. rebuildBackendForTest tears
        // the old backend down first, so by then nothing is borrowing.
        check(m.rebuildBackendForTest(QStringLiteral("flex")),
              "switch to a family that declares no offline source");
        check(!m.hasOfflineHealth(),
              "the switch released it — rows do not survive into another family");
        check(m.offlineHealthRows().isEmpty(),
              "and the family-agnostic health read is backend-only again");
    }

    if (g_failures == 0)
        std::fprintf(stderr, "offline_health_registry_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
