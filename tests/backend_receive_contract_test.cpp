// #5890 / #5554 section 2.3: bounded seed for the first M4 receive group.
// No firmware peer, connect to hardware, USB enumeration, or keying. The
// registry distinguishes protocol declarations from desktop implementations.
// Host-state cases do NOT claim that an unconfigured DSP has applied a value.
#include "TestSettingsProfile.h"
#include "SeamThreadAffinityProbe.h"
#include "IcomReceiveContractTestAccess.h"
#include "core/backends/flex/FlexBackend.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/Hl2RxDsp.h"
#include "core/backends/anan/AnanBackend.h"
#include "core/backends/sim/SimBackend.h"
#include "core/backends/sim/SimSignalSource.h"
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

namespace AetherSDR::hl2 {
// Reuse the existing friend to open only a production RX worker/channel.
// No connectRadio(), Metis start, network peer, TX DSP configure or samples.
struct Hl2DspReadbackTestAccess {
    static std::array<double, 3> audio(Hl2Backend& backend)
    {
        const Hl2Backend::Receiver* receiver = backend.rx(0);
        return {double(receiver->audioMuted), receiver->audioGain, double(receiver->audioPanPercent)};
    }
    static bool prepare(Hl2Backend& backend)
    {
        std::string error;
        if (!backend.openReceiverDsp(0, &error)) {
            return false;
        }
        Hl2RxDsp* dsp = backend.rx(0)->dsp;
        bool configured = false;
        QMetaObject::invokeMethod(dsp, [&] {
            configured = dsp->configure(Hl2RxDsp::Config{});
        }, Qt::BlockingQueuedConnection);
        return configured;
    }
    static WdspChannel::Config applied(Hl2Backend& backend)
    {
        Hl2RxDsp* dsp = backend.rx(0)->dsp;
        WdspChannel::Config config;
        // A queue barrier AND a read of the real channel, on its own thread.
        QMetaObject::invokeMethod(dsp, [&] {
            if (const WdspChannel::Config* current = dsp->channelConfig()) {
                config = *current;
            }
        }, Qt::BlockingQueuedConnection);
        return config;
    }
};
}

