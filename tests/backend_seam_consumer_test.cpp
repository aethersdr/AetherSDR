// Every signal IRadioBackend declares has a consumer once RadioModel has wired
// a backend (#5678 row 2.5).
//
// The defect class this pins is "declared, emitted, connected by nobody":
// IRadioBackend::waterfallRowReady was declared as a data-plane outlet, RTL-SDR
// emitted it on every FFT frame, and no connect() anywhere received it. It
// looked like a live waterfall path and carried nothing. A backend author
// reading the interface would reasonably emit it and see no rows.
//
// The check walks IRadioBackend's own meta-object, so a signal added to the
// header is covered with no edit here. For each family the factory can build,
// RadioModel::rebuildBackendForTest() runs the PRODUCTION setupBackend()
// wiring (no dial, no socket, no device), and every declared signal must then
// report QObject::isSignalConnected() on the backend.
//
// Receiving the signal is not the same as rendering it; this test does not
// claim the consumer does anything useful. It claims only that no seam signal
// is an outlet into nothing.
//
// A few signals are legitimately unconnected at rest, and each is named below
// with the consumer that connects it and why that consumer is not standing.
// A new entry there is a claim a reviewer can check, not a mute button.

#include "TestSettingsProfile.h"
#include "core/backends/IRadioBackend.h"
#include "models/RadioModel.h"

#include <QCoreApplication>
#include <QMetaMethod>
#include <QString>
#include <QStringList>

#include <cstdio>

using namespace AetherSDR;

namespace {
int g_failures = 0;
void check(bool ok, const QString& what)
{
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", qPrintable(what));
    g_failures += !ok;
}

// QObject::isSignalConnected() is protected. Naming it through a derived class
// yields a pointer-to-member of QObject that may be applied to any QObject.
// WHAT A GREEN RUN DOES NOT PROVE. isSignalConnected() says that SOMETHING is
// connected, not WHO. IRadioBackend's own constructor connects `connected`,
// `disconnected` and `sliceRemoved` to itself for PCM session bookkeeping
// (IRadioBackend.h), so those three pass here even on a family where nothing
// above the seam listens -- on flex and sim, RadioModel wires `connected` and
// `disconnected` in the same `if (!m_connection)` block as the two exemptions
// below. The pin this test exists for still holds: a NEWLY declared outlet is
// not self-connected, so a waterfallRowReady-shaped orphan is still caught.
struct ConnectedPeek : QObject {
    static bool connected(const QObject* obj, const QMetaMethod& sig)
    {
        return (obj->*(&ConnectedPeek::isSignalConnected))(sig);
    }
};
// Signals with no STANDING connection after setupBackend(), and where their
// consumer lives instead. `families` empty = every family.
struct Exempt {
    const char* signal;
    QStringList families;
    const char* consumer;
};
const Exempt kExempt[] = {
    // Request-scoped: connected for the lifetime of one invokeExtension()
    // call and disconnected on its reply (RadioModel::invokeBackendExtension,
    // AutomationServer, RadioSetupDialog, BandscopeDialog, DroopCalibration).
    {"extensionResult", {}, "per-request connect around invokeExtension()"},
    {"extensionError",  {}, "per-request connect around invokeExtension()"},
    // Flex and sim report the link through their RadioConnection, so
    // setupBackend() wires the seam's connection signals only when
    // m_connection is null (RadioModel.cpp, `if (!m_connection)`).
    {"connectionError", {QStringLiteral("flex"), QStringLiteral("sim")},
     "RadioConnection::errorOccurred"},
    {"configurationWarning", {QStringLiteral("flex"), QStringLiteral("sim")},
     "RadioConnection path; seam wired only when m_connection is null"},
};

bool exempt(const QString& family, const QByteArray& signal)
{
    for (const Exempt& e : kExempt) {
        if (signal == e.signal && (e.families.isEmpty() || e.families.contains(family)))
            return true;
    }
    return false;
}
} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("backend-seam-consumer"));
    if (!profile.isValid()) { return 1; }
    QCoreApplication app(argc, argv);

    const QMetaObject& mo = IRadioBackend::staticMetaObject;
    QStringList declared;
    for (int i = mo.methodOffset(); i < mo.methodCount(); ++i) {
        if (mo.method(i).methodType() == QMetaMethod::Signal)
            declared << QString::fromLatin1(mo.method(i).name());
    }
    check(declared.size() >= 40,
          QStringLiteral("IRadioBackend declares %1 signals (non-vacuous)").arg(declared.size()));

    int familiesChecked = 0;
    {
        RadioModel model;
        const QStringList families = {
            QStringLiteral("sim"), QStringLiteral("hl2"), QStringLiteral("anan"),
            QStringLiteral("icom"), QStringLiteral("rtl"), QStringLiteral("flex"),
        };
        for (const QString& family : families) {
            if (!model.rebuildBackendForTest(family)) {
                std::printf("  %s: not built into this binary, skipped\n", qPrintable(family));
                continue;
            }
            IRadioBackend* backend = model.backend();
            if (!backend) {
                check(false, QStringLiteral("%1: backend present").arg(family));
                continue;
            }
            ++familiesChecked;
            QStringList orphans;
            for (int i = mo.methodOffset(); i < mo.methodCount(); ++i) {
                const QMetaMethod sig = mo.method(i);
                if (sig.methodType() != QMetaMethod::Signal)
                    continue;
                if (!ConnectedPeek::connected(backend, sig) && !exempt(family, sig.name()))
                    orphans << QString::fromLatin1(sig.name());
            }
            check(orphans.isEmpty(),
                  QStringLiteral("%1: every IRadioBackend signal has a consumer%2")
                      .arg(family,
                           orphans.isEmpty() ? QString()
                                             : QStringLiteral(" (unconnected: %1)")
                                                   .arg(orphans.join(QStringLiteral(", ")))));
        }
    }
    check(familiesChecked >= 1,
          QStringLiteral("at least one family wired (%1)").arg(familiesChecked));

    std::printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
