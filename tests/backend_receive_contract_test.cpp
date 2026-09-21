// #5890 / #5554 section 2.3: bounded seed for the first M4 receive group.
// No firmware peer, connect to hardware, USB enumeration, or keying. The
// registry distinguishes protocol declarations from desktop implementations.
// Host-state cases do NOT claim that an unconfigured DSP has applied a value.
#include "TestSettingsProfile.h"
#include "SeamThreadAffinityProbe.h"
#include "IcomReceiveContractTestAccess.h"
#include "core/backends/flex/FlexBackend.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/anan/AnanBackend.h"
#include "core/backends/sim/SimBackend.h"
#ifdef AETHER_BACKEND_RTL
#include "core/backends/rtl/RtlSdrBackend.h"
#endif

#include <QCoreApplication>
#include <QEvent>
#include <QSignalSpy>
#include <array>
#include <cmath>
#include <cstdio>
#include <memory>

using namespace AetherSDR;
namespace {
int failures = 0;
void check(bool condition, const char* description)
{
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", description);
    failures += !condition;
}

enum class Operation { Frequency, Mode, Filter, Agc };
constexpr std::array kOperations{Operation::Frequency, Operation::Mode,
                                 Operation::Filter, Operation::Agc};
// One invocation helper, shared by concrete-family cases. AGC is the legacy
// pair contract here; M4 must deliberately revise it before desktop wiring.
void request(IRadioBackend& backend, Operation operation)
{
    switch (operation) {
    case Operation::Frequency: backend.setSliceFrequency(0, 14'250'000); break;
    case Operation::Mode: backend.setSliceMode(0, QStringLiteral("LSB")); break;
    case Operation::Filter: backend.setSliceFilter(0, 300, 2700); break;
    case Operation::Agc: backend.setSliceAgc(0, QStringLiteral("fast"), 50); break;
    }
}

template<class T> std::unique_ptr<IRadioBackend> make() { return std::make_unique<T>(); }
struct Family {
    const char* name;
    std::unique_ptr<IRadioBackend> (*make)();
    bool daemonMode;
    bool daemonFilter;
    const char* coverage;
};
const std::array kFamilies{
    Family{"flex", make<FlexBackend>, true, true, "slice sink + independent status decode"},
    Family{"hl2", make<hl2::Hl2Backend>, true, true, "pre-connect receiver configuration (not DSP completion)"},
    Family{"icom", make<icom::IcomCivBackend>, false, false, "scheduled CI-V + independent frame decode"},
    Family{"anan", make<anan::AnanBackend>, false, false, "pre-connect receiver configuration (not DSP completion)"},
    Family{"sim", make<SimBackend>, true, false, "production Demo session state (not hardware/filter DSP)"},
#ifdef AETHER_BACKEND_RTL
    Family{"rtl", make<rtl::RtlSdrBackend>, true, false, "cold refusal only; USB/DDC dispatch not covered"},
#else
    Family{"rtl", nullptr, true, false, "NOT BUILT: optional librtlsdr unavailable"},
#endif
};

SliceDelta last(const QSignalSpy& observations)
{
    return observations.isEmpty() ? SliceDelta{} : qvariant_cast<SliceDelta>(observations.last().at(1));
}

void declarations()
{
    for (const Family& family : kFamilies) {
        std::printf("COVERAGE %s: %s\n", family.name, family.coverage);
        if (!family.make) {
            continue; // Visible omission, never a silently successful RTL case.
        }
        const std::unique_ptr<IRadioBackend> backend = family.make();
        const RadioCapabilities caps = backend->capabilities();
        check(caps.family == QLatin1String(family.name), "registry resolves the concrete family");
        check(caps.receiveModeControl.has_value() == family.daemonMode
                  && caps.receiveFilterControl.has_value() == family.daemonFilter,
              "daemon capability declarations are independent of desktop verb support");
        check(!backend->isConnected(), "constructing the contract fixture never connects hardware");
    }
}

void flexCommandsAndObservations()
{
    // FlexLib 4.2.18 Slice.cs: tune MHz/f6, mode, filter cuts, independent AGC
    // writes. This pins current seam behavior, not new desktop routing.
    const std::array<QStringList, 4> expected{
        QStringList{"slice tune 0 14.250000 autopan=0"},
        QStringList{"slice set 0 mode=LSB"}, QStringList{"filt 0 300 2700"},
        QStringList{"slice set 0 agc_mode=fast", "slice set 0 agc_threshold=50"}};
    for (std::size_t i = 0; i < kOperations.size(); ++i) {
        FlexBackend backend;
        QStringList commands;
        int genericCommands = 0;
        backend.setSliceCommandSink([&](const QString& command) { commands.append(command); });
        backend.setCommandSink([&](const QString&) { ++genericCommands; });
        QSignalSpy observations(&backend, &IRadioBackend::sliceChanged);
        test::SeamThreadAffinityProbe affinity(&backend);
        test::attachAllSeamSignals(affinity);
        request(backend, kOperations[i]);
        check(commands == expected[i] && genericCommands == 0,
              "Flex receive intent uses only the guarded slice sink, with exact legacy encoding");
        check(observations.isEmpty(), "Flex dispatch does not manufacture observed state");
        // Intentionally differs from every request. A client cannot mistake
        // acceptance for confirmation or overwrite an authoritative answer.
        backend.decodeSliceStatus(0, {{"RF_frequency", "14.260000"}, {"mode", "USB"},
            {"filter_lo", "100"}, {"filter_hi", "2900"},
            {"agc_mode", "slow"}, {"agc_threshold", "65"}});
        const SliceDelta observed = last(observations);
        check(observed.frequency == 14.26 && observed.mode == QStringLiteral("USB")
                  && observed.filterLow == 100 && observed.filterHigh == 2900
                  && observed.agcMode == QStringLiteral("slow") && observed.agcThreshold == 65,
              "Flex independently decoded observation wins over all requested values");
        check(commands == expected[i], "decoding receive state does not redispatch the request");
        check(affinity.violations().isEmpty(), "driven Flex seam signals stay on the owner thread");
    }
}

void icomCommandsAndObservations()
{
    using namespace icom;
    // IC-705 CI-V reference command families: 05 frequency, 26 mode/data,
    // 1A 03 IF width, 16 12 AGC. Filter also queues its PBT pair and readbacks.
    const std::array<QString, 4> expected{
        "fe fe a4 e0 05 00 00 25 14 00 fd", "fe fe a4 e0 26 00 00 00 01 fd",
        "fe fe a4 e0 1a 03 28 fd", "fe fe a4 e0 16 12 01 fd"};
    for (std::size_t i = 0; i < kOperations.size(); ++i) {
        IcomCivBackend backend;
        IcomCivBackendTestAccess::prepare(backend);
        QSignalSpy observations(&backend, &IRadioBackend::sliceChanged);
        test::SeamThreadAffinityProbe affinity(&backend);
        test::attachAllSeamSignals(affinity);
        request(backend, kOperations[i]);
        IcomCivBackendTestAccess::pump(backend);
        check(IcomCivBackendTestAccess::firstDispatched(backend) == expected[i],
              "Icom desktop receive intent enters the production paced CI-V scheduler");
        if (kOperations[i] == Operation::Frequency || kOperations[i] == Operation::Agc) {
            check(observations.isEmpty(), "Icom frequency/AGC requests are not observations");
        }
        // Mode/filter currently publish an optimistic desktop projection;
        // do not rewrite that established contract as part of a test seed.
        observations.clear();
        CivFrame frequency;
        frequency.cmd = 0x03;
        frequency.data = {0x00, 0x00, 0x26, 0x14, 0x00}; // 14.260 MHz, BCD LSB first
        IcomCivBackendTestAccess::observe(backend, frequency, 0);
        check(observations.isEmpty(), "obsolete Icom session observations are rejected");
        IcomCivBackendTestAccess::observe(backend, frequency);
        check(last(observations).frequency == 14.26,
              "independent Icom readback publishes the radio value, not requested frequency");
        CivFrame mode;
        mode.cmd = 0x26; // 04 cannot confirm the DATA flag on a profiled IC-705.
        mode.hasSub = true;
        mode.sub = 0x00;
        mode.data = {0x01, 0x00, 0x02}; // USB, DATA off, FIL2; not requested LSB.
        observations.clear();
        IcomCivBackendTestAccess::observe(backend, mode);
        bool sawMode = false;
        for (const QList<QVariant>& event : observations) {
            sawMode |= qvariant_cast<SliceDelta>(event.at(1)).mode == QStringLiteral("USB");
        }
        check(sawMode,
              "Icom radio mode corrects the optimistic desktop projection");
        CivFrame filter;
        filter.cmd = 0x1a;
        filter.hasSub = true;
        filter.sub = 0x03;
        filter.data = {0x20}; // SSB width code 20 BCD = 1600 Hz.
        IcomCivBackendTestAccess::observe(backend, filter);
        const SliceDelta passband = last(observations);
        check(passband.filterLow && passband.filterHigh
                  && *passband.filterHigh - *passband.filterLow == 1600,
              "Icom radio width replaces the requested desktop passband width");
        CivFrame agc;
        agc.cmd = 0x16;
        agc.hasSub = true;
        agc.sub = 0x12;
        agc.data = {0x03}; // Slow, not requested fast.
        IcomCivBackendTestAccess::observe(backend, agc);
        check(last(observations).agcMode == QStringLiteral("slow"),
              "Icom AGC readback publishes the radio selection");
        check(affinity.violations().isEmpty(), "driven Icom seam signals stay on the owner thread");
    }
    // A suspended/slow test may resume after the outstanding write expires.
    // Advance only the scheduler's supplied time; no sleeping or radio peer.
    for (std::size_t i = 0; i < kOperations.size(); ++i) {
        IcomCivBackend delayed;
        IcomCivBackendTestAccess::prepare(delayed);
        request(delayed, kOperations[i]);
        IcomCivBackendTestAccess::pump(delayed);
        check(IcomCivBackendTestAccess::dispatchCount(delayed) > 0,
              "Icom timeout fixture first dispatches the requested write");
        IcomCivBackendTestAccess::expireReply(delayed);
        // A sufficiently slow request/pump may already have sent the follow-up.
        check(IcomCivBackendTestAccess::dispatchCount(delayed) >= 2,
              "Icom timeout fixture also dispatches a queued follow-up");
        check(IcomCivBackendTestAccess::firstDispatched(delayed) == expected[i],
              "Icom first-dispatch proof survives a later reply timeout");
    }
    IcomCivBackend backend;
    IcomCivBackendTestAccess::prepare(backend);
    backend.setSliceMode(0, QStringLiteral("SAM"));
    backend.setSliceAgc(0, QStringLiteral("off"), 50);
    check(IcomCivBackendTestAccess::queuedCount(backend) == 0
              && IcomCivBackendTestAccess::firstDispatched(backend).isEmpty(),
          "unsupported Icom SAM/AGC-off queue or dispatch no CI-V command");
}

void hostConfiguration()
{
    // These cold backends have configuration state but no configured receive
    // DSP. Pin the state path without mislabelling its echo as DSP readback.
    for (const Family& family : kFamilies) {
        if (QLatin1String(family.name) != QLatin1String("hl2")
            && QLatin1String(family.name) != QLatin1String("anan")) {
            continue;
        }
        std::unique_ptr<IRadioBackend> backend = family.make();
        QSignalSpy observations(backend.get(), &IRadioBackend::sliceChanged);
        test::SeamThreadAffinityProbe affinity(backend.get());
        test::attachAllSeamSignals(affinity);
        request(*backend, Operation::Frequency);
        check(last(observations).frequency == 14.25, "host backend stores the requested receive frequency");
        request(*backend, Operation::Mode);
        check(last(observations).mode == QStringLiteral("LSB") && last(observations).filterLow == -2900,
              "host mode change adopts its default passband");
        backend->setSliceFilter(0, -2500, -200);
        check(last(observations).filterLow == -2500 && last(observations).filterHigh == -200,
              "host filter request updates receiver configuration");
        request(*backend, Operation::Mode);
        check(last(observations).filterLow == -2500 && last(observations).filterHigh == -200,
              "repeated host mode request preserves the manual filter");
        request(*backend, Operation::Agc);
        if (QLatin1String(family.name) == QLatin1String("hl2")) {
            check(last(observations).agcMode == QStringLiteral("fast") && last(observations).agcThreshold == 50,
                  "HL2 publishes its accepted AGC configuration");
        } else {
            const auto& anan = static_cast<const anan::AnanBackend&>(*backend);
            check(anan.agcModeForTest() == 4 && std::abs(anan.agcCeilingDbForTest() - 30.0) < 1e-9,
                  "ANAN retains WDSP AGC configuration without claiming a radio readback");
        }
        check(affinity.violations().isEmpty(), "driven host backend signals stay on the owner thread");
    }
}

void demoAndColdRefusal()
{
    SimBackend backend;
    QSignalSpy observations(&backend, &IRadioBackend::sliceChanged);
    for (Operation op : kOperations) {
        request(backend, op);
    }
    check(observations.isEmpty(), "disconnected Demo refuses the whole receive group");
    test::SeamThreadAffinityProbe affinity(&backend);
    test::attachAllSeamSignals(affinity);
    backend.connectRadio({}); // Own production synthetic source, not fake radio firmware.
    observations.clear();
    request(backend, Operation::Frequency);
    check(last(observations).frequency == 14.25, "Demo accepts tuning in its synthetic session");
    request(backend, Operation::Mode);
    check(last(observations).mode == QStringLiteral("LSB"), "Demo accepts sideband in its synthetic session");
    request(backend, Operation::Filter);
    check(last(observations).filterLow == 300 && last(observations).filterHigh == 2700,
          "Demo stores filter state without advertising a daemon DSP filter verb");
    request(backend, Operation::Agc);
    check(last(observations).agcMode == QStringLiteral("fast") && last(observations).agcThreshold == 50,
          "Demo AGC is explicitly synthetic state, not hardware gain control");
    backend.disconnectRadio();
    observations.clear();
    for (Operation op : kOperations) {
        request(backend, op);
    }
    QCoreApplication::sendPostedEvents(&backend, QEvent::MetaCall);
    check(observations.isEmpty() && affinity.afterDisconnect().isEmpty(),
          "Demo receive requests and queued deliveries publish nothing after disconnect");
    check(affinity.violations().isEmpty(), "driven Demo seam signals stay on the owner thread");
#ifdef AETHER_BACKEND_RTL
    rtl::RtlSdrBackend rtl;
    QSignalSpy rtlObservations(&rtl, &IRadioBackend::sliceChanged);
    for (Operation op : kOperations) {
        request(rtl, op);
    }
    check(rtlObservations.isEmpty(), "cold RTL refuses receive requests without opening USB");
#endif
}
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("backend-receive-contract"));
    if (!profile.isValid()) {
        return 1;
    }
    QCoreApplication app(argc, argv);
    declarations();
    flexCommandsAndObservations();
    icomCommandsAndObservations();
    hostConfiguration();
    demoAndColdRefusal();
    return failures == 0 ? 0 : 1;
}
