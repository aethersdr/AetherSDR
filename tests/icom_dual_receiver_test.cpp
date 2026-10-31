// Official IC-7610 A7380-7EX-4 pp.13-15 and IC-7760 A7788-8EX-2 pp.23-26.
// Inject decoded frames into the production backend. No sockets or radio peer.
#include "core/backends/icom/IcomCivBackend.h"
#include "core/backends/icom/IcomSession.h"
#include "core/Resampler.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "TestSettingsProfile.h"
#include "TxTestAuthority.h"
#include <QCoreApplication>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace AetherSDR;
using namespace AetherSDR::icom;

namespace AetherSDR::icom {
struct IcomStreamTestAccess {
    static void capture(IcomStream& stream, std::vector<std::vector<std::uint8_t>>& packets)
    {
        stream.m_testWriter = [&packets](std::span<const std::uint8_t> packet) {
            packets.emplace_back(packet.begin(),packet.end());
        };
        stream.m_ready = true;
    }
};
struct IcomCivBackendTestAccess {
    static void requestMedia(IcomSession& session, std::vector<std::vector<std::uint8_t>>& packets,
                              std::uint8_t hint, int channels = 0)
    {
        session.m_control = new IcomStream(&session);
        IcomStreamTestAccess::capture(*session.m_control,packets);
        session.m_serial = new IcomStream(&session);
        session.m_audio = new IcomStream(&session);
        session.m_haveRadioId = true;
        session.m_authOk = true;
        session.m_advertisedCivAddress = hint;
        session.m_params.rxChannels = channels;
        session.requestStreamsIfReady();
    }
    static bool queuedPtt(IcomCivBackend& backend, bool key, const TxCoordinator::Operation& operation)
    {
        (void)backend.m_civScheduler.reset();
        backend.m_civTrace.clear();
        backend.setKeying(key,operation);
        backend.pumpCiv(backend.nowMs() + 30);
        const QString expected = key ? QStringLiteral("fe fe 50 e0 1c 00 01 fd")
                                     : QStringLiteral("fe fe 50 e0 1c 00 00 fd");
        return std::ranges::any_of(backend.m_civTrace,[&](const auto& entry) {
            return entry.outbound && entry.hex == expected;
        });
    }
    static void prepare(IcomCivBackend& backend, std::uint8_t model)
    {
        backend.m_session = std::make_unique<IcomSession>(); // unstarted, no sockets
        backend.m_session->setCivAddress(0x50); // identity != configured bus address
        backend.m_model = &unknownModel();
        backend.m_deviceName = QStringLiteral("Operator-named RF deck");
        backend.m_connected = true;
        backend.m_sessionGeneration = 7;
        backend.m_civReported = 0;
        emit backend.connected();
        CivFrame identity;
        identity.from = 0x50;
        identity.to = kControllerAddress;
        identity.cmd = cmd::kReadId;
        identity.hasSub = true;
        identity.sub = 0;
        identity.data = {model};
        backend.onCivFrame(identity,7);
        (void)backend.m_civScheduler.reset();
        backend.m_lanOutput = 0;
    }
    static void inject(IcomCivBackend& backend, std::vector<std::uint8_t> body,
                       std::uint64_t generation = 7, std::uint8_t source = 0x50)
    {
        std::vector<std::uint8_t> wire{0xfe,0xfe,0xe0,source};
        wire.insert(wire.end(), body.begin(), body.end());
        wire.push_back(0xfd);
        const auto parsed = parseFrame(wire);
        if (parsed) { backend.onCivFrame(*parsed, generation); }
    }
    static void audio(IcomCivBackend& b, const std::vector<float>& pcm) { b.onDualAudio(pcm); }
    static int data(const IcomCivBackend& b, int receiver) { return b.m_receivers[receiver].data; }
    static auto confirmation(const IcomCivBackend& b, const std::vector<std::uint8_t>& wire)
    { return b.confirmationFor(wire); }
    static auto key(const IcomCivBackend& b, const std::vector<std::uint8_t>& wire)
    { return b.semanticKey(wire); }
    static auto addressed(const IcomCivBackend& b, const std::vector<std::uint8_t>& wire)
    { return b.receiverCommand(wire); }
    static int tx(const IcomCivBackend& b) { return b.m_dualTxReceiver; }
    static int dataItem(const IcomCivBackend& b) { return b.currentModulationProfile()->dataInputItem; }
};
}

