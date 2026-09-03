#include "core/backends/icom/IcomCivBackend.h"

#include <QCoreApplication>
#include <QSignalSpy>

#include <cmath>
#include <cstdint>
#include <iostream>
#include <optional>

using namespace AetherSDR;
using namespace AetherSDR::icom;

namespace AetherSDR::icom {

struct IcomCivBackendTestAccess {
    static void prepareGeneration(IcomCivBackend& backend, std::uint64_t generation)
    {
        backend.m_connected = true;
        backend.m_sessionGeneration = generation;
    }

    static void selectModelAndFrequency(IcomCivBackend& backend,
                                        const IcomModel& model,
                                        std::uint64_t frequencyHz)
    {
        backend.m_model = &model;
        backend.m_frequencyHz = frequencyHz;
    }

    static void publishMeterDefinitions(IcomCivBackend& backend)
    {
        backend.publishMeterDefs();
    }

    static void setKeyed(IcomCivBackend& backend, bool keyed)
    {
        backend.m_keyed = keyed;
        backend.m_meters.setTransmitting(keyed);
    }

    static void deliver(IcomCivBackend& backend, const CivFrame& frame,
                        std::uint64_t generation)
    {
        backend.onCivFrame(frame, generation);
    }

    static void expectPttConfirmation(IcomCivBackend& backend, bool keyed)
    {
        backend.m_keyed = !keyed;
        backend.m_pendingPttIntent = keyed;
        backend.m_pendingPttUntilMs = backend.nowMs() + 1000;
    }
};

} // namespace AetherSDR::icom

