// Focused recovery of PR #5436's deterministic control coverage after the
// broader radio_capability_gating_test retirement (#5452, recovery #5443).
// No connectRadio(), event-loop waits, sockets or firmware peer: the session
// is unstarted and frames/state are injected through the existing test seam.
#include "core/backends/icom/IcomCivBackend.h"
#include "core/backends/icom/IcomSession.h"
#include "core/backends/flex/FlexBackend.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "TestSettingsProfile.h"
#include "TxTestAuthority.h"

#include <QCoreApplication>
#include <algorithm>
#include <cstdio>

using namespace AetherSDR;

namespace AetherSDR::icom {
struct IcomCivBackendTestAccess {
    static void selectModel(IcomCivBackend& backend, const IcomModel& model)
    {
        backend.m_model = &model;
    }

    static void deliverCwPitch(IcomCivBackend& backend, int raw)
    {
        backend.m_connected = true;
        backend.m_sessionGeneration = 1;
        CivFrame frame;
        frame.cmd = 0x14;
        frame.hasSub = true;
        frame.sub = 0x09;
        frame.data = {static_cast<std::uint8_t>(raw / 100),
                      static_cast<std::uint8_t>(((raw % 100) / 10) * 16 + raw % 10)};
        backend.onCivFrame(frame, 1);
    }

    static void deliverDataBandwidth(IcomCivBackend& backend, int item, int packed)
    {
        backend.m_connected = true;
        backend.m_sessionGeneration = 1;
        backend.m_dataMode = true;
        CivFrame frame;
        frame.cmd = 0x1a;
        frame.hasSub = true;
        frame.sub = 0x05;
        frame.data = {0, static_cast<std::uint8_t>((item / 10) * 16 + item % 10),
                      static_cast<std::uint8_t>(packed)};
        backend.onCivFrame(frame, 1);
    }

    static bool queuesNativeControlPolls(IcomCivBackend& backend)
    {
        backend.m_controlPollPhase = 2;
        backend.m_dataMode = true;
        backend.onLinkTick();
        const auto queued = [&](const std::vector<std::uint8_t>& frame) {
            return std::any_of(backend.m_civScheduler.m_queue.begin(),
                               backend.m_civScheduler.m_queue.end(),
                               [&](const auto& request) { return request.request.frame == frame; });
        };
        const std::uint8_t address = backend.m_session->civAddress();
        return queued(cmdReadLevel(address, level::kSquelch))
            && queued(cmdReadLevel(address, level::kCwPitch))
            && queued(cmdReadLevel(address, level::kKeySpeed))
            && queued(cmdReadFunction(address, func::kTxBandwidth))
            && queued(cmdReadSetting(address, 17));
    }

    static void selectCwMode(IcomCivBackend& backend, bool reverse)
    {
        backend.m_mode = reverse ? CivMode::CwR : CivMode::Cw;
        backend.m_dataMode = false;
    }

    static bool tuning(const IcomCivBackend& backend) { return backend.m_tuning; }
    static int power(const IcomCivBackend& backend) { return backend.m_txPowerPercent; }

    static void prepareSession(IcomCivBackend& backend,
                               const IcomModel& model)
    {
        backend.m_model = &model;
        backend.m_connected = true;
        backend.m_session = std::make_unique<IcomSession>();
        backend.m_session->setCivAddress(model.civAddress);
        // Routine polling waits for a VERIFIED CI-V identity (#5164), so a
        // fixture that only sets m_model would see onLinkTick() return before
        // queueing anything. A real session reaches this state by way of a
        // 19 00 reply, which is what these two fields record; the table's
        // civAddress is that model's factory-default address and model ID.
        backend.m_civReported = model.civAddress;
        backend.m_civModelId = model.civAddress;
    }

    static void antennaReply(IcomCivBackend& b, std::uint8_t sub,
                             std::vector<std::uint8_t> data, std::uint64_t generation = 1)
    {
        b.m_sessionGeneration = 1;
        CivFrame frame;
        frame.cmd = cmd::kRxAntenna;
        frame.hasSub = true;
        frame.sub = sub;
        frame.data = std::move(data);
        b.onCivFrame(frame, generation);
    }
    static bool antennaReads(IcomCivBackend& b, bool startup)
    {
        b.m_controlPollPhase = 2; // next tick is the three-second controls group
        if (startup) { b.sendConnectReadBurst(); }
        else { b.onLinkTick(); }
        const auto read = cmdReadRxAntenna(b.m_session->civAddress());
        return std::any_of(b.m_civScheduler.m_queue.begin(), b.m_civScheduler.m_queue.end(),
            [&](const auto& request) { return request.request.frame == read; });
    }
    static bool antennaReplyCompletesRead(IcomCivBackend& b)
    {
        b.queueRead(cmdReadRxAntenna(b.m_session->civAddress()), "rx.antenna",
                    IcomCivScheduler::Priority::Control);
        const auto now = b.m_civScheduler.m_queue.front().enqueuedAtMs;
        if (!b.m_civScheduler.takeNext(now)) { return false; }
        CivFrame reply;
        reply.cmd = cmd::kRxAntenna;
        reply.hasSub = true;
        reply.sub = 0;
        reply.data = {1};
        return b.m_civScheduler.observe(reply, now + 5) == IcomCivScheduler::Observation::Accepted
            && !b.m_civScheduler.stats().readInFlight
            && b.m_civScheduler.stats().timeouts == 0;
    }

