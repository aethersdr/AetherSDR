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
#include <QThread>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ranges>

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
    static void captureUplink(IcomCivBackend& backend,
                              std::vector<std::vector<std::uint8_t>>& packets)
    {
        backend.m_session->m_audio = new IcomStream(backend.m_session.get());
        IcomStreamTestAccess::capture(*backend.m_session->m_audio, packets);
    }
    static void pumpUplink(IcomCivBackend& backend) { backend.m_session->onTxPump(); }
    static void receiveAudio(IcomSession& session, std::uint16_t sequence, int bytes)
    {
        const auto packet = buildAudio(0,0,sequence,sequence,
            std::vector<std::uint8_t>(static_cast<std::size_t>(bytes),0));
        session.onAudioPayload(QByteArray(reinterpret_cast<const char*>(packet.data()),
            static_cast<qsizetype>(packet.size())));
    }
    static void receiveIdle(IcomSession& session)
    {
        const auto packet = buildIdle(0,0,100);
        session.onAudioPayload(QByteArray(reinterpret_cast<const char*>(packet.data()),
            static_cast<qsizetype>(packet.size())));
    }
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
    static void events() { for (int i=0; i<4; ++i) { QThread::msleep(10); QCoreApplication::processEvents(); } }
    static void clearCommands(IcomCivBackend& b) { (void)b.m_civScheduler.reset(); b.m_civTrace.clear(); }
    static void pump(IcomCivBackend& b) { b.pumpCiv(b.nowMs() + 30); }
    static bool sent(const IcomCivBackend& b, const std::vector<std::uint8_t>& wire)
    {
        const QString hex = QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(wire.data()), wire.size()).toHex(' '));
        return std::ranges::any_of(b.m_civTrace, [&](const auto& row) { return row.outbound && row.hex == hex; });
    }
    static int borrowedSlot(const IcomCivBackend& b, int receiver) { return b.m_receivers[receiver].borrowedSlot; }
    static std::size_t snapshots(const IcomCivBackend& b) { return b.m_dualSavedPresets.size(); }
    static auto snapshot(const IcomCivBackend& b, int slot) { return b.m_dualSavedPresets.at({6, slot}); }
    static void audio(IcomCivBackend& b, const std::vector<float>& pcm) { b.onDualAudio(pcm); }
    static int data(const IcomCivBackend& b, int receiver) { return b.m_receivers[receiver].data; }
    static auto confirmation(const IcomCivBackend& b, const std::vector<std::uint8_t>& wire)
    { return b.confirmationFor(wire); }
    static auto key(const IcomCivBackend& b, const std::vector<std::uint8_t>& wire)
    { return b.semanticKey(wire); }
    static auto addressed(const IcomCivBackend& b, const std::vector<std::uint8_t>& wire)
    { return b.receiverCommand(wire); }
    static int tx(const IcomCivBackend& b) { return b.m_dualTxReceiver; }
    static auto dualWatch(const IcomCivBackend& b) { return b.m_dualWatch; }
    static bool requestDualWatch(IcomCivBackend& backend)
    {
        (void)backend.m_civScheduler.reset();
        backend.m_civTrace.clear();
        backend.invokeExtension(QStringLiteral("icom"), QStringLiteral("receiver.dualwatch"),77,QStringLiteral("on"));
        backend.pumpCiv(backend.nowMs()+30);
        return std::ranges::any_of(backend.m_civTrace,[](const auto& entry) {
            return entry.outbound && entry.hex == QStringLiteral("fe fe 50 e0 07 c2 01 fd");
        });
    }
    static int dataItem(const IcomCivBackend& b) { return b.currentModulationProfile()->dataInputItem; }
    static QString warning(const IcomCivBackend& b) { return b.m_lastModInputWarning; }
    static void pcAudio(IcomCivBackend& b) { b.m_pcAudioEnabled = true; }
    static auto txFrequency(const IcomCivBackend& b) { return b.m_dualTxFrequencyHz; }
    static std::uint64_t scopeFrames(const IcomCivBackend& b, int receiver)
    { return b.m_receivers[receiver].scopeFrames; }
    static double reference(const IcomCivBackend& b, int receiver)
    { return b.m_receivers[receiver].calibration.referenceDb; }
    static std::uint64_t audioFrames(const IcomCivBackend& b, int receiver)
    { return b.m_receivers[receiver].audioFrames; }
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
         {std::array<int,3>{0x98,0,16}, {0xb2,0,16}, {0x8e,0,16}, {0xa4,0,4}, {0x50,2,16}, {0x98,1,4}}) {
        std::vector<std::vector<std::uint8_t>> packets;
        IcomSession session;
        IcomCivBackendTestAccess::requestMedia(session,packets,static_cast<std::uint8_t>(hint),overrideChannels);
        check(!packets.empty() && packets.back().size() > 0x73
              && packets.back()[0x72] == expected && packets.back()[0x73] == 4,
              "production stream request negotiates RX shape independently of mono TX");
        if (expected == 16) {
            for (int i = 0; i < 6; ++i) {
                IcomCivBackendTestAccess::receiveAudio(session,
                    static_cast<std::uint16_t>(65530 + i), i % 3 == 2 ? 1112 : 1364);
            }
            IcomCivBackendTestAccess::receiveIdle(session);
            IcomCivBackendTestAccess::receiveAudio(session,1,1364); // one missing long fragment across wrap
            const QVariantMap media = session.transportDiagnostics().value(QStringLiteral("media")).toMap();
            check(media.value(QStringLiteral("receivedPackets")).toULongLong() == 7
                  && media.value(QStringLiteral("receivedFrames")).toULongLong() == 2261
                  && media.value(QStringLiteral("concealedFrames")).toULongLong() == 341
                  && media.value(QStringLiteral("concealmentBlocks")).toULongLong() == 1,
                  "session diagnostics separate received stereo frames, exact concealment and outer idles");
        }
    }
    for (std::uint8_t model : {0x98, 0xb2, 0x8e}) {
        std::vector<std::vector<std::uint8_t>> uplink;
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
        float speakerLeft = 0, speakerSample = 0;
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
            [&](const PcmFrame& pcm) { ++speakerFrames; speakerLeft = pcm.samples().first(); speakerSample = pcm.samples().last(); });
        int identityUpdates = 0;
        QObject::connect(&radio,&RadioModel::infoChanged,[&] { ++identityUpdates; });
        IcomCivBackendTestAccess::prepare(backend, model);
        check(radio.panadapters().size() == 1 && radio.slices().size() == 1,
              "connection presents MAIN only while retaining stereo transport");
        check(backend.createPanadapter(), "opening SUB uses the native pan lifecycle");
        IcomCivBackendTestAccess::inject(backend, {0x07,0xc2,1});
        check(radio.model() == (model == 0x98 ? QStringLiteral("IC-7610")
                               : model == 0xb2 ? QStringLiteral("IC-7760") : QStringLiteral("IC-7851"))
              && radio.nickname() == QStringLiteral("Operator-named RF deck") && identityUpdates > 0,
              "CI-V 19 00 publishes the canonical status-bar model independently of address and nickname");
        check(slices[0] && !slices[1] && backend.capabilities().maxPanadapters == 2,
              "MAIN is visible and SUB capacity remains available after identity");
        IcomCivBackendTestAccess::inject(backend, makeScope(0,0,14'100'000,100'000));
        IcomCivBackendTestAccess::inject(backend, makeScope(1,3,7'000'000,7'200'000));
        check(spectra[0].size() == 689 * sizeof(float) && spectra[1].size() == 689 * sizeof(float),
              "both scopes preserve all 689 native bins");
        float centreBin = 0, farBin = 0;
        if (spectra[0].size() == 689 * sizeof(float)) {
            std::memcpy(&centreBin, spectra[0].constData() + 344 * sizeof(float), sizeof(float));
            std::memcpy(&farBin, spectra[0].constData() + 600 * sizeof(float), sizeof(float));
        }
        const double spanDb = profileFor(*modelForId(model)).dualReceiver->scopeSpanDb;
        check(std::abs(centreBin - (-140.0 + spanDb)) < 0.001
              && std::abs(farBin - (-140.0 + spanDb / 2)) < 0.001,
              "native bin positions and profile-relative 0..200 amplitude scale survive");
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
        const auto mainTrace = spectra[0];
        for (int i = 0; i < 4; ++i) {
            IcomCivBackendTestAccess::inject(backend, makeScope(1,1,7'050'000,7'250'000));
        }
        check(spectra[0] == mainTrace && centres[0] == 14.1 && centres[1] == 7.15
              && IcomCivBackendTestAccess::scopeFrames(backend,0) == 1
              && IcomCivBackendTestAccess::scopeFrames(backend,1) == 5,
              "SUB sweeps cannot replace MAIN's trace, axis, waterfall or freshness counter");
        IcomCivBackendTestAccess::inject(backend, {0x27,0x19,1,0x20,0,0});
        check(IcomCivBackendTestAccess::reference(backend,1) == (model == 0x8e ? 20.0 : 0.0),
              "receiver REF readback follows profile limits without changing other models");
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
        // Official model tables: DATA OFF/D1/D2/D3 are distinct global menus.
        // D1=USB and D2=LAN is the operator's IC-7851 configuration.
        const int offItem = model == 0x98 ? 91 : model == 0xb2 ? 129 : 63;
        const std::uint8_t lan = model == 0x98 ? 0x05 : 0x09;
        const std::uint8_t usb = model == 0x98 ? 0x03 : model == 0xb2 ? 0x01 : 0x08;
        IcomCivBackendTestAccess::pcAudio(backend);
        for (int variant = 0; variant < 4; ++variant) {
            const int item = offItem + variant;
            IcomCivBackendTestAccess::inject(backend, {0x1a,0x05,encodeBcdByte(item / 100),
                encodeBcdByte(item % 100), variant == 0 ? std::uint8_t{0} : variant == 1 ? usb : lan});
        }
        check(IcomCivBackendTestAccess::warning(backend).isEmpty(),
              "active D2 LAN does not warn about inactive DATA OFF MIC or D1 USB");
        IcomCivBackendTestAccess::inject(backend, {0x26,0,1,1,2});
        check(IcomCivBackendTestAccess::warning(backend).contains(QStringLiteral("DATA1 MOD is USB"))
              && !IcomCivBackendTestAccess::warning(backend).contains(QStringLiteral("PC Audio is on but")),
              "D1 USB warning names the actual variant and excludes unused DATA OFF");
        IcomCivBackendTestAccess::inject(backend, {0x26,0,1,2,2});
        check(IcomCivBackendTestAccess::warning(backend).isEmpty(),
              "switching to confirmed D2 reuses its own LAN source instead of stale D1");
        IcomCivBackendTestAccess::inject(backend,{0x07,0xc2,0});
        check(IcomCivBackendTestAccess::dualWatch(backend) == false
              && IcomCivBackendTestAccess::requestDualWatch(backend)
              && IcomCivBackendTestAccess::dualWatch(backend) == false,
              "Dualwatch uses explicit C2 intent and retains reported OFF until readback");
        IcomCivBackendTestAccess::inject(backend,{0x07,0xc2,1});
        IcomCivBackendTestAccess::inject(backend,{0x07,0xc2,2});
        check(IcomCivBackendTestAccess::dualWatch(backend) == true
              && frequencies[0] == 14.2 && frequencies[1] == 7.15,
              "valid Dualwatch readback enables SUB without equalizing or moving either receiver");
        check(IcomCivBackendTestAccess::confirmation(backend,
                  buildFrameSub(0x50,0x07,0xc2,std::array<std::uint8_t,1>{1}))
                  == buildFrameSub(0x50,0x07,0xc2),
              "Dualwatch confirmation reads C2 instead of changing selected-band focus");
        IcomCivBackendTestAccess::inject(backend, {0x0f,0});
        backend.setActiveSlice(1);
        check(IcomCivBackendTestAccess::tx(backend) == 0, "SUB editing focus cannot move TX");
        IcomCivBackendTestAccess::clearCommands(backend);
        check(backend.requestSliceControl(1, {SliceControlRequest::Field::TxSlice, true}) == ReceiveDispatch::Dispatched
              && IcomCivBackendTestAccess::tx(backend) == 0,
              "SUB TX selection dispatches split but retains confirmed MAIN until readback");
        IcomCivBackendTestAccess::pump(backend);
        check(IcomCivBackendTestAccess::sent(backend, buildFrame(0x50, 0x0f, std::array<std::uint8_t,1>{1})),
              "SUB TX card selects split ON without changing focus or keying PTT");
        IcomCivBackendTestAccess::clearCommands(backend);
        IcomCivBackendTestAccess::inject(backend, {0x0f,1});
        check(IcomCivBackendTestAccess::tx(backend) == 1
              && IcomCivBackendTestAccess::dataItem(backend) == (model == 0x98 ? 94 : model == 0xb2 ? 132 : 66),
              "split selects SUB's exact DATA3 modulation register");
        auto txFrequency = encodeFreq(7'150'000);
        std::vector<std::uint8_t> txReadback{0x1c,0x03};
        txReadback.insert(txReadback.end(), txFrequency.begin(), txFrequency.end());
        IcomCivBackendTestAccess::inject(backend, txReadback);
        check(IcomCivBackendTestAccess::txFrequency(backend) == 7'150'000,
              "actual TX-frequency reply is retained alongside the transmit model delta");
        check(backend.requestSliceControl(0, {SliceControlRequest::Field::TxSlice, true}) == ReceiveDispatch::Dispatched
              && IcomCivBackendTestAccess::tx(backend) == 1,
              "MAIN TX selection waits for split OFF readback");
        IcomCivBackendTestAccess::clearCommands(backend);
        IcomCivBackendTestAccess::inject(backend, {0x0f,0});
        IcomCivBackendTestAccess::inject(backend, {0x0f,1});
        TxTestAuthority authority;
        IcomCivBackendTestAccess::captureUplink(backend, uplink);
        const auto stereoPcm = [](int frames) {
            std::vector<qint16> samples(static_cast<std::size_t>(frames) * 2);
            for (int i = 0; i < frames; ++i) {
                samples[i * 2] = 16384;
                samples[i * 2 + 1] = 8192;
            }
            return QByteArray(reinterpret_cast<const char*>(samples.data()),
                              static_cast<qsizetype>(samples.size() * sizeof(qint16)));
        };
        const auto submit = [&](int rate, const TxCoordinator::Context& context) {
            backend.submitTxAudio(stereoPcm(4096), rate, TxAudioSource::ClientLeveled, context);
            IcomCivBackendTestAccess::pumpUplink(backend);
        };
        const auto decodedUplink = [&] {
            std::vector<std::uint8_t> payload;
            for (const auto& packet : uplink) {
                const auto chunk = audioPayload(packet);
                payload.insert(payload.end(), chunk.begin(), chunk.end());
            }
            return decodeAudio(AudioCodec::Lpcm1ch16, payload);
        };
        submit(48000, authority.context);
        check(uplink.empty(), "unkeyed dual receiver refuses outgoing audio packets");
        check(IcomCivBackendTestAccess::queuedPtt(backend,true,authority.operation),
              "authorized PTT uses one global 1C 00 at the customized address");
        submit(48000, {});
        check(uplink.empty(), "keyed dual receiver still requires an authorized audio context");
        submit(48000, authority.context);
        const auto direct = decodedUplink();
        check(direct.size() == 960 && std::ranges::all_of(direct, [](float sample) {
                  return std::abs(sample - 0.375f) < 0.0001f;
              }), "production uplink packetizes one 20 ms mono frame from stereo input");
        uplink.clear();
        // Start a fresh transmission so the first frame here must come from
        // the 24 -> 48 kHz path, rather than the preceding queue's tail.
        check(IcomCivBackendTestAccess::queuedPtt(backend,false,authority.operation)
              && IcomCivBackendTestAccess::queuedPtt(backend,true,authority.operation),
              "new transmission clears the previous audio queue");
        submit(24000, authority.context);
        // The resampler retains acoustic filter delay. Drain the available
        // complete packets before checking the settled signal at their tail.
        for (int tick = 0; tick < 8; ++tick) { IcomCivBackendTestAccess::pumpUplink(backend); }
        const auto resampled = decodedUplink();
        check(resampled.size() >= 7680 && resampled.size() % 960 == 0
              && std::ranges::all_of(resampled | std::views::drop(resampled.size() - 200), [](float sample) {
                  return std::abs(sample - 0.375f) < 0.002f;
              }), "24 kHz seam audio reaches the 48 kHz mono uplink with expected level");
        // Leave another producer block queued to exercise unkey cancellation.
        backend.submitTxAudio(stereoPcm(4096), 24000, TxAudioSource::ClientLeveled, authority.context);
        check(IcomCivBackendTestAccess::queuedPtt(backend,false,authority.operation),
              "unkey remains global and reaches the emergency dispatch path");
        uplink.clear();
        IcomCivBackendTestAccess::pumpUplink(backend);
        submit(24000, authority.context);
        check(uplink.empty(), "unkey flushes queued uplink audio and refuses new microphone frames");
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
        // Native codec-10 cycle: 1364/1364/1112 PCM bytes = 341/341/278
        // stereo sample frames. Both resamplers must consume every fragment,
        // including fragments for which their filter emits no output yet.
        std::uint64_t inputFrames = 0;
        for (int i = 0; i < 8; ++i) {
            IcomCivBackendTestAccess::audio(backend, {0.25f,-0.125f});
            ++inputFrames;
        }
        for (int cycle = 0; cycle < 16; ++cycle) {
            for (int frames : {341,341,278}) {
                std::vector<float> fragment(frames * 2);
                for (int i = 0; i < frames; ++i) {
                    fragment[i * 2] = 0.25f;
                    fragment[i * 2 + 1] = -0.125f;
                }
                IcomCivBackendTestAccess::audio(backend,fragment);
                inputFrames += frames;
            }
        }
        check(IcomCivBackendTestAccess::audioFrames(backend,0) == inputFrames
              && IcomCivBackendTestAccess::audioFrames(backend,1) == inputFrames,
              "native stereo fragments advance MAIN/SUB on the same sample timeline");
        IcomCivBackendTestAccess::audio(backend,pcm);
        check(sliceAudio[0] && sliceAudio[1] && speakerFrames, "stereo RX produces separate decoder feeds and one speaker mix");
        for (int block = 0; block < 3; ++block) { IcomCivBackendTestAccess::audio(backend,pcm); }
        check(std::abs(receiverSample[0] - 0.25f) < 0.001f
              && std::abs(receiverSample[1] + 0.125f) < 0.001f
              && std::abs(speakerLeft - 0.25f) < 0.001f
              && std::abs(speakerSample + 0.125f) < 0.001f,
              "shared default monitor routes MAIN left and SUB right without crossfeed");
        SliceAudioRequest mute;
        mute.field = SliceAudioRequest::Field::Mute;
        mute.value = 1;
        backend.requestSliceAudio(1,mute);
        IcomCivBackendTestAccess::audio(backend,pcm);
        check(std::abs(speakerLeft - 0.25f) < 0.001f
              && std::abs(speakerSample) < 0.001f
              && std::abs(receiverSample[1] + 0.125f) < 0.001f,
              "muting SUB removes only its monitor contribution, preserving decoder audio");
        mute.value = 0;
        backend.requestSliceAudio(1,mute);
        SliceAudioRequest pan;
        pan.field = SliceAudioRequest::Field::Pan;
        pan.value = 50;
        backend.requestSliceAudio(0,pan);
        backend.requestSliceAudio(1,pan);
        IcomCivBackendTestAccess::audio(backend,pcm);
        check(std::abs(speakerLeft - 0.0625f) < 0.001f
              && std::abs(speakerSample - 0.0625f) < 0.001f,
              "centering both receivers produces a bounded equal mix on both speakers");
        const int previousSpeakerFrames = speakerFrames;
        IcomCivBackendTestAccess::inject(backend, model == 0x98
            ? std::vector<std::uint8_t>{0x1a,5,0,0x86,1}
            : model == 0xb2 ? std::vector<std::uint8_t>{0x1a,5,1,0x23,1}
                           : std::vector<std::uint8_t>{0x1a,5,0,0x56,1});
        IcomCivBackendTestAccess::audio(backend,pcm);
        check(speakerFrames == previousSpeakerFrames, "LAN IF is not played as demodulated AF");
    }
    for (std::uint8_t model : {0x98, 0xb2, 0x8e}) {
        RadioModel radio;
        if (!radio.rebuildBackendForTest(QStringLiteral("icom"))) { return 1; }
        auto& backend = static_cast<IcomCivBackend&>(*radio.backend());
        IcomCivBackendTestAccess::prepare(backend, model);
        const auto injectWire = [&](const std::vector<std::uint8_t>& wire, std::uint64_t generation = 7) {
            IcomCivBackendTestAccess::inject(backend, {wire.begin() + 4, wire.end() - 1}, generation);
        };
        injectWire(buildFrameSub(0x50,0x25,0,encodeFreq(14100000)));
        injectWire(buildFrameSub(0x50,0x25,1,encodeFreq(14300000)));
        IcomCivBackendTestAccess::inject(backend, makeScope(0,3,14000000,14200000));
        IcomCivBackendTestAccess::inject(backend, makeScope(1,3,14200000,14400000));
        IcomCivBackendTestAccess::inject(backend, {0x27,0x16,0,1});
        IcomCivBackendTestAccess::inject(backend, {0x27,0x16,1,1});
        IcomCivBackendTestAccess::inject(backend, {0x07,0xc2,0});
        check(backend.createPanadapter() && radio.panadapters().size() == 1,
              "SUB creation waits for Dualwatch readback rather than inventing enable state");
        IcomCivBackendTestAccess::inject(backend, {0x07,0xc2,1});
        check(radio.panadapters().size() == 2, "confirmed Dualwatch opens the existing SUB receiver");
        IcomCivBackendTestAccess::clearCommands(backend);
        backend.setPanCenter(QStringLiteral("0"),14120000,IRadioBackend::PanCenterIntent::Drag);
        IcomCivBackendTestAccess::events();
        const auto mainRead = buildFrameSub(0x50,0x27,0x1e,std::array<std::uint8_t,2>{6,4});
        check(IcomCivBackendTestAccess::sent(backend,mainRead) && IcomCivBackendTestAccess::snapshots(backend)==0,
              "Scroll-F gesture reads a spare preset before any persistent write");
        const auto mainOriginal = cmdScopeFixedEdge(0x50,6,4,14000000,14500000);
        injectWire(mainOriginal,6);
        check(IcomCivBackendTestAccess::snapshots(backend)==0, "stale-session preset cannot authorize a window write");
        injectWire(cmdScopeFixedEdge(0x50,7,4,15000000,15500000));
        check(IcomCivBackendTestAccess::snapshots(backend)==0,
              "a different preset reply cannot authorize or retire the requested backup");
        injectWire(mainOriginal);
        IcomCivBackendTestAccess::pump(backend);
        check(IcomCivBackendTestAccess::snapshots(backend)==1
              && IcomCivBackendTestAccess::sent(backend,cmdScopeFixedEdge(0x50,6,4,14020000,14220000)),
              "MAIN Scroll-F writes the clamped kHz window after saving readback");
        IcomCivBackendTestAccess::clearCommands(backend);
        backend.setPanCenter(QStringLiteral("1"),14320000,IRadioBackend::PanCenterIntent::Drag);
        IcomCivBackendTestAccess::events();
        const auto subOriginal = cmdScopeFixedEdge(0x50,6,3,14200000,14400000);
        injectWire(subOriginal);
        IcomCivBackendTestAccess::pump(backend);
        check(IcomCivBackendTestAccess::borrowedSlot(backend,0)==4
              && IcomCivBackendTestAccess::borrowedSlot(backend,1)==3
              && IcomCivBackendTestAccess::snapshots(backend)==2,
              "same-band MAIN/SUB borrow different global slots with independent snapshots");
        check(IcomCivBackendTestAccess::confirmation(backend,subOriginal)
              == buildFrameSub(0x50,0x27,0x1e,std::array<std::uint8_t,2>{6,3})
              && IcomCivBackendTestAccess::key(backend,mainOriginal)!=IcomCivBackendTestAccess::key(backend,subOriginal),
              "preset confirmation and coalescing retain both range and slot");
        IcomCivBackendTestAccess::clearCommands(backend);
        backend.setPanCenter(QStringLiteral("1"),14340000,IRadioBackend::PanCenterIntent::Drag);
        check(backend.removePanadapter(QStringLiteral("1")) && radio.panadapters().size()==2,
              "owned Dualwatch close waits for the radio to confirm off");
        IcomCivBackendTestAccess::events();
        check(!IcomCivBackendTestAccess::sent(backend,cmdScopeFixedEdge(0x50,6,3,14240000,14440000)),
              "closing SUB cancels a queued scope gesture before it can alter a preset");
        IcomCivBackendTestAccess::inject(backend,{0x07,0xc2,0});
        check(radio.panadapters().size()==1 && radio.slices().size()==1,
              "confirmed close retires SUB without recreating it from ongoing scope frames");
        IcomCivBackendTestAccess::inject(backend,makeScope(1,3,14200000,14400000));
        check(radio.panadapters().size()==1, "hidden SUB scope does not resurrect a closed pane");
        check(!backend.removePanadapter(QStringLiteral("0")), "MAIN receive path cannot be retired");
        backend.createPanadapter();
        IcomCivBackendTestAccess::inject(backend,{0x07,0xc2,1});
        backend.invokeExtension(QStringLiteral("icom"),QStringLiteral("receiver.dualwatch"),88,QStringLiteral("on"));
        check(backend.removePanadapter(QStringLiteral("1")) && radio.panadapters().size()==1
              && IcomCivBackendTestAccess::dualWatch(backend)==true,
              "explicit Dualwatch ownership survives closing SUB");
        IcomCivBackendTestAccess::clearCommands(backend);
        IcomCivBackendTestAccess::inject(backend,{0x07,0xc2,0});
        backend.createPanadapter();
        backend.invokeExtension(QStringLiteral("icom"),QStringLiteral("receiver.dualwatch"),89,QStringLiteral("off"));
        IcomCivBackendTestAccess::clearCommands(backend);
        IcomCivBackendTestAccess::inject(backend,{0x07,0xc2,0});
        IcomCivBackendTestAccess::inject(backend,makeScope(1,3,14200000,14400000));
        check(radio.panadapters().size()==1 && IcomCivBackendTestAccess::dualWatch(backend)==false,
              "explicit Dualwatch off cancels a pending pane open instead of fighting the operator");

    }
    return failures ? 1 : 0;
}