namespace {

int failures = 0;

void check(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void testIc9700DerivedForwardPowerAcrossBands()
{
    const IcomModel* ic9700 = modelForCivAddress(0xA2);
    check(ic9700 != nullptr, "IC-9700 derived-power fixture resolves the model");
    if (!ic9700) {
        return;
    }

    IcomCivBackend backend;
    IcomCivBackendTestAccess::prepareGeneration(backend, 1);
    IcomCivBackendTestAccess::selectModelAndFrequency(backend, *ic9700, 144'000'000ULL);

    QSignalSpy definitionSpy(&backend, &IRadioBackend::meterDefined);
    QSignalSpy updateSpy(&backend, &IRadioBackend::meterUpdate);
    IcomCivBackendTestAccess::publishMeterDefinitions(backend);
    IcomCivBackendTestAccess::setKeyed(backend, true);

    bool wattsDefinition = false;
    for (const QList<QVariant>& args : definitionSpy) {
        const MeterDef def = qvariant_cast<MeterDef>(args.at(0));
        if (def.source == QStringLiteral("TX") && def.name == QStringLiteral("FWDPWR")) {
            wattsDefinition = def.unit == QStringLiteral("Watts");
        }
    }
    check(wattsDefinition, "IC-9700 publishes derived forward power as Watts above the seam");
    check(!backend.capabilities().forwardPowerRequiresSmoothing,
          "IC-9700 disables duplicate client-side forward-power ballistics");
    check(!backend.capabilities().txPowerBands.isEmpty(),
          "IC-9700 declares the per-band ratings consumed by its power face");

    CivFrame current;
    current.to = kControllerAddress;
    current.from = 0xA2;
    current.cmd = cmd::kMeter;
    current.hasSub = true;
    current.sub = meter::kId;
    current.data = {0x01, 0x21};
    const int beforeCurrent = updateSpy.count();
    IcomCivBackendTestAccess::deliver(backend, current, 1);
    check(updateSpy.count() == beforeCurrent + 1
              && updateSpy.last().at(0).toString() == QStringLiteral("RAD:PACURRENT")
              && std::fabs(updateSpy.last().at(1).toDouble() - 10.0) < 0.001,
          "IC-9700 raw Id 121 crosses the backend seam as 10 A");

    CivFrame po;
    po.to = kControllerAddress;
    po.from = 0xA2;
    po.cmd = cmd::kMeter;
    po.hasSub = true;
    po.sub = meter::kPower;
    po.data = {0x01, 0x43};  // official 50-percent Po breakpoint

    const auto deliverAndRead = [&](std::uint64_t frequencyHz) -> std::optional<double> {
        IcomCivBackendTestAccess::selectModelAndFrequency(backend, *ic9700, frequencyHz);
        const int before = updateSpy.count();
        IcomCivBackendTestAccess::deliver(backend, po, 1);
        if (updateSpy.count() != before + 1) {
            return std::nullopt;
        }
        const QList<QVariant> args = updateSpy.last();
        if (args.at(0).toString() != QStringLiteral("TX:FWDPWR")) {
            return std::nullopt;
        }
        return args.at(1).toDouble();
    };

    const std::optional<double> twoMetres = deliverAndRead(144'000'000ULL);
    const std::optional<double> seventyCentimetres = deliverAndRead(430'000'000ULL);
    const std::optional<double> twentyThreeCentimetres = deliverAndRead(1'240'000'000ULL);
    check(twoMetres && std::fabs(*twoMetres - 50.0) < 0.01,
          "IC-9700 50-percent Po publishes 50 derived W on 2 m");
    check(seventyCentimetres && std::fabs(*seventyCentimetres - 37.5) < 0.01,
          "IC-9700 band transition publishes 37.5 derived W on 70 cm");
    check(twentyThreeCentimetres && std::fabs(*twentyThreeCentimetres - 5.0) < 0.01,
          "IC-9700 band transition publishes 5 derived W on 23 cm");

    const int beforeGap = updateSpy.count();
    IcomCivBackendTestAccess::selectModelAndFrequency(backend, *ic9700, 900'000'000ULL);
    IcomCivBackendTestAccess::deliver(backend, po, 1);
    check(updateSpy.count() == beforeGap,
          "IC-9700 gap frequency publishes no stale prior-deck watt estimate");

    const IcomModel* ic705 = modelForCivAddress(0xA4);
    check(ic705 != nullptr, "cross-radio power fixture resolves the IC-705");
    if (ic705) {
        IcomCivBackendTestAccess::selectModelAndFrequency(backend, *ic705, 14'100'000ULL);
        po.from = 0xA4;
        const int before = updateSpy.count();
        IcomCivBackendTestAccess::deliver(backend, po, 1);
        check(updateSpy.count() == before + 1
                  && std::fabs(updateSpy.last().at(1).toDouble() - 5.0) < 0.01,
              "IC-705 keeps its native 5 W raw-143 calibration");
        check(backend.capabilities().forwardPowerRequiresSmoothing,
              "IC-705 retains established client-side power ballistics");
        const RadioCapabilities caps = backend.capabilities();
        check(caps.txPowerBands.size() == 1
                  && caps.txPowerMaxWattsAt(14'100'000.0) == 10.0,
              "IC-705 uses only its own continuous 10 W rated-output range");
    }

    const IcomModel* ic7300mk2 = modelForCivAddress(0xB6);
    check(ic7300mk2 != nullptr, "cross-radio power fixture resolves the IC-7300MK2");
    if (ic7300mk2) {
        IcomCivBackendTestAccess::selectModelAndFrequency(backend, *ic7300mk2, 14'100'000ULL);
        po.from = 0xB6;
        const int before = updateSpy.count();
        IcomCivBackendTestAccess::deliver(backend, po, 1);
        check(updateSpy.count() == before + 1
                  && std::fabs(updateSpy.last().at(1).toDouble() - 50.0) < 0.01,
              "IC-7300MK2 keeps its native 50 W raw-143 calibration");
        check(backend.capabilities().forwardPowerRequiresSmoothing,
              "IC-7300MK2 retains established client-side power ballistics");
        check(backend.capabilities().txPowerBands.isEmpty(),
              "IC-7300MK2 does not inherit the IC-9700 per-band power ratings");
    }

    IcomCivBackendTestAccess::selectModelAndFrequency(backend, *ic9700, 430'000'000ULL);
    const int beforeClientUnkey = updateSpy.count();
    backend.setKeying(false);
    bool clientUnkeyReset = false;
    for (int i = beforeClientUnkey; i < updateSpy.count(); ++i) {
        const QList<QVariant> args = updateSpy.at(i);
        clientUnkeyReset |= args.at(0).toString() == QStringLiteral("TX:FWDPWR")
            && args.at(1).toDouble() == 0.0;
    }
    check(clientUnkeyReset,
          "client-requested Icom unkey immediately clears derived forward power");

    CivFrame ptt;
    ptt.to = kControllerAddress;
    ptt.from = 0xA2;
    ptt.cmd = cmd::kControl;
    ptt.hasSub = true;
    ptt.sub = control::kPtt;
    ptt.data = {0x00};

    const int beforeUnkey = updateSpy.count();
    IcomCivBackendTestAccess::expectPttConfirmation(backend, false);
    IcomCivBackendTestAccess::deliver(backend, ptt, 1);
    bool resetForwardPower = false;
    for (int i = beforeUnkey; i < updateSpy.count(); ++i) {
        const QList<QVariant> args = updateSpy.at(i);
        resetForwardPower |= args.at(0).toString() == QStringLiteral("TX:FWDPWR")
            && args.at(1).toDouble() == 0.0;
    }
    check(resetForwardPower,
          "authoritative Icom unkey publishes an immediate forward-power reset");

    po.from = 0xA2;
    const int beforeLatePower = updateSpy.count();
    IcomCivBackendTestAccess::deliver(backend, po, 1);
    check(updateSpy.count() == beforeLatePower,
          "late TX-only meter reply cannot repopulate power while unkeyed");
    IcomCivBackendTestAccess::deliver(backend, current, 1);
    check(updateSpy.count() == beforeLatePower,
          "late Id reply cannot repopulate PA current while unkeyed");

    if (ic705) {
        IcomCivBackendTestAccess::selectModelAndFrequency(backend, *ic705, 14'100'000ULL);
        po.from = 0xA4;
        const int before = updateSpy.count();
        IcomCivBackendTestAccess::deliver(backend, po, 1);
        check(updateSpy.count() == before + 1,
              "IC-9700 late-reply guard does not change native-watt Icom timing");
    }

    IcomCivBackendTestAccess::selectModelAndFrequency(backend, *ic9700, 430'000'000ULL);
    po.from = 0xA2;
    ptt.data = {0x01};
    IcomCivBackendTestAccess::expectPttConfirmation(backend, true);
    IcomCivBackendTestAccess::deliver(backend, ptt, 1);
    const int beforeNextTxPower = updateSpy.count();
    IcomCivBackendTestAccess::deliver(backend, po, 1);
    check(updateSpy.count() == beforeNextTxPower + 1
              && std::fabs(updateSpy.last().at(1).toDouble() - 37.5) < 0.01,
          "the next authoritative PTT-ON accepts a fresh IC-9700 power sample");
    const int beforeNextCurrent = updateSpy.count();
    IcomCivBackendTestAccess::deliver(backend, current, 1);
    check(updateSpy.count() == beforeNextCurrent + 1
              && updateSpy.last().at(0).toString() == QStringLiteral("RAD:PACURRENT")
              && std::fabs(updateSpy.last().at(1).toDouble() - 10.0) < 0.001,
          "the next authoritative PTT-ON accepts a fresh IC-9700 Id sample");
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testIc9700DerivedForwardPowerAcrossBands();
    return failures == 0 ? 0 : 1;
}