using namespace AetherSDR;
namespace AetherSDR {
struct SimReceiveContractTestAccess {
    static bool blanker(SimBackend& backend)
    {
        bool enabled = false;
        QMetaObject::invokeMethod(backend.m_signalSource, [&] {
            enabled = backend.m_signalSource->m_audio.noiseBlank();
        }, Qt::BlockingQueuedConnection);
        return enabled;
    }
    static double toneEnergy(SimBackend& backend)
    {
        double energy = 0;
        QMetaObject::invokeMethod(backend.m_signalSource, [&] {
            // The real generator, on its own thread. Settle the notch before
            // measuring, without a clock, sound device or synthetic peer.
            NoiseMixer& mixer = backend.m_signalSource->m_audio;
            for (int frame = 0; frame < 100; ++frame) {
                const QVector<float> samples = mixer.mixFrame();
                if (frame >= 50) {
                    for (float sample : samples) { energy += double(sample) * sample; }
                }
            }
        }, Qt::BlockingQueuedConnection);
        return energy;
    }
};
}
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
// One invocation helper, shared by concrete-family cases. The selected AGC
// field reaches Flex alone; host backends still receive the required pair.
void request(IRadioBackend& backend, Operation operation)
{
    switch (operation) {
    case Operation::Frequency:
        backend.requestSliceTune(0, {14'250'000, SliceTuneRequest::PanIntent::PreservePan});
        break;
    case Operation::Mode: backend.setSliceMode(0, QStringLiteral("LSB")); break;
    case Operation::Filter:
        backend.requestSliceFilter(0, {300, 2700, SliceFilterRequest::Origin::Operator});
        break;
    case Operation::Agc:
        backend.requestSliceAgc(0, {SliceAgcRequest::Field::Mode, QStringLiteral("fast"), 50, 10});
        break;
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
        QStringList{"slice set 0 agc_mode=fast"}};
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

void intentVariants()
{
    FlexBackend flex;
    QStringList commands;
    int genericCommands = 0;
    flex.setSliceCommandSink([&](const QString& command) { commands.append(command); });
    flex.setCommandSink([&](const QString&) { ++genericCommands; });
    flex.requestSliceTune(2, {7'100'000, SliceTuneRequest::PanIntent::AllowRecenter});
    flex.requestSliceFilter(2, {-2700, -100, SliceFilterRequest::Origin::ModeNormalization});
    flex.requestSliceFilter(2, {-2600, -200, SliceFilterRequest::Origin::Adaptive});
    flex.requestSliceAgc(2, {SliceAgcRequest::Field::Threshold, QStringLiteral("slow"), 42, 10});
    flex.requestSliceAgc(2, {SliceAgcRequest::Field::OffLevel, QStringLiteral("fast"), 65, 31});
    check(commands == QStringList{"slice tune 2 7.100000", "filt 2 -2600 -200",
                                  "slice set 2 agc_threshold=42", "slice set 2 agc_off_level=31"}
              && genericCommands == 0,
          "Flex recenter/adaptive/individual AGC fields use guarded sink; mode normalization writes nothing");
    // Retain compatibility for backend-internal callers of the paired method.
    commands.clear();
    flex.setSliceAgc(2, QStringLiteral("fast"), 55);
    check(commands == QStringList{"slice set 2 agc_mode=fast", "slice set 2 agc_threshold=55"},
          "legacy paired Flex AGC remains a deliberate two-field operation");

    icom::IcomCivBackend icom;
    icom::IcomCivBackendTestAccess::prepare(icom);
    icom.requestSliceAgc(0, {SliceAgcRequest::Field::OffLevel, QStringLiteral("fast"), 42, 31});
    check(icom::IcomCivBackendTestAccess::queuedCount(icom) == 0
              && icom::IcomCivBackendTestAccess::dispatchCount(icom) == 0,
          "AGC off-level does not invent an unsupported Icom operation");
    icom.requestSliceAgc(0, {SliceAgcRequest::Field::Mode, QStringLiteral("off"), 42, 31});
    check(icom::IcomCivBackendTestAccess::queuedCount(icom) == 0,
          "new AGC adapter preserves Icom's refusal of AGC off");
}

void receiveControlContracts()
{
    FlexBackend flex;
    QStringList commands;
    int generic = 0;
    flex.setSliceCommandSink([&](const QString& command) { commands.append(command); });
    flex.setCommandSink([&](const QString&) { ++generic; });
    QSignalSpy observations(&flex, &IRadioBackend::sliceChanged);
    const std::array keys{"nb", "nr", "anf", "", "apf", "lms_nr", "speex_nr",
                          "rnnoise", "nrf", "lms_anf", "anft"};
    for (std::size_t i = 0; i < keys.size(); ++i) {
        const auto feature = static_cast<SliceDspRequest::Feature>(i);
        for (const auto field : {SliceDspRequest::Field::Enabled, SliceDspRequest::Field::Level}) {
            commands.clear();
            const bool supported = feature != SliceDspRequest::Feature::Mn
                && (field == SliceDspRequest::Field::Enabled
                    || (feature != SliceDspRequest::Feature::Rnn && feature != SliceDspRequest::Feature::Anft));
            const ReceiveDispatch result = flex.requestSliceDsp(3, {feature, field, true, 47});
            check(result == (supported ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported),
                  "Flex DSP accepts only implemented feature/field combinations");
            const QString key = QString::fromLatin1(keys[i])
                + (field == SliceDspRequest::Field::Level ? QStringLiteral("_level") : QString());
            check(commands == (supported ? QStringList{QStringLiteral("slice set 3 %1=%2").arg(key)
                                    .arg(field == SliceDspRequest::Field::Enabled ? 1 : 47)} : QStringList{}),
                  "Flex DSP preserves exact enable/level encoding without inventing manual notch");
        }
    }
    commands.clear();
    check(!flex.capabilities().receiveAudioControl && !flex.capabilities().hasRadioDialLock,
          "Flex desktop audio and slice lock do not grow daemon or global-lock capabilities");
    flex.requestSliceAudio(3, {SliceAudioRequest::Field::Gain, 37});
    flex.requestSliceAudio(3, {SliceAudioRequest::Field::Mute, 1,
                             SliceAudioRequest::Origin::ExternalReceiveSuppression});
    flex.requestSliceAudio(3, {SliceAudioRequest::Field::Pan, 64});
    flex.requestSliceSquelch(3, {true, 25, true, true});
    flex.requestSliceSquelch(3, {false, 80, false, false});
    flex.requestSliceRxAntenna(3, QStringLiteral("RX_A"));
    flex.requestSliceLock(3, true);
    flex.requestSliceLock(3, false);
    check(commands == QStringList{"slice set 3 audio_level=37", "slice set 3 audio_mute=1",
              "slice set 3 audio_pan=64", "slice set 3 squelch=1", "slice set 3 squelch_level=25",
              "slice set 3 rxant=RX_A", "slice lock 3", "slice unlock 3"} && generic == 0,
          "Flex routes all controls through guarded slice sink; squelch uses separate changed fields");
    check(observations.isEmpty(), "dispatch does not manufacture Flex radio observations");
    flex.decodeSliceStatus(3, {{"audio_level", "29"}, {"audio_mute", "0"}, {"audio_pan", "20"},
        {"nb", "0"}, {"nb_level", "33"}, {"squelch", "0"}, {"squelch_level", "18"},
        {"rxant", "ANT1"}, {"lock", "1"}});
    const SliceDelta truth = last(observations);
    check(truth.audioGain == 29 && truth.audioMute == false && truth.audioPan == 20
              && truth.nb == false && truth.nbLevel == 33 && truth.squelchOn == false
              && truth.squelchLevel == 18 && truth.rxAntenna == QStringLiteral("ANT1")
              && truth.locked == true && commands.size() == 8,
          "independent Flex status corrects every migrated control group without writeback");
    commands.clear();
    check(flex.requestSliceAudio(0, {SliceAudioRequest::Field::Mute, 2}) == ReceiveDispatch::Unsupported
              && flex.requestSliceDsp(0, {SliceDspRequest::Feature::Nb, SliceDspRequest::Field::Level, true, -1}) == ReceiveDispatch::Unsupported
              && flex.requestSliceRxAntenna(0, QStringLiteral("ANT1\nslice set 0 tx=1")) == ReceiveDispatch::Unsupported
              && flex.requestSliceLock(-1, true) == ReceiveDispatch::Unsupported && commands.isEmpty(),
          "invalid receive inputs cannot become Flex commands");

    // Icom acceptance proves scheduler output, not a radio ACK. Paired NB/NR
    // and MN may generate multiple frames; a single intent is not one frame.
    using namespace icom;
    struct IcomCase { SliceDspRequest request; QString first; };
    const std::array icomCases{
        IcomCase{{SliceDspRequest::Feature::Nb, SliceDspRequest::Field::Enabled, true, 45}, "fe fe a4 e0 16 22 01 fd"},
        IcomCase{{SliceDspRequest::Feature::Nr, SliceDspRequest::Field::Level, true, 45}, "fe fe a4 e0 16 40 01 fd"},
        IcomCase{{SliceDspRequest::Feature::Anf, SliceDspRequest::Field::Enabled, true, 45}, "fe fe a4 e0 16 41 01 fd"},
        IcomCase{{SliceDspRequest::Feature::Mn, SliceDspRequest::Field::Level, false, 45}, "fe fe a4 e0 16 48 00 fd"}};
    for (const IcomCase& test : icomCases) {
        IcomCivBackend backend;
        IcomCivBackendTestAccess::prepare(backend);
        check(backend.requestSliceDsp(0, test.request) == ReceiveDispatch::Dispatched,
              "Icom accepts existing paired DSP operations");
        IcomCivBackendTestAccess::pump(backend);
        check(IcomCivBackendTestAccess::firstDispatched(backend) == test.first,
              "typed Icom DSP dispatch reaches the documented CI-V function");
    }
    for (int operation = 0; operation < 3; ++operation) {
        IcomCivBackend backend;
        IcomCivBackendTestAccess::prepare(backend);
        const ReceiveDispatch result = operation == 0
            ? backend.requestSliceAudio(0, {SliceAudioRequest::Field::Gain, 40})
            : operation == 1 ? backend.requestSliceSquelch(0, {false, 76, true, false})
                             : backend.requestSliceLock(0, true);
        IcomCivBackendTestAccess::pump(backend);
        const std::array expected{QStringLiteral("fe fe a4 e0 14 01 01 02 fd"),
            QStringLiteral("fe fe a4 e0 14 03 00 00 fd"), QStringLiteral("fe fe a4 e0 16 50 01 fd")};
        check(result == ReceiveDispatch::Dispatched
                  && IcomCivBackendTestAccess::firstDispatched(backend) == expected[operation],
              "Icom AF gain, squelch-off threshold and global lock retain native encoding");
    }
    {
        IcomCivBackend backend;
        IcomCivBackendTestAccess::prepare(backend);
        check(backend.requestSliceRxAntenna(0, QStringLiteral("RX-ANT")) == ReceiveDispatch::Unsupported,
              "Icom profile without a selectable RX antenna refuses the intent");
        // Select the evidenced MK2 dialect. The unstarted session deliberately
        // retains the fixture's A4 address; dialect and destination are separate.
        IcomCivBackendTestAccess::selectModel(backend, *modelForId(0xB6));
        QSignalSpy observations(&backend, &IRadioBackend::sliceChanged);
        check(backend.requestSliceRxAntenna(0, QStringLiteral("RX-ANT")) == ReceiveDispatch::Dispatched,
              "Icom selectable RX antenna accepts the canonical port name");
        IcomCivBackendTestAccess::pump(backend);
        check(IcomCivBackendTestAccess::firstDispatched(backend) == QStringLiteral("fe fe a4 e0 12 00 01 fd")
                  && observations.isEmpty(),
              "Icom RX antenna retains native encoding without manufacturing readback");
        const std::uint64_t queued = IcomCivBackendTestAccess::queuedCount(backend);
        check(backend.requestSliceRxAntenna(0, QStringLiteral("ANT2")) == ReceiveDispatch::Unsupported
                  && backend.requestSliceRxAntenna(1, QStringLiteral("ANT1")) == ReceiveDispatch::Unsupported
                  && IcomCivBackendTestAccess::queuedCount(backend) == queued,
              "unsupported Icom ports and slice identities enqueue no RX antenna command");
        CivFrame antenna;
        antenna.cmd = 0x12;
        antenna.hasSub = true;
        antenna.sub = 0;
        antenna.data = {0};
        IcomCivBackendTestAccess::observe(backend, antenna);
        check(last(observations).rxAntenna == QStringLiteral("ANT1")
                  && IcomCivBackendTestAccess::queuedCount(backend) == queued,
              "independent Icom antenna readback corrects the requested port without echo");
    }
    for (const Family& family : kFamilies) {
        if (!family.make) { continue; }
        std::unique_ptr<IRadioBackend> backend = family.make();
        if (QLatin1String(family.name) != QLatin1String("flex")) {
            check(backend->requestSliceDsp(0, {SliceDspRequest::Feature::Apf, SliceDspRequest::Field::Level, true, 40})
                      == ReceiveDispatch::Unsupported,
                  "non-Flex APF request refuses explicitly without inventing a backend feature");
        }
        if (QLatin1String(family.name) == QLatin1String("sim")
            || QLatin1String(family.name) == QLatin1String("anan")) {
            check(backend->requestSliceAudio(0, {SliceAudioRequest::Field::Gain, 40}) == ReceiveDispatch::Unsupported
                      && backend->requestSliceLock(0, true) == ReceiveDispatch::LocalOnly,
                  "no independent mixer is invented; client-only slice lock remains available");
        }
        if (QLatin1String(family.name) == QLatin1String("icom")) {
            check(backend->requestSliceAudio(0, {SliceAudioRequest::Field::Gain, 40}) == ReceiveDispatch::Unsupported
                      && backend->requestSliceDsp(0, {SliceDspRequest::Feature::Nb, SliceDspRequest::Field::Enabled, true, 50})
                          == ReceiveDispatch::Unsupported
                      && backend->requestSliceSquelch(0, {true, 40, true, true}) == ReceiveDispatch::Unsupported,
                  "cold Icom cannot claim a dispatch without a connected session/profile");
        }
    }
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
        backend->requestSliceFilter(0, {-2500, -200, SliceFilterRequest::Origin::Operator});
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
    for (NoiseMixer::Channel channel : NoiseMixer::allChannels()) {
        backend.setDemoNoiseEnabled(NoiseMixer::name(channel), false);
    }
    backend.setDemoNoiseEnabled(QStringLiteral("birdie"), true);
    backend.setDemoNoiseKnob(QStringLiteral("birdie"), QStringLiteral("hz"), 1200);
    backend.setDemoNoiseLevel(QStringLiteral("birdie"), -12);
    check(backend.requestSliceDsp(0, {SliceDspRequest::Feature::Nb, SliceDspRequest::Field::Enabled, true, 50})
              == ReceiveDispatch::Dispatched && SimReceiveContractTestAccess::blanker(backend),
          "Demo typed NB reaches the real signal worker's blanker state");
    backend.requestSliceDsp(0, {SliceDspRequest::Feature::Nb, SliceDspRequest::Field::Enabled, false, 50});
    check(!SimReceiveContractTestAccess::blanker(backend), "Demo disabling NB reaches the worker too");
    const double beforeNotch = SimReceiveContractTestAccess::toneEnergy(backend);
    check(backend.requestSliceDsp(0, {SliceDspRequest::Feature::Anf, SliceDspRequest::Field::Enabled, true, 50})
              == ReceiveDispatch::Dispatched && last(observations).anf == true,
          "Demo typed ANF retains its supported synthetic observation");
    const double afterNotch = SimReceiveContractTestAccess::toneEnergy(backend);
    check(beforeNotch > 1 && afterNotch < beforeNotch * 0.1,
          "Demo ANF changes actual generator samples, not only a copied status flag");
    backend.requestSliceDsp(0, {SliceDspRequest::Feature::Anf, SliceDspRequest::Field::Enabled, false, 50});
    check(SimReceiveContractTestAccess::toneEnergy(backend) > beforeNotch * 0.9,
          "Demo disabling ANF restores the real generator tone");
    backend.disconnectRadio();
    observations.clear();
    check(backend.requestSliceDsp(0, {SliceDspRequest::Feature::Nb, SliceDspRequest::Field::Enabled, false, 50})
              == ReceiveDispatch::Unsupported,
          "Demo refuses typed DSP requests after disconnect");
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

void hl2WorkerDispatch()
{
    hl2::Hl2Backend backend;
    const bool prepared = hl2::Hl2DspReadbackTestAccess::prepare(backend);
    check(prepared, "socket-free fixture opens only the production HL2 receive DSP worker");
    if (!prepared) { return; }
    backend.setSliceMode(0, QStringLiteral("LSB"));
    WdspChannel::Config applied = hl2::Hl2DspReadbackTestAccess::applied(backend);
    check(applied.mode == WdspChannel::Mode::Lsb
              && applied.filterLowHz == -2900 && applied.filterHighHz == -100,
          "HL2 mode then default-passband reaches the actual worker in order");
    backend.requestSliceFilter(0, {-2400, -200, SliceFilterRequest::Origin::Operator});
    backend.setSliceMode(0, QStringLiteral("LSB"));
    applied = hl2::Hl2DspReadbackTestAccess::applied(backend);
    check(applied.filterLowHz == -2400 && applied.filterHighHz == -200,
          "repeated HL2 mode re-push preserves the manual passband in the worker");
    backend.setSliceMode(0, QStringLiteral("CW"));
    backend.requestSliceFilter(0, {-200, 200, SliceFilterRequest::Origin::Adaptive});
    applied = hl2::Hl2DspReadbackTestAccess::applied(backend);
    check(applied.filterLowHz == 400 && applied.filterHighHz == 800,
          "typed adaptive filter retains HL2 carrier-to-CW-pitch translation in the worker");
    backend.requestSliceAgc(0, {SliceAgcRequest::Field::Mode, QStringLiteral("fast"), 50, 10});
    applied = hl2::Hl2DspReadbackTestAccess::applied(backend);
    check(applied.agcMode == 4 && std::abs(applied.maximumAgcGainDb - 30.0) < 1e-9,
          "typed AGC pair configures the actual worker's mode and gain ceiling");
    backend.requestSliceAgc(0, {SliceAgcRequest::Field::Threshold, QStringLiteral("fast"), 40, 10});
    backend.requestSliceAgc(0, {SliceAgcRequest::Field::OffLevel, QStringLiteral("off"), 100, 90});
    applied = hl2::Hl2DspReadbackTestAccess::applied(backend);
    check(applied.agcMode == 4 && std::abs(applied.maximumAgcGainDb - 24.0) < 1e-9,
          "threshold preserves fast AGC; unsupported off-level cannot alter the worker");
    check(backend.requestSliceDsp(0, {SliceDspRequest::Feature::Nb, SliceDspRequest::Field::Level, true, 71})
              == ReceiveDispatch::Dispatched, "HL2 accepts its implemented blanker only");
    applied = hl2::Hl2DspReadbackTestAccess::applied(backend);
    check(applied.noiseBlankerEnabled && applied.noiseBlankerLevel == 71,
          "HL2 typed blanker configures the actual DSP worker");
    backend.requestSliceAudio(0, {SliceAudioRequest::Field::Gain, 43});
    backend.requestSliceAudio(0, {SliceAudioRequest::Field::Mute, 1});
    backend.requestSliceAudio(0, {SliceAudioRequest::Field::Pan, 77});
    const auto audio = hl2::Hl2DspReadbackTestAccess::audio(backend);
    check(audio[0] == 1 && std::abs(audio[1] - 0.43) < 1e-6 && audio[2] == 77,
          "HL2 audio adapters configure the existing receiver mixer fields");
}
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("backend-receive-contract"));
    if (!profile.isValid()) {
        return 1;
    }
    QCoreApplication app(argc, argv);
    qputenv("AETHER_AUTOMATION", "1");
    qunsetenv("AETHER_AUTOMATION_ALLOW_TX");
    declarations();
    flexCommandsAndObservations();
    intentVariants();
    receiveControlContracts();
    icomCommandsAndObservations();
    hostConfiguration();
    hl2WorkerDispatch();
    demoAndColdRefusal();
    return failures == 0 ? 0 : 1;
}
