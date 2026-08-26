// Socket-free Icom incident telemetry contract.
//
// Positive radio/session convergence belongs to the live automation bridge.
// This test drives only the deterministic backend state transition that turns
// an expired key-on confirmation into a payload-free support dossier.

#include "core/backends/icom/IcomCivBackend.h"

#include <QCoreApplication>
#include <QVariantMap>

#include <cstdint>
#include <cstdio>
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

    return failures == 0 ? 0 : 1;
}
