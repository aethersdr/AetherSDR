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
// Socket-free: no target is ever aimed at a reachable address, and the refusal
// paths never construct a poller at all.

#include "models/RadioModel.h"
#include "core/backends/OfflineHealthSource.h"

#include <QCoreApplication>
#include <QHostAddress>
#include <QString>

#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok) ++g_failures;
}

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
    {
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

        check(m.setOfflineHealthTarget(QHostAddress(QStringLiteral("192.0.2.1"))),
              "the declaring family accepts an aim (TEST-NET-1, unroutable)");
        check(m.hasOfflineHealth(), "and the source stays alive while aimed");

        // `target off` must take the ROWS away too, not merely stop the
        // traffic. Leaving them standing was the defect: after "off" the
        // snapshot still described a poller that no longer had a radio, and
        // there was no way back to the snapshot the session started with.
        check(m.setOfflineHealthTarget(QHostAddress()), "'off' is accepted");

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