    static bool antennaConfirmation(IcomCivBackend& b)
    {
        const auto write = cmdSetRxAntenna(b.m_session->civAddress(), true);
        const auto read = cmdReadRxAntenna(b.m_session->civAddress());
        return b.confirmationFor(write) == read && b.semanticKey(write) == b.semanticKey(read);
    }

    static QString lastOutboundCiv(const IcomCivBackend& backend)
    {
        return backend.m_lastOutboundCiv;
    }

    static void dispatchReady(IcomCivBackend& backend)
    {
        // sendUserCommand samples its pump time before enqueue samples its own
        // deadline. Crossing a millisecond leaves the write for the next tick.
        // Drive that tick explicitly; this fixture never runs the event loop.
        backend.pumpCiv(backend.nowMs());
    }

    static std::size_t queuedRequestCount(const IcomCivBackend& backend)
    {
        return backend.m_civScheduler.m_queue.size();
    }
};
} // namespace AetherSDR::icom

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

int main(int argc, char** argv)
{
    TestSettingsProfile settings(QStringLiteral("icom_control_profile_test"));
    if (!settings.isValid()) {
        return 1;
    }
    QCoreApplication app(argc, argv);
    using namespace AetherSDR::icom;
    {
        FlexBackend flex;
        const RadioCapabilities caps = flex.capabilities();
        check(caps.canCreateSlices, "Flex retains independent ordinary slice creation");
        check(caps.hasAgcThreshold && caps.hasAmCarrierLevel && caps.hasVoxDelay,
              "Flex retains AGC threshold, AM carrier, and VOX delay");
        check(!caps.hasModeIndependentSquelch, "Flex retains its mode-specific SQL policy");
        check(caps.cwSpeedMinWpm == 5 && caps.cwSpeedMaxWpm == 100
                  && caps.cwPitchMinHz == 100 && caps.cwPitchMaxHz == 6000
                  && caps.cwPitchStepHz == 10,
              "Flex retains its existing CW control ranges");
        hl2::Hl2Backend hl2Backend;
        check(!hl2Backend.capabilities().canCreateSlices,
              "HL2 paired receiver/pan topology does not expose independent creation");
        check(hl2Backend.capabilities().hasAgcThreshold, "HL2 retains host AGC threshold");
    }
    {
        const IcomModel* ic705 = modelForName("IC-705");
        check(ic705 != nullptr, "the IC-705 resolves from the Icom model table");
        if (ic705) {
            int pitch = -1;
            IcomCivBackend backend;
            IcomCivBackendTestAccess::selectModel(backend, *ic705);
            const RadioCapabilities caps = backend.capabilities();
            check(!caps.canCreateSlices, "Icom fixed receivers do not expose independent slice creation");
            check(caps.txPowerBands.size() == 1
                      && caps.txPowerMaxWattsAt(14'200'000.0) == 10.0,
                  "IC-705 alone declares its continuous 10 W rated-output range");
            QObject::connect(&backend, &IRadioBackend::transmitChanged,
                             [&pitch](const TransmitDelta& delta) {
                if (delta.cwPitch) {
                    pitch = *delta.cwPitch;
                }
            });
            IcomCivBackendTestAccess::deliverCwPitch(backend, 128);
            check(pitch == 601 && caps.cwPitchStepHz == 10,
                  "IC-705 retains its existing pitch conversion and step");
            check(!caps.hasModeIndependentSquelch, "IC-705 SQL policy remains unchanged");
            check(caps.hasFmRepeaterOffset, "IC-705 retains native repeater offsets");
            check(caps.hasCwTune, "IC-705 CW Tune policy remains unchanged");
        }

        const IcomModel* ic7300Mk2 = modelForName("IC-7300MK2");
        check(ic7300Mk2 != nullptr,
              "the IC-7300MK2 resolves from the Icom model table");
        if (ic7300Mk2) {
            int pitch = -1;
            IcomCivBackend backend;
            IcomCivBackendTestAccess::selectModel(backend, *ic7300Mk2);
            const RadioCapabilities caps = backend.capabilities();
            check(caps.txPowerBands.isEmpty()
                      && caps.txPowerMaxWattsAt(14'200'000.0) == 100.0,
                  "IC-7300MK2 retains its unbanded 100 W capability path");
            check(!caps.hasAgcThreshold && !caps.hasAmCarrierLevel && !caps.hasVoxDelay,
                  "IC-7300MK2 declares unimplemented controls unavailable");
            IcomCivBackendTestAccess::prepareSession(backend, *ic7300Mk2);
            const auto queued = IcomCivBackendTestAccess::queuedRequestCount(backend);
            backend.setSliceAgc(0, QStringLiteral("off"), 0);
            check(!caps.agcModes.contains("off")
                      && IcomCivBackendTestAccess::queuedRequestCount(backend) == queued
                      && IcomCivBackendTestAccess::lastOutboundCiv(backend).isEmpty(),
                  "unsupported Icom AGC Off never writes Fast to the radio");
            check(caps.hasModeIndependentSquelch,
                  "IC-7300MK2 allows its native squelch in data and CW modes");
            check(!caps.hasFmRepeaterOffset, "MK2 does not advertise absent duplex commands");
            check(!caps.hasCwTune, "MK2 does not advertise an unimplemented CW tune carrier");
            {
                IcomCivBackend polled;
                IcomCivBackendTestAccess::prepareSession(polled, *ic7300Mk2);
                check(IcomCivBackendTestAccess::queuesNativeControlPolls(polled),
                      "MK2 periodic polling includes CW, squelch and active data TBW");
            }
            for (const bool reverse : {false, true}) {
                TxTestAuthority authority;
                IcomCivBackend cwBackend;
                cwBackend.setTransmitContext(authority.context);
                IcomCivBackendTestAccess::prepareSession(cwBackend, *ic7300Mk2);
                IcomCivBackendTestAccess::selectCwMode(cwBackend, reverse);
                const int power = IcomCivBackendTestAccess::power(cwBackend);
                cwBackend.setTune(true, 3, authority.operation);
                check(!IcomCivBackendTestAccess::tuning(cwBackend)
                          && IcomCivBackendTestAccess::power(cwBackend) == power
                          && IcomCivBackendTestAccess::lastOutboundCiv(cwBackend).isEmpty(),
                      "CW Tune refusal precedes power change, tone and PTT dispatch");
            }
            backend.setSliceRepeaterOffsetDir(0, QStringLiteral("up"));
            backend.setSliceFmRepeaterOffset(0, 600000);
            check(IcomCivBackendTestAccess::lastOutboundCiv(backend).isEmpty(),
                  "MK2 refuses undocumented repeater writes");
            check(caps.cwSpeedMinWpm == 6 && caps.cwSpeedMaxWpm == 48
                      && caps.cwPitchMinHz == 300 && caps.cwPitchMaxHz == 900,
                  "IC-7300MK2 CW ranges match CI-V endpoints");
            QObject::connect(&backend, &IRadioBackend::transmitChanged,
                             [&pitch](const TransmitDelta& delta) {
                if (delta.cwPitch) {
                    pitch = *delta.cwPitch;
                }
            });
            IcomCivBackendTestAccess::deliverCwPitch(backend, 128);
            check(pitch == 600 && caps.cwPitchStepHz == 5,
                  "IC-7300MK2 midpoint decodes to 600 Hz with 5 Hz controls");
            IcomCivBackendTestAccess::deliverCwPitch(backend, 0);
            check(pitch == 300, "IC-7300MK2 pitch minimum is 300 Hz");
            IcomCivBackendTestAccess::deliverCwPitch(backend, 255);
            check(pitch == 900, "IC-7300MK2 pitch maximum is 900 Hz");
        }

        // Official diagrams: the upper nibble is HIGH, the lower is LOW.
        // Exercise every pair through the actual reply handler, independently
        // of the writer so matching encoder/decoder mistakes cannot cancel out.
        for (const char* name : {"IC-7300MK2", "IC-705"}) {
            const IcomModel* model = modelForName(name);
            check(model != nullptr, "TBW model resolves");
            if (!model) {
                continue;
            }
            const auto profile = txBandwidthProfileFor(*model);
            check(profile.has_value(), "model has an official TBW profile");
            if (!profile) {
                continue;
            }
            int low = -1;
            int high = -1;
            IcomCivBackend backend;
            IcomCivBackendTestAccess::selectModel(backend, *model);
            QObject::connect(&backend, &IRadioBackend::transmitChanged,
                             [&](const TransmitDelta& delta) {
                if (delta.txFilterLow) {
                    low = *delta.txFilterLow;
                }
                if (delta.txFilterHigh) {
                    high = *delta.txFilterHigh;
                }
            });
            for (std::size_t h = 0; h < profile->highEdgesHz.size(); ++h) {
                for (std::size_t l = 0; l < profile->lowEdgesHz.size(); ++l) {
                    low = high = -1;
                    IcomCivBackendTestAccess::deliverDataBandwidth(
                        backend, profile->dataItem, static_cast<int>((h << 4) | l));
                    check(low == profile->lowEdgesHz[l] && high == profile->highEdgesHz[h],
                          "TBW reply respects official high/low nibble order");
                    IcomCivBackend writer;
                    IcomCivBackendTestAccess::prepareSession(writer, *model);
                    IcomCivBackendTestAccess::deliverDataBandwidth(writer, profile->dataItem, 0x30);
                    writer.setTxFilter(profile->lowEdgesHz[l], profile->highEdgesHz[h]);
                    IcomCivBackendTestAccess::dispatchReady(writer);
                    const QString expected = QStringLiteral("1a 05 00 %1 %2")
                        .arg((profile->dataItem / 10) * 16 + profile->dataItem % 10,
                             2, 16, QLatin1Char('0'))
                        .arg(static_cast<int>((h << 4) | l), 2, 16, QLatin1Char('0'));
                    check(IcomCivBackendTestAccess::lastOutboundCiv(writer) == expected,
                          "TBW writer sends the official high/low nibble order");
                }
            }
        }

    }
    check(cmdReadRxAntenna(0xB6) == std::vector<std::uint8_t>({0xFE,0xFE,0xB6,0xE0,0x12,0xFD}),
          "MK2 antenna read uses observed bare 12 form");
    for (const std::vector<uint8_t>& payload : {std::vector<uint8_t>{}, {2}, {0, 1}}) {
        IcomCivBackend backend;
        IcomCivBackendTestAccess::prepareSession(backend, *modelForName("IC-7300MK2"));
        const ControlSpec* antennaSpec = nullptr;
        for (const auto& spec : controlSpecs()) {
            if (spec.id == "rx.antenna") { antennaSpec = &spec; }
        }
        check(antennaSpec != nullptr, "antenna registry row exists");
        if (antennaSpec) {
            check(!backend.scrubDrive(*antennaSpec), "unread antenna cannot be scrubbed");
            IcomCivBackendTestAccess::antennaReply(backend, 0, payload);
            check(!backend.scrubDrive(*antennaSpec), "malformed first antenna reply cannot seed scrub");
            check(IcomCivBackendTestAccess::lastOutboundCiv(backend).isEmpty(),
                  "malformed antenna reply cannot cause a default antenna write");
        }
    }
    for (const char* name : {"IC-7300MK2", "IC-705", "IC-9700"}) {
        const auto* model = modelForName(name);
        check(model != nullptr, "antenna test model resolves");
        if (!model) { continue; }
        const bool supported = std::string(name) == "IC-7300MK2";
        IcomCivBackend b;
        IcomCivBackendTestAccess::prepareSession(b, *model);
        QString antenna;
        int publications = 0;
        QObject::connect(&b, &IRadioBackend::sliceChanged, [&](int, const SliceDelta& delta) {
            if (delta.rxAntenna) { antenna = *delta.rxAntenna; ++publications; }
        });
        IcomCivBackendTestAccess::antennaReply(b, 0, {1});
        check(supported ? antenna == "RX-ANT" : antenna.isEmpty(), "antenna adoption is model gated");
        const int before = publications;
        IcomCivBackendTestAccess::antennaReply(b, 0, {});
        IcomCivBackendTestAccess::antennaReply(b, 0, {2});
        IcomCivBackendTestAccess::antennaReply(b, 1, {0});
        IcomCivBackendTestAccess::antennaReply(b, 0, {0, 1});
        IcomCivBackendTestAccess::antennaReply(b, 0, {0}, 0);
        check(publications == before, "invalid and stale antenna replies cannot publish");
        IcomCivBackendTestAccess::antennaReply(b, 0, {0});
        check(supported ? antenna == "ANT1" : antenna.isEmpty(), "radio ANT1 return adopted");
        check(IcomCivBackendTestAccess::lastOutboundCiv(b).isEmpty(), "passive antenna adoption writes nothing");
        check(IcomCivBackendTestAccess::antennaReads(b, false) == supported, "periodic antenna read is model gated");
        IcomCivBackend startup;
        IcomCivBackendTestAccess::prepareSession(startup, *model);
        check(IcomCivBackendTestAccess::antennaReads(startup, true) == supported, "startup antenna read is model gated");
        if (supported) {
            check(IcomCivBackendTestAccess::antennaConfirmation(b), "antenna write confirmation shares generation");
            IcomCivBackend transaction;
            IcomCivBackendTestAccess::prepareSession(transaction, *model);
            check(IcomCivBackendTestAccess::antennaReplyCompletesRead(transaction),
                  "bare antenna read completes on subcommand-bearing reply without timeout");
        }
    }
    return g_failures ? 1 : 0;
}
