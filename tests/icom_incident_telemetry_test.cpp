// Socket-free Icom incident telemetry contract.
//
// Positive radio/session convergence belongs to the live automation bridge.
// This test drives only the deterministic backend state transition that turns
// an expired key-on confirmation into a payload-free support dossier.

#include "core/backends/icom/IcomCivBackend.h"

#include <QCoreApplication>
#include <QStringList>
#include <QVariantMap>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace AetherSDR;
using namespace AetherSDR::icom;

namespace AetherSDR::icom {

struct IcomCivBackendTestAccess {
    static void prepareExpiredKeyOn(IcomCivBackend& backend,
                                    const IcomModel& model,
                                    std::uint64_t generation)
    {
        backend.m_model = &model;
        backend.m_connected = true;
        backend.m_sessionGeneration = generation;
        backend.m_keyed = false;
        backend.m_pendingPttIntent = true;
        backend.m_pendingPttUntilMs = backend.nowMs() - 1;
    }

    static void deliver(IcomCivBackend& backend, const CivFrame& frame,
                        std::uint64_t generation)
    {
        backend.onCivFrame(frame, generation);
    }

    static QVariantMap incident(const IcomCivBackend& backend)
    {
        return backend.m_lastIncident;
    }

    // A backend that has a radio-authoritative frequency and one frequency
    // write outstanding on the wire — the state a refused tune arrives into.
    static void prepareOutstandingFrequencyWrite(IcomCivBackend& backend,
                                                 const IcomModel& model,
                                                 std::uint64_t generation,
                                                 std::uint64_t heldHz)
    {
        backend.m_model = &model;
        backend.m_connected = true;
        backend.m_sessionGeneration = generation;
        backend.m_frequencyHz = heldHz;
        IcomCivScheduler::Request request;
        request.frame = cmdSetFrequency(model.civAddress, heldHz + 1'000'000);
        request.key = "frequency";
        request.expectsReply = true;
        request.acceptsGenericReply = true;
        backend.m_civScheduler.enqueue(request, backend.nowMs());
        // Take it off the queue so it is genuinely in flight: observe() only
        // retires a transaction that was actually dispatched, and the whole
        // point is that the FA below completes THIS request.
        (void)backend.m_civScheduler.takeNext(backend.nowMs());
    }

    static std::string lastCompletedKey(const IcomCivBackend& backend)
    {
        return backend.m_civScheduler.stats().lastCompletedKey;
    }

    static void prepareAcceptedPttRead(IcomCivBackend& backend,
                                       const IcomModel& model,
                                       std::uint64_t sessionGeneration)
    {
        backend.m_model = &model;
        backend.m_connected = true;
        backend.m_sessionGeneration = sessionGeneration;
        const std::vector<std::uint8_t> read =
            buildFrameSub(model.civAddress, cmd::kControl, control::kPtt);
        backend.queueRead(read, "ptt", IcomCivScheduler::Priority::Operator);
        (void)backend.m_civScheduler.takeNext(backend.nowMs());
    }
};

}  // namespace AetherSDR::icom

