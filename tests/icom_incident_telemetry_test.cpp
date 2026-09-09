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

    static QVariantMap freshness(const IcomCivBackend& backend)
    {
        return backend.stateFreshness();
    }
    static void age(IcomCivBackend& backend, const QString& key)
    {
        backend.m_confirmedState[key].atMs = backend.nowMs() - 6000;
    }
    static void intent(IcomCivBackend& backend, const std::vector<std::uint8_t>& frame)
    {
        backend.queueWrite(frame, {}, IcomCivScheduler::Priority::Operator, true, true);
    }
    static void identify(IcomCivBackend& backend) { backend.m_civReported = 0xA4; }
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

    IcomCivBackend freshBackend;
    IcomCivBackendTestAccess::prepareAcceptedPttRead(freshBackend, *ic705, kGeneration);
    IcomCivBackendTestAccess::identify(freshBackend);
    const auto snapshot = [&]() { return IcomCivBackendTestAccess::freshness(freshBackend); };
    const auto field = [&](const char* name) {
        return snapshot().value("fields").toMap().value(QLatin1String(name)).toMap();
    };
    check(!snapshot().value("trackedStateReady").toBool()
              && field("squelchPercent").value("status") == "never-confirmed",
          "transport and identity do not bless construction defaults");
    const auto deliver = [&](std::uint8_t command, bool hasSub, std::uint8_t sub,
                             std::vector<std::uint8_t> data, std::uint64_t generation = 1) {
        IcomCivBackendTestAccess::deliver(freshBackend,
            CivFrame{kControllerAddress, ic705->civAddress, command, hasSub, sub, data}, generation);
    };
    deliver(0x14, true, 0x03, {0xFA});
    deliver(0x14, true, 0x03, {0x00, 0x50}, 99);
    check(field("squelchPercent").value("status") == "never-confirmed",
          "malformed and previous-session frames cannot establish freshness");
    deliver(0x14, true, 0x03, {0x00, 0x51});
    check(field("squelchPercent").value("value").toInt() == 20
              && field("squelchPercent").value("status") == "confirmed",
          "decoded SQL reply confirms the radio value");
    IcomCivBackendTestAccess::age(freshBackend, QStringLiteral("civ.20.3"));
    check(field("squelchPercent").value("status") == "stale",
          "unchanged values still age out");
    deliver(0x14, true, 0x03, {0x00, 0x51});
    check(field("squelchPercent").value("status") == "confirmed",
          "unchanged valid replies refresh their own field");
    IcomCivBackendTestAccess::intent(freshBackend, cmdSetLevel(ic705->civAddress, level::kSquelch, 60));
    check(field("squelchPercent").value("status") == "pending",
          "write intent cannot masquerade as radio confirmation");
    deliver(0xFB, false, 0, {});
    check(field("squelchPercent").value("status") == "pending",
          "generic ACK cannot confirm a state value");
    deliver(0x14, true, 0x03, {0x00, 0x60});
    deliver(0x03, false, 0, {0x00, 0x00, 0x20, 0x07, 0x00});
    deliver(0x26, true, 0, {0x01, 0x00, 0x01});
    deliver(0x16, true, 0x12, {0x02});
    deliver(0x14, true, 0x0A, {0x00, 0x13});
    deliver(0x1C, true, 0, {0});
    check(snapshot().value("trackedStateReady").toBool(),
          "all six decoded fields establish bounded diagnostic readiness");
    deliver(0x26, true, 0, {0x02, 0x00, 0x01});
    check(!snapshot().value("trackedStateReady").toBool()
              && field("agcCode").value("status") == "previous-context",
          "radio-originated mode change invalidates old-context controls");
    deliver(0x16, true, 0x12, {0xFF});
    check(field("agcCode").value("status") == "previous-context",
          "out-of-range AGC cannot refresh the context");
    IcomCivBackendTestAccess::intent(freshBackend,
        buildFrameSub(ic705->civAddress, 0x07, 0x01));
    check(field("modeDataFilter").value("status") == "previous-context",
          "outgoing VFO selection invalidates even an identical mode and frequency");
    IcomCivBackend neverConfirmed;
    IcomCivBackendTestAccess::intent(neverConfirmed,
        cmdSetLevel(ic705->civAddress, level::kSquelch, 60));
    const QVariantMap unknownSql = IcomCivBackendTestAccess::freshness(neverConfirmed)
        .value("fields").toMap().value("squelchPercent").toMap();
    check(unknownSql.value("status") == "pending" && !unknownSql.value("value").isValid(),
          "a first write records pending intent without inventing a confirmed value");
    check(!snapshot().value("backendInstanceId").toString().isEmpty()
        && snapshot().value("backendInstanceId") != IcomCivBackendTestAccess::freshness(neverConfirmed).value("backendInstanceId"),
          "backend replacement in one process has a distinct diagnostic ID namespace");
    return failures == 0 ? 0 : 1;
}
