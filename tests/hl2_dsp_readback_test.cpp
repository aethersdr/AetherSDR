// The DSP read-back: does it report the DSP, or does it report the request?
//
// §8's first "still missing" item exists because `get_state` answers from the
// MODEL, so a control that moves the model and never reaches the DSP reads as
// "the control does nothing" — the hardest symptom to act on, because it is
// indistinguishable from the operator having misunderstood the control.
//
// A read-back is only worth having if it can FAIL to agree with the request.
// So these cases are not "the accessor returns something": they are that the
// value MOVES when the DSP is reconfigured, and that it does NOT move when
// only the request does. A read-back that returned its own input would pass a
// test written the first way and be worthless.

#include "TestSettingsProfile.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/MetisClient.h"
#include "core/backends/hl2/Hl2TxDsp.h"

#include <QCoreApplication>
#include <QEvent>
#include <QVariantList>
#include <QVariantMap>

#include <cmath>
#include <cstdio>
#include <string>

namespace AetherSDR::hl2 {

// Seeds only the real modulator, on its owning thread. No connectRadio(),
// discovery, WDSP RX opens, transport start or TX audio processing occurs.
struct Hl2DspReadbackTestAccess {
    static bool configure(Hl2Backend& backend, Qt::ConnectionType type = Qt::BlockingQueuedConnection)
    {
        return QMetaObject::invokeMethod(backend.m_txDsp, [&backend] {
            backend.m_txDsp->configure(Hl2TxDsp::Config{});
        }, type);
    }
    static void tearDown(Hl2Backend& backend) { backend.tearDownReceivers(); }
    static void linkDown(Hl2Backend& backend)
    {
        QMetaObject::invokeMethod(backend.m_metis, "linkDown", Qt::BlockingQueuedConnection);
        QCoreApplication::sendPostedEvents(&backend, QEvent::MetaCall);
    }
    static void linkUp(Hl2Backend& backend)
    {
        // The main-thread session startup handler stays queued and is removed
        // at destruction. A wire transition alone does not reconfigure DSP.
        QMetaObject::invokeMethod(backend.m_metis, "linkUp", Qt::BlockingQueuedConnection);
    }
    static void connectFailed(Hl2Backend& backend)
    {
        QMetaObject::invokeMethod(backend.m_metis, "connectFailed", Qt::BlockingQueuedConnection,
                                  Q_ARG(QString, QStringLiteral("injected failure")));
        QCoreApplication::sendPostedEvents(&backend, QEvent::MetaCall);
    }
};

}  // namespace AetherSDR::hl2