namespace {

int failures = 0;

void check(bool condition, const char* message)
{
    std::printf("%s  %s\n", condition ? "PASS" : "FAIL", message);
    if (!condition) {
        ++failures;
    }
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    const IcomModel* ic705 = modelForName("IC-705");
    check(ic705 != nullptr, "incident telemetry resolves the IC-705 profile");
    if (!ic705) {
        return 1;
    }

    IcomCivBackend backend;
    constexpr std::uint64_t kGeneration = 1;
    IcomCivBackendTestAccess::prepareExpiredKeyOn(
        backend, *ic705, kGeneration);

    CivFrame unkeyed;
    unkeyed.to = kControllerAddress;
    unkeyed.from = ic705->civAddress;
    unkeyed.cmd = cmd::kControl;
    unkeyed.hasSub = true;
    unkeyed.sub = control::kPtt;
    unkeyed.data = {0x00};
    IcomCivBackendTestAccess::deliver(backend, unkeyed, kGeneration);

    const QVariantMap incident = IcomCivBackendTestAccess::incident(backend);
    const QVariantMap ptt = incident.value(QStringLiteral("ptt")).toMap();
    const QVariantMap commandPlane =
        incident.value(QStringLiteral("commandPlane")).toMap();
    check(incident.value(QStringLiteral("kind")).toString()
                  == QLatin1String("ptt-not-confirmed")
              && incident.value(QStringLiteral("model")).toString()
                  == QLatin1String("IC-705"),
          "expired key-on records a typed, model-scoped incident");
    check(ptt.value(QStringLiteral("pendingIntent")).toBool()
              && ptt.value(QStringLiteral("intentKeyed")).toBool()
              && !ptt.value(QStringLiteral("publishedKeyed")).toBool(),
          "incident preserves requested and published PTT state before cleanup");
    check(commandPlane.contains(QStringLiteral("scheduler"))
              && commandPlane.contains(QStringLiteral("transactions")),
          "incident includes scheduler state and bounded transaction history");

    QVariantMap extensionResult;
    QObject::connect(&backend, &IRadioBackend::extensionResult, &app,
                     [&extensionResult](quint64 id, const QVariant& result) {
                         if (id == 0x1C1D) {
                             extensionResult = result.toMap();
                         }
                     });
    backend.invokeExtension(QStringLiteral("icom"),
                            QStringLiteral("civ.incident"), 0x1C1D, {});
    check(extensionResult.value(QStringLiteral("kind")).toString()
              == QLatin1String("ptt-not-confirmed"),
          "read-only CI-V incident verb returns the retained dossier");

    IcomCivBackend confirmationBackend;
    std::vector<bool> confirmations;
    QObject::connect(&confirmationBackend, &IRadioBackend::keyingStateConfirmed,
                     &app, [&confirmations](bool keyed) {
                         confirmations.push_back(keyed);
                     });
    IcomCivBackendTestAccess::prepareAcceptedPttRead(
        confirmationBackend, *ic705, kGeneration);
    check(confirmations.empty(),
          "queueing a PTT read publishes no optimistic radio confirmation");
    IcomCivBackendTestAccess::deliver(
        confirmationBackend, unkeyed, kGeneration);
    check(confirmations.size() == 1 && !confirmations.front(),
          "only an accepted CI-V PTT-off readback publishes confirmation");

    // ---- A REFUSED TUNE IS NOT A SUCCESSFUL ONE --------------------------
    //
    // FA is the radio's NG. observe() retires FB and FA identically — both
    // merely release the slot and carry no state — so before this, nothing in
    // the backend consumed a refusal and the optimistic frequency stood.
    // isNg() existed in CivCodec.h with no caller in the backend at all.
    //
    // Reachable in ordinary use on an IC-9700: three bands, two receivers, so
    // a receiver cannot take a band the other one already holds. The radio
    // answers cmd 05 with FA and does not move. Measured on hardware
    // 2026-08-29 — six cross-band sets, six FAs, display followed all six
    // (#4840).
    {
        constexpr std::uint64_t kHeldHz = 145'030'000;
        IcomCivBackend refusedBackend;
        std::vector<double> published;
        QObject::connect(&refusedBackend, &IRadioBackend::sliceChanged, &app,
                         [&published](int, const SliceDelta& delta) {
                             if (delta.frequency)
                                 published.push_back(*delta.frequency);
                         });
        QStringList warnings;
        QObject::connect(&refusedBackend, &IRadioBackend::configurationWarning,
                         &app, [&warnings](const QString& w) { warnings << w; });

        IcomCivBackendTestAccess::prepareOutstandingFrequencyWrite(
            refusedBackend, *ic705, kGeneration, kHeldHz);

        CivFrame refused;
        refused.to = kControllerAddress;
        refused.from = ic705->civAddress;
        refused.cmd = kCivNg;
        IcomCivBackendTestAccess::deliver(refusedBackend, refused, kGeneration);

        // The correction is deferred one event-loop turn, for the same reason
        // setSliceFrequency()'s out-of-band gate defers it: SliceModel has
        // already announced the operator's request, so a direct emit would be
        // announced away and the indicator would keep lying.
        QCoreApplication::processEvents();

        check(IcomCivBackendTestAccess::lastCompletedKey(refusedBackend)
                  == "frequency",
              "the FA retires the outstanding frequency write");
        check(!warnings.isEmpty()
                  && warnings.constLast().contains(QLatin1String("refused")),
              "a refused tune TELLS the operator the radio said no");
        // The load-bearing assertion: a backend that ignores FA publishes
        // nothing here, and the display keeps the frequency the radio rejected.
        check(published.size() == 1
                  && std::llround(published.front() * 1.0e6)
                         == static_cast<long long>(kHeldHz),
              "a refused tune republishes the radio's real VFO, not the "
              "frequency the radio rejected");
    }

    return failures == 0 ? 0 : 1;
}
