// IRadioBackend threading contract, rules 1, 2 and 6 (IRadioBackend.h,
// "THREADING AND LIFETIME CONTRACT"): every seam signal is emitted from the
// thread the backend object lives on, and nothing is emitted after
// disconnected().
//
// The probe connects to EVERY signal IRadioBackend declares with
// Qt::DirectConnection, so the lambda runs on whichever thread actually
// emits, and records whether that thread is the backend's own. A backend
// that forwards a worker's frame with a Direct connection, or emits from
// inside a worker callback, shows up here as a named violation with the
// offending thread.
//
// The probe table is checked against the meta-object at runtime: a signal
// added to IRadioBackend without a probe entry fails this test, so the
// contract cannot silently stop covering a new signal.
//
// WHAT THIS TEST ACTUALLY COVERS, per family:
//
//   sim   — rules 1, 2 and 6 for real. Driven through RadioModel across its
//           whole surface (connect, the data plane, slice verbs, pan
//           create/remove, extension verbs, disconnect, and a SECOND session
//           so rule 6 is judged per session), with a minimum-emission floor so
//           the assertions cannot pass vacuously. Forcing the sim's
//           audioFrameReady forward to Qt::DirectConnection fails this test.
//
//   flex, hl2, anan, icom, rtl — rule 1 (the backend is constructed on its
//           owner's thread) plus "construction and the synchronous getters
//           emit nothing they should not". These are never connected, so they
//           emit nothing and rules 2 and 6 are NOT exercised for them here;
//           the report says so rather than printing a pass over zero
//           observations. Live-emission affinity for hl2 is pinned by
//           hl2_connect_reentrancy_test, which drives connectRadio() with the
//           DSP build on the I/O thread and carries the same probe. For the
//           remaining families it is a survey result until the probe is
//           dropped into a test that drives one of them.

#include "SeamThreadAffinityProbe.h"
#include "TestSettingsProfile.h"
#include "core/backends/IRadioBackend.h"
#include "core/RadioDiscovery.h"
#include "core/backends/sim/SimBackend.h"
#include "models/PanadapterModel.h"
#include "models/RadioModel.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QHostAddress>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QTimer>

#include <cstdio>

using namespace AetherSDR;
using AetherSDR::test::SeamThreadAffinityProbe;
using AetherSDR::test::attachAllSeamSignals;
using AetherSDR::test::declaredSeamSignals;
using Probe = SeamThreadAffinityProbe;