using AetherSDR::hl2::Hl2Backend;
using AetherSDR::hl2::Hl2TxDsp;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) {
        ++g_failures;
    }
}
static bool near(double a, double b) { return std::abs(a - b) < 1e-9; }

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("hl2-dsp-readback-test"));
    QCoreApplication app(argc, argv);
    check(profile.isValid(), "settings are isolated");
    std::printf("\n  DSP read-back — reports the DSP, not the request\n\n");

    Hl2TxDsp dsp;
    const auto unconfigured = [](const QVariantList& chains) {
        return chains.size() == 1
            && chains.first().toMap() == QVariantMap{
                {QStringLiteral("chain"), QStringLiteral("hl2-tx")},
                {QStringLiteral("level"), QStringLiteral("not-configured")}};
    };
    check(!dsp.isConfigured() && unconfigured(Hl2Backend::gatherDspChains({}, &dsp)),
          "a fresh modulator reports no configuration fields");

    // 1. What it was configured with is what it reports.
    Hl2TxDsp::Config a;
    a.inputSampleRateHz = 24000;
    a.outputSampleRateHz = 48000;
    a.dspBlockSize = 512;
    a.filterLowHz = 300.0;
    a.filterHighHz = 2700.0;
    a.alcTargetPeak = 0.85;
    a.alcMaxGainDb = 40.0;
    std::string err;
    check(dsp.configure(a, &err), "the modulator accepts a configuration");
    check(dsp.config().filterLowHz == 300.0 && dsp.config().filterHighHz == 2700.0,
          "the read-back reports the passband it was configured with");
    check(near(dsp.config().alcTargetPeak, 0.85)
              && near(dsp.config().alcMaxGainDb, 40.0),
          "the read-back reports the ALC quartet it was configured with");
    check(dsp.config().dspBlockSize == 512 && dsp.config().outputSampleRateHz == 48000,
          "the read-back reports rates and block size");

    // 2. IT MOVES. A read-back that returns a constant would satisfy case 1
    //    and prove nothing, so reconfigure and require the value to change.
    Hl2TxDsp::Config b = a;
    b.filterLowHz = 150.0;
    b.filterHighHz = 3000.0;
    b.alcMaxGainDb = 0.0;
    check(dsp.configure(b, &err), "the modulator accepts a second configuration");
    check(dsp.config().filterLowHz == 150.0 && dsp.config().filterHighHz == 3000.0,
          "the read-back MOVED when the DSP was reconfigured");
    check(near(dsp.config().alcMaxGainDb, 0.0),
          "the ALC ceiling moved with it");

    // 3. THE DEAD-SLIDER CASE, which is the whole reason the verb exists.
    //    Change the REQUEST without reaching the DSP, and the read-back must
    //    keep reporting what the DSP actually has. Here the request is a local
    //    Config the caller has edited and not applied — exactly the shape of a
    //    control that updates the model and never crosses the seam.
    Hl2TxDsp::Config requested = b;
    requested.filterLowHz = 100.0;
    requested.filterHighHz = 3900.0;
    check(requested.filterLowHz != dsp.config().filterLowHz,
          "the request and the DSP now disagree, which is the defect's shape");
    check(dsp.config().filterLowHz == 150.0,
          "the read-back still reports the DSP, NOT the unapplied request");

    // 4. And once the request IS applied, the disagreement closes. Without
    //    this, case 3 could pass on a read-back that had simply stuck.
    check(dsp.configure(requested, &err), "applying the request succeeds");
    check(dsp.config().filterLowHz == 100.0 && dsp.config().filterHighHz == 3900.0,
          "applying it closes the disagreement");

    // ---- ownership: the gather answers from the snapshot it is handed -----
    //
    // Hl2Backend::dspChains() runs its gather on the I/O thread, and m_rx is
    // declared GUI THREAD ONLY: createPanadapter()'s push_back reallocates and
    // removePanadapter()'s erase shifts, either of which can pull the storage
    // out from under a reader midway. The first version iterated m_rx and
    // review caught it (#5401).
    //
    // The invariant is now enforced where it cannot rot: gatherDspChains() is a
    // STATIC member over the two lists it may read, so it has no `this` and the
    // whole body is unable to name m_rx — through a rename, a helper, or an
    // alias alike. That is the compiler's job, not a test's, and it replaces
    // the source scan an earlier revision of this file used.
    //
    // What is left for a test is the behavioural half: that the answer is a
    // function of the snapshot passed in and of nothing else. Substitute the
    // snapshot, and the reply must follow it — in length, in index, and in the
    // values it reports. A gather reading some other list would keep answering
    // the same thing while the argument changed.
    {
        check(Hl2Backend::gatherDspChains({}, nullptr).isEmpty(),
              "an empty snapshot with no TX chain gathers nothing");

        // Length and index come from the snapshot. Null entries are deliberate:
        // a receiver that exists with no channel behind it is a state worth
        // seeing, and it keeps this case free of any WDSP channel.
        const QVariantList three = Hl2Backend::gatherDspChains(
            {nullptr, nullptr, nullptr}, nullptr);
        check(three.size() == 3, "three receivers in gives three chains out");
        const QVariantList one = Hl2Backend::gatherDspChains({nullptr}, nullptr);
        check(one.size() == 1,
              "and ONE receiver in gives one — the count follows the argument");
        bool indexed = true;
        for (int i = 0; i < three.size(); ++i) {
            const QVariantMap e = three.at(i).toMap();
            indexed = indexed
                      && e.value(QStringLiteral("chain")).toString()
                             == QLatin1String("rx-wdsp")
                      && e.value(QStringLiteral("receiver")).toInt() == i
                      && e.value(QStringLiteral("level")).toString()
                             == QLatin1String("not-configured");
        }
        check(indexed,
              "each entry is indexed by its position in the snapshot, and an "
              "empty slot reports not-configured rather than vanishing");

        // The TX chain's VALUES come from the modulator handed in, and move
        // when it does. `dsp` above is configured with the passband from case
        // 4 (100/3900), so this also pins that the gather reads the DSP's own
        // config rather than a copy taken earlier.
        const QVariantList tx = Hl2Backend::gatherDspChains({}, &dsp);
        check(tx.size() == 1, "a modulator with no receivers gathers one chain");
        const QVariantMap t = tx.isEmpty() ? QVariantMap{} : tx.at(0).toMap();
        check(t.value(QStringLiteral("chain")).toString() == QLatin1String("hl2-tx")
                  && t.value(QStringLiteral("level")).toString()
                         == QLatin1String("dsp-config"),
              "it is named and levelled as the TX chain");
        check(near(t.value(QStringLiteral("filterLowHz")).toDouble(), 100.0)
                  && near(t.value(QStringLiteral("filterHighHz")).toDouble(), 3900.0),
              "and reports the modulator's CURRENT passband");

        Hl2TxDsp::Config moved = dsp.config();
        moved.filterLowHz = 200.0;
        moved.filterHighHz = 2800.0;
        check(dsp.configure(moved, &err), "the modulator takes a further change");
        const QVariantList after = Hl2Backend::gatherDspChains({}, &dsp);
        const QVariantMap t2 = after.isEmpty() ? QVariantMap{} : after.at(0).toMap();
        check(near(t2.value(QStringLiteral("filterLowHz")).toDouble(), 200.0)
                  && near(t2.value(QStringLiteral("filterHighHz")).toDouble(), 2800.0),
              "which the gather reports — the values track the object passed in");

        // Both halves at once, in order: receivers first, TX last.
        const QVariantList both = Hl2Backend::gatherDspChains({nullptr, nullptr},
                                                              &dsp);
        check(both.size() == 3, "two receivers and a modulator gather three chains");
        check(!both.isEmpty()
                  && both.last().toMap().value(QStringLiteral("chain")).toString()
                         == QLatin1String("hl2-tx"),
              "with the TX chain last, after the receivers");
    }

    // Refusal invalidates read-back; ordinary unkey/reset does not.
    check(dsp.isConfigured(), "successful configuration marks the chain valid");
    dsp.reset();
    check(dsp.isConfigured()
              && !unconfigured(Hl2Backend::gatherDspChains({}, &dsp)),
          "unkey reset preserves the configured chain");
    Hl2TxDsp::Config invalid = a;
    invalid.inputSampleRateHz = 0;
    check(!dsp.configure(invalid, &err)
              && unconfigured(Hl2Backend::gatherDspChains({}, &dsp)),
          "invalid rates withdraw a previously valid configuration");
    check(dsp.configure(a, &err), "a valid retry restores configuration");
    invalid = a;
    invalid.outputSampleRateHz = 48001;
    check(!dsp.configure(invalid, &err)
              && unconfigured(Hl2Backend::gatherDspChains({}, &dsp)),
          "a refused non-integer ratio also withdraws configuration");
    check(dsp.configure(a, &err), "configuration can be restored after refusal");
    dsp.invalidateConfiguration();
    check(unconfigured(Hl2Backend::gatherDspChains({}, &dsp)),
          "session deactivation hides all previous-session values");

    // Actual backend lifecycle, without ever starting a transport. Read-back
    // crosses the same I/O queue as invalidation, so it also checks ordering.
    {
        using Access = AetherSDR::hl2::Hl2DspReadbackTestAccess;
        Hl2Backend backend;
        check(unconfigured(backend.dspChains()), "fresh backend reports TX not-configured");
        check(Access::configure(backend) && !unconfigured(backend.dspChains()),
              "backend reads its configured I/O-owned modulator");
        backend.disconnectRadio();
        check(unconfigured(backend.dspChains()), "disconnect withdraws the session config");
        check(Access::configure(backend, Qt::QueuedConnection), "queue a new configuration");
        backend.disconnectRadio();
        check(unconfigured(backend.dspChains()),
              "cancellation invalidation runs after a configuration already queued");
        check(Access::configure(backend), "restore before receiver teardown");
        Access::tearDown(backend);
        check(unconfigured(backend.dspChains()), "receiver teardown withdraws TX read-back");
        check(Access::configure(backend), "restore before injected link loss");
        Access::linkDown(backend);
        check(!unconfigured(backend.dspChains()),
              "link loss preserves the applied configuration of retained DSP");
        check(Access::configure(backend), "restore before injected connection failure");
        Access::connectFailed(backend);
        check(unconfigured(backend.dspChains()), "connection failure withdraws TX read-back");
        check(Access::configure(backend), "restore before transient link loss");
        Access::linkDown(backend);
        check(!unconfigured(backend.dspChains()), "transient silence retains config");
        Access::linkUp(backend);
        check(!unconfigured(backend.dspChains()), "link resume preserves the same valid config");
    }

    if (g_failures == 0) {
        std::printf("\nALL PASS\n");
        return 0;
    }
    std::printf("\nFAILURES PRESENT\n");
    return 1;
}