int failures = 0;
void check(bool value, const char* message)
{
    std::printf("%s %s\n", value ? "PASS" : "FAIL", message);
    failures += !value;
}
std::vector<std::uint8_t> makeScope(int receiver, int mode, std::uint64_t a,
                                 std::uint64_t b, bool outOfRange = false)
{
    std::vector<std::uint8_t> body{0x27,0,static_cast<std::uint8_t>(receiver),1,1,
                                 static_cast<std::uint8_t>(mode)};
    const auto first = encodeFreq(a), second = encodeFreq(b);
    body.insert(body.end(), first.begin(), first.end());
    body.insert(body.end(), second.begin(), second.end());
    body.push_back(outOfRange ? 1 : 0);
    if (!outOfRange) {
        body.resize(body.size() + 689, 0);
        body[17 + 344] = 200; // centre bin
        body[17 + 600] = 100; // would have been truncated at 475
    }
    return body;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    TestSettingsProfile settings(QStringLiteral("icom-dual-receiver"));
    if (!settings.isValid()) { return 1; }
    for (const auto [hint,overrideChannels,expected] :
         {std::array<int,3>{0x98,0,16}, {0xb2,0,16}, {0xa4,0,4}, {0x50,2,16}, {0x98,1,4}}) {
        std::vector<std::vector<std::uint8_t>> packets;
        IcomSession session;
        IcomCivBackendTestAccess::requestMedia(session,packets,static_cast<std::uint8_t>(hint),overrideChannels);
        check(!packets.empty() && packets.back().size() > 0x73
              && packets.back()[0x72] == expected && packets.back()[0x73] == 4,
              "production stream request negotiates RX shape independently of mono TX");
    }
    for (std::uint8_t model : {0x98, 0xb2}) {
        RadioModel radio;
        if (!radio.rebuildBackendForTest(QStringLiteral("icom"))) { return 1; }
        auto& backend = static_cast<IcomCivBackend&>(*radio.backend());
        std::array<QVector<float>,2> renderSpectrum, renderWaterfall;
        std::array<double,2> waterfallLow{}, waterfallHigh{};
        QObject::connect(&radio, &RadioModel::panFeedSpectrumReady,
            [&](quint32 streamId, const QVector<float>& bins, qint64, const SpectrumDecodeScale&) {
                const int receiver = streamId & 0xff;
                if (receiver < 2) { renderSpectrum[receiver] = bins; }
            });
        QObject::connect(&radio, &RadioModel::panFeedWaterfallRowReady,
            [&](quint32 streamId, const QVector<float>& bins, double low, double high, quint32, qint64, bool) {
                const int receiver = streamId & 0xff;
                if (receiver < 2) { renderWaterfall[receiver] = bins; waterfallLow[receiver] = low; waterfallHigh[receiver] = high; }
            });
        std::array<QByteArray,2> spectra;
        std::array<double,2> centres{}, widths{}, frequencies{};
        std::array<int,2> slices{}, sliceAudio{};
        int speakerFrames = 0;
        float speakerSample = 0;
        std::array<float,2> receiverSample{};
        QObject::connect(&backend, &IRadioBackend::panCenterBandwidthChanged,
            [&](const QString& id, double centre, double width) { centres[id.toInt()] = centre; widths[id.toInt()] = width; });
        QObject::connect(&backend, &IRadioBackend::sliceChanged,
            [&](int id, const SliceDelta& delta) { ++slices[id]; if (delta.frequency) { frequencies[id] = *delta.frequency; } });
        QObject::connect(&backend, &IRadioBackend::spectrumFrameReady,
            [&](int id, const QByteArray& bins, const SpectrumCoverage&) { spectra[id] = bins; });
        QObject::connect(&backend, &IRadioBackend::sliceAudioFrameReady,
            [&](int id, const PcmFrame& pcm) { ++sliceAudio[id]; receiverSample[id] = pcm.samples().last(); });
        QObject::connect(&backend, &IRadioBackend::audioFrameReady,
            [&](const PcmFrame& pcm) { ++speakerFrames; speakerSample = pcm.samples().last(); });
        int identityUpdates = 0;
        QObject::connect(&radio,&RadioModel::infoChanged,[&] { ++identityUpdates; });
        IcomCivBackendTestAccess::prepare(backend, model);
        check(radio.model() == (model == 0x98 ? QStringLiteral("IC-7610") : QStringLiteral("IC-7760"))
              && radio.nickname() == QStringLiteral("Operator-named RF deck") && identityUpdates > 0,
              "CI-V 19 00 publishes the canonical status-bar model independently of address and nickname");
        check(slices[0] && slices[1] && backend.capabilities().maxPanadapters == 2,
              "two fixed receivers and two pans after identity");
        IcomCivBackendTestAccess::inject(backend, makeScope(0,0,14'100'000,100'000));
        IcomCivBackendTestAccess::inject(backend, makeScope(1,3,7'000'000,7'200'000));
        check(spectra[0].size() == 689 * sizeof(float) && spectra[1].size() == 689 * sizeof(float),
              "both scopes preserve all 689 native bins");
        float centreBin = 0, farBin = 0;
        if (spectra[0].size() == 689 * sizeof(float)) {
            std::memcpy(&centreBin, spectra[0].constData() + 344 * sizeof(float), sizeof(float));
            std::memcpy(&farBin, spectra[0].constData() + 600 * sizeof(float), sizeof(float));
        }
        check(centreBin == -40.0f && farBin == -90.0f, "native bin positions and 0..200 amplitude scale survive");
        check(centres[0] == 14.1 && widths[0] == 0.2 && centres[1] == 7.1 && widths[1] == 0.2,
              "Center half-span and Scroll-F edges remain receiver-specific");
        check(radio.slices().size() == 2 && radio.panadapters().size() == 2
              && radio.slice(0)->panId() != radio.slice(1)->panId(),
              "production RadioModel attaches both slices to distinct pans");
        check(renderSpectrum[0].size() == 689 && renderSpectrum[1].size() == 689
              && renderSpectrum == renderWaterfall
              && std::abs(waterfallLow[0] - 14.0) < 1e-9 && std::abs(waterfallHigh[0] - 14.2) < 1e-9
              && std::abs(waterfallLow[1] - 7.0) < 1e-9 && std::abs(waterfallHigh[1] - 7.2) < 1e-9,
              "production spectrum and waterfall share bins, orientation and frequency edges");
        const QByteArray before = spectra[1];
        IcomCivBackendTestAccess::inject(backend, makeScope(0,1,14'000'000,14'200'000,true));
        check(spectra[1] == before, "out-of-range MAIN does not clear SUB");
        const QByteArray beforeMain = spectra[0];
        IcomCivBackendTestAccess::inject(backend, makeScope(0,0,18'100'000,100'000),6);
        IcomCivBackendTestAccess::inject(backend, makeScope(0,0,18'100'000,100'000),7,0x51);
        check(spectra[0] == beforeMain, "obsolete session and foreign bus device cannot publish spectrum");
        auto shortScope = makeScope(0,0,14'100'000,100'000); shortScope.pop_back();
        IcomCivBackendTestAccess::inject(backend, shortScope);
        check(spectra[0] == beforeMain, "truncated waveform cannot become a padded plausible trace");
        for (int receiver : {1,0}) {
            auto freq = encodeFreq(receiver ? 7'150'000 : 14'200'000);
            std::vector<std::uint8_t> body{0x25,static_cast<std::uint8_t>(receiver)};
            body.insert(body.end(), freq.begin(), freq.end());
            IcomCivBackendTestAccess::inject(backend, body);
            IcomCivBackendTestAccess::inject(backend, {0x26,static_cast<std::uint8_t>(receiver),1,
                static_cast<std::uint8_t>(receiver ? 3 : 2),2});
        }
        check(frequencies[0] == 14.2 && frequencies[1] == 7.15,
              "reordered addressed frequency replies preserve MAIN/SUB identity");
        check(IcomCivBackendTestAccess::data(backend,0) == 2 && IcomCivBackendTestAccess::data(backend,1) == 3,
              "D2/D3 are retained as enums");
        IcomCivBackendTestAccess::inject(backend, {0x0f,0});
        backend.setActiveSlice(1);
        check(IcomCivBackendTestAccess::tx(backend) == 0, "SUB editing focus cannot move TX");
        IcomCivBackendTestAccess::inject(backend, {0x0f,1});
        check(IcomCivBackendTestAccess::tx(backend) == 1
              && IcomCivBackendTestAccess::dataItem(backend) == (model == 0x98 ? 94 : 132),
              "split selects SUB's exact DATA3 modulation register");
        TxTestAuthority authority;
        check(IcomCivBackendTestAccess::queuedPtt(backend,true,authority.operation),
              "authorized PTT uses one global 1C 00 at the customized address");
        check(IcomCivBackendTestAccess::queuedPtt(backend,false,authority.operation),
              "unkey remains global and reaches the emergency dispatch path");
        const auto modeWrite = buildFrameSub(0x50,0x26,1,std::array<std::uint8_t,3>{1,3,2});
        check(IcomCivBackendTestAccess::confirmation(backend, modeWrite) == buildFrameSub(0x50,0x26,1),
              "SUB mode write confirmation stays SUB");
        const auto mainRead = buildFrameSub(0x50,0x26,0);
        check(IcomCivBackendTestAccess::key(backend,modeWrite) != IcomCivBackendTestAccess::key(backend,mainRead),
              "MAIN/SUB writes do not supersede each other's scheduler generations");
        check(IcomCivBackendTestAccess::addressed(backend,cmdSetLevel(0x50,level::kRf,100))
                  == buildFrameSub(0x50,0x29,0,std::array<std::uint8_t,4>{0x14,0x02,0x01,0x00}),
              "generic MAIN control remains addressed while SUB is focused");
        for (const auto& global : {cmdSetLevel(0x50,level::kRfPower,100),
                                  cmdSetLevel(0x50,level::kMicGain,100),
                                  cmdSetFunction(0x50,func::kVox,0), cmdSetPtt(0x50,false)}) {
            check(IcomCivBackendTestAccess::addressed(backend,global) == global,
                  "global TX controls never acquire a command-29 receiver wrapper");
        }
        std::vector<float> pcm(1920,0);
        for (std::size_t i=0;i<960;++i) { pcm[i*2]=0.25f; pcm[i*2+1]=-0.125f; }
        IcomCivBackendTestAccess::audio(backend,pcm);
        check(sliceAudio[0] && sliceAudio[1] && speakerFrames, "stereo RX produces separate decoder feeds and one speaker mix");
        for (int block = 0; block < 3; ++block) { IcomCivBackendTestAccess::audio(backend,pcm); }
        check(std::abs(receiverSample[0] - 0.25f) < 0.001f
              && std::abs(receiverSample[1] + 0.125f) < 0.001f
              && std::abs(speakerSample - 0.0625f) < 0.001f,
              "separate receiver samples produce the expected bounded speaker mix");
        SliceAudioRequest mute;
        mute.field = SliceAudioRequest::Field::Mute;
        mute.value = 1;
        backend.requestSliceAudio(1,mute);
        IcomCivBackendTestAccess::audio(backend,pcm);
        check(std::abs(speakerSample - 0.125f) < 0.001f
              && std::abs(receiverSample[1] + 0.125f) < 0.001f,
              "muting SUB removes only its monitor contribution, preserving decoder audio");
        const int previousSpeakerFrames = speakerFrames;
        IcomCivBackendTestAccess::inject(backend, model == 0x98
            ? std::vector<std::uint8_t>{0x1a,5,0,0x86,1}
            : std::vector<std::uint8_t>{0x1a,5,1,0x23,1});
        IcomCivBackendTestAccess::audio(backend,pcm);
        check(speakerFrames == previousSpeakerFrames, "LAN IF is not played as demodulated AF");
    }
    return failures ? 1 : 0;
}