namespace {
int g_failures = 0;
void check(bool ok, const QString& what)
{
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", qPrintable(what));
    g_failures += !ok;
}

void spin(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

// `exercised` says whether this family was actually driven. A probe that
// recorded nothing cannot judge rules 2 or 6, and saying so beats printing a
// pass over zero observations.
void reportProbe(const QString& family, const Probe& p, bool exercised)
{
    const QStringList observed = p.observed();
    std::printf("  %s observed: %s\n", qPrintable(family),
                observed.isEmpty() ? "(nothing)"
                                   : qPrintable(observed.join(QStringLiteral(", "))));
    for (const QString& v : p.violations()) {
        std::printf("  VIOLATION [%s]: %s\n", qPrintable(family), qPrintable(v));
    }
    if (!exercised || observed.isEmpty()) {
        std::printf("  NOTE [%s]: 0 emissions — rules 2 and 6 not exercised for this family\n",
                    qPrintable(family));
        // Still meaningful: a backend that emitted something during bare
        // construction or a synchronous getter would show up here.
        check(p.violations().isEmpty(),
              QStringLiteral("%1: construction and the synchronous getters emit nothing off-thread")
                  .arg(family));
        return;
    }
    check(p.violations().isEmpty(),
          QStringLiteral("%1: every seam signal emitted on the backend's thread").arg(family));
    check(p.afterDisconnect().isEmpty(),
          QStringLiteral("%1: nothing emitted after disconnected() (%2)")
              .arg(family, p.afterDisconnect().join(QStringLiteral(", "))));
}

RadioInfo demoInfo()
{
    RadioInfo i;
    i.name    = QStringLiteral("FLEX-6700");
    i.model   = SimBackend::demoModelName();
    i.serial  = SimBackend::demoSerial();
    i.family  = SimBackend::familyName();
    i.address = QHostAddress(QHostAddress::LocalHost);   // synthetic; never dialed
    i.port    = 4992;
    return i;
}

// The reference implementation, driven through RadioModel exactly as the app
// drives the demo: the synthetic wire (pan create/remove) only runs on that
// path, so a standalone SimBackend cannot reach it.
void simulatorFullSurface()
{
    std::printf("-- sim: through RadioModel, full surface\n");
    RadioModel model;
    model.connectToRadio(demoInfo());          // rebuilds the backend, then dials synthetically
    IRadioBackend* sim = model.backend();
    check(sim != nullptr && model.family() == QLatin1String("sim"), "sim: model built the simulator");
    if (!sim) return;
    Probe p(sim);
    attachAllSeamSignals(p);

    for (int i = 0; i < 60 && !model.isConnected(); ++i) spin(50);
    check(model.isConnected(), "sim: connected through the model");
    spin(400);                                   // initial state + first frames

    sim->setSliceFrequency(0, 7'074'000.0);
    sim->setSliceMode(0, QStringLiteral("CW"));
    sim->setSliceFilter(0, 100, 2700);
    sim->setSliceAgc(0, QStringLiteral("med"), 50);
    sim->setSliceAudioMute(0, true);
    sim->setSliceAudioGain(0, 40);
    sim->setSliceAudioPan(0, 60);
    sim->setSliceAudioMute(0, false);
    spin(100);

    const int pansBefore = model.panadapters().size();
    model.createPanadapter();
    for (int i = 0; i < 60 && model.panadapters().size() == pansBefore; ++i) spin(50);
    check(model.panadapters().size() == pansBefore + 1, "sim: second pan materialised in the model");
    QString newPan;
    for (const PanadapterModel* pan : model.panadapters()) {
        if (pan->panId().compare(QStringLiteral("0x40000000"), Qt::CaseInsensitive) != 0) newPan = pan->panId();
    }
    check(!newPan.isEmpty(), "sim: new pan has its own id");
    sim->setPanCenter(newPan, 7'100'000.0, IRadioBackend::PanCenterIntent::Drag);
    spin(100);
    model.removePanadapter(newPan);
    for (int i = 0; i < 60 && model.panadapters().size() != pansBefore; ++i) spin(50);
    check(model.panadapters().size() == pansBefore, "sim: second pan removed from the model");

    sim->invokeExtension(QStringLiteral("sim"), QStringLiteral("noise.enable"), 1, true);
    sim->invokeExtension(QStringLiteral("sim"), QStringLiteral("bogus.verb"), 2, {});
    sim->invokeExtension(QStringLiteral("nope"), QStringLiteral("x"), 3, {});
    spin(600);                                   // let the data plane run

    model.disconnectFromRadio();
    for (int i = 0; i < 60 && model.isConnected(); ++i) spin(50);
    spin(400);                                   // anything a worker had queued lands here

    check(p.count(QStringLiteral("connected")) == 1, "sim: connected once");
    check(p.count(QStringLiteral("disconnected")) == 1, "sim: disconnected once");
    check(p.count(QStringLiteral("sliceChanged")) >= 1, "sim: sliceChanged observed");
    check(p.count(QStringLiteral("radioChanged")) >= 1, "sim: radioChanged observed");
    check(p.count(QStringLiteral("panCenterBandwidthChanged")) >= 2, "sim: pan geometry observed for both pans");
    check(p.count(QStringLiteral("audioFrameReady")) >= 5, "sim: audio data plane observed");
    check(p.count(QStringLiteral("spectrumFrameReady")) >= 5, "sim: spectrum data plane observed");
    check(p.count(QStringLiteral("extensionResult")) + p.count(QStringLiteral("extensionError")) >= 2,
          "sim: extension replies observed");

    // A SECOND session. Rule 6 is per session — a reconnect legitimately
    // follows a disconnect — so the gate is reset and the new session's
    // traffic is judged on its own. Without the reset every emission after the
    // first disconnect would read as a violation.
    const int audioAfterFirstSession = p.count(QStringLiteral("audioFrameReady"));
    p.resetDisconnectGate();
    model.connectToRadio(demoInfo());
    for (int i = 0; i < 60 && !model.isConnected(); ++i) spin(50);
    check(model.isConnected(), "sim: second session connected");
    spin(500);
    check(p.count(QStringLiteral("connected")) == 2, "sim: connected twice across two sessions");
    check(p.count(QStringLiteral("audioFrameReady")) > audioAfterFirstSession,
          "sim: the second session streams audio of its own");
    model.disconnectFromRadio();
    for (int i = 0; i < 60 && model.isConnected(); ++i) spin(50);
    spin(400);
    check(p.count(QStringLiteral("disconnected")) == 2, "sim: disconnected twice");

    reportProbe(QStringLiteral("sim"), p, /*exercised=*/true);
}

// Every other family: built by the production factory, asked the synchronous
// questions, disconnected while unconnected, then torn down by the model.
void constructedFamilies()
{
    RadioModel model;
    const QStringList families = {QStringLiteral("flex"), QStringLiteral("hl2"),
                                  QStringLiteral("anan"), QStringLiteral("icom"),
                                  QStringLiteral("rtl")};
    for (const QString& family : families) {
        std::printf("-- %s: constructed, never connected\n", qPrintable(family));
        if (!model.rebuildBackendForTest(family)) {
            std::printf("  %s: not built into this binary, skipped\n", qPrintable(family));
            continue;
        }
        IRadioBackend* b = model.backend();
        check(b != nullptr, QStringLiteral("%1: backend constructed").arg(family));
        if (!b) continue;
        check(b->thread() == QThread::currentThread(),
              QStringLiteral("%1: backend lives on its owner's thread (rule 1)").arg(family));
        Probe p(b);
        attachAllSeamSignals(p);
        const RadioCapabilities caps = b->capabilities();
        check(caps.family.compare(family, Qt::CaseInsensitive) == 0 || caps.family.isEmpty(),
              QStringLiteral("%1: capabilities().family agrees (\"%2\")").arg(family, caps.family));
        (void)b->healthSnapshot();
        (void)b->linkStats();
        (void)b->dspChains();
        (void)b->currentOperatingState();
        b->disconnectRadio();
        spin(200);
        reportProbe(family, p, /*exercised=*/false);
        // Teardown happens on the next rebuild (or ~RadioModel); the probe's
        // context outlives it only within this iteration, so a late emission
        // during destruction would reach a dead lambda — the family-switch test
        // owns that half of the contract.
    }
    // Leave the model on the simulator so the destructor drains a known backend.
    model.rebuildBackendForTest(QStringLiteral("sim"));
}
} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("backend-seam-affinity"));
    if (!profile.isValid()) { return 1; }
    QCoreApplication app(argc, argv);
    QThread::currentThread()->setObjectName(QStringLiteral("main"));

    {
        SimBackend probeTarget;
        Probe p(&probeTarget);
        attachAllSeamSignals(p);
        const QStringList declared = declaredSeamSignals();
        QStringList missing;
        for (const QString& name : declared) {
            if (!p.probedNames().contains(name)) missing << name;
        }
        check(missing.isEmpty(),
              QStringLiteral("probe table covers every declared seam signal (%1 probed, %2 declared%3)")
                  .arg(p.probed()).arg(declared.size())
                  .arg(missing.isEmpty() ? QString() : QStringLiteral("; unprobed: ") + missing.join(QStringLiteral(", "))));
    }

    simulatorFullSurface();
    constructedFamilies();

    std::printf("%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
