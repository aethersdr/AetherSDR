// Socket-free native selected-receiver routing through the production model/tap.
#include "TestSettingsProfile.h"
#include "asr/AsrEngine.h"
#include "core/backends/IRadioBackend.h"
#include "gui/AsrAudioTap.h"
#include "models/DecoderAudioModel.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include <QCoreApplication>
#include <QEvent>
#include <cstdio>
#include <memory>
using namespace AetherSDR;
namespace {
int failures = 0;
void check(bool good, const char* description) {
    if (!good) { ++failures; std::fprintf(stderr, "FAIL: %s\n", description); }
}
class Source final : public IRadioBackend {
public:
    bool exportAudio = true;
    bool live = false;
    RadioCapabilities capabilities() const override {
        RadioCapabilities caps;
        caps.maxSlices = 8;
        if (exportAudio) { caps.receiveAudioExport = ReceiveAudioExport{{24000, 48000}, 8}; }
        return caps;
    }
    bool ownsRxAudio() const override { return true; }
    void connectRadio(const RadioConnectRequest&) override { live = true; emit connected(); }
    void disconnectRadio() override { live = false; emit disconnected(); }
    bool isConnected() const override { return live; }
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const TxCoordinator::Operation&, const TxCoordinator::Completion&) override {}
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
};
class SilentBackend final : public IAsrBackend {
public:
    bool load(const QString&, QString*) override { return true; }
    bool isLoaded() const override { return true; }
    AsrTranscript transcribe(const std::vector<float>&, QString*) override { return {}; }
    void unload() override {}
};
void drain() { QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall); }
}
int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("asr-native-receiver"));
    QCoreApplication app(argc, argv);
    RadioModel radio;
    auto backend = std::make_unique<Source>();
    Source* source = backend.get();
    radio.setBackendForTest(std::move(backend), QStringLiteral("native-test"));
    source->connectRadio({});
    for (int id : {3, 7}) {
        SliceDelta delta;
        delta.panId = QStringLiteral("native-test-pan");
        delta.frequency = 14.1;
        delta.mode = QStringLiteral("USB");
        delta.inUse = true;
        emit source->sliceChanged(id, delta);
    }
    SliceModel* a = radio.slice(3);
    SliceModel* b = radio.slice(7);
    check(a && b, "normalized receiver models exist");
    if (!a || !b) { return 1; }
    DecoderAudioModel route(radio, DecoderAudioModel::Consumer::NativeAsr);
    AsrEngine asr([] { return std::make_unique<SilentBackend>(); });
    AsrAudioTap tap(nullptr, &asr);
    int chunks = 0;
    int resets = 0;
    int rate = 0;
    QVector<float> delivered;
    QObject::connect(&asr, &AsrEngine::requestProcess, &app,
        [&](const QVector<float>& pcm, int hz) { ++chunks; delivered = pcm; rate = hz; });
    QObject::connect(&asr, &AsrEngine::requestReset, &app, [&] { ++resets; });
    route.setSlice(a);
    tap.setNativeInput(&route);
    tap.setTapPoint(AsrTapPoint::PostDsp);
    tap.setEnabled(true);
    check(tap.usesNativeInput() && tap.tapPoint() == AsrTapPoint::PostDsp,
          "native input does not rewrite the saved presentation preference");
    PcmProducer pa, pb, mix;
    pa.start(PcmPurpose::Slice, 3, {48000, PcmLayout::Mono});
    pb.start(PcmPurpose::Slice, 7, {24000, PcmLayout::Mono});
    mix.start();
    emit source->sliceAudioFrameReady(7, *pb.produce({0.8f}));
    emit source->audioFrameReady(*mix.produce({0.9f, 0.9f}));
    emit source->sliceAudioFrameReady(3, *pa.produce({0.125f, 0.25f}));
    drain();
    check(chunks == 1 && rate == 48000 && delivered == QVector<float>({0.125f, 0.25f}),
          "selected native rate/audio survives with no output device and no sibling/mix input");
    a->setAudioGain(0); a->setAudioMute(true); a->setAudioPan(0);
    emit source->sliceAudioFrameReady(3, *pa.produce({0.375f}));
    drain();
    check(chunks == 2 && delivered == QVector<float>({0.375f}), "monitor mute/gain/pan do not affect ASR");
    for (const QString& mode : {QStringLiteral("AM"), QStringLiteral("SAM")}) {
        const int beforeMode = chunks;
        SliceDelta changed; changed.mode = mode;
        emit source->sliceChanged(3, changed);
        pa.start(PcmPurpose::Slice, 3, {48000, PcmLayout::Mono});
        emit source->sliceAudioFrameReady(3, *pa.produce({0.45f}));
        drain();
        check(chunks == beforeMode + 1 && delivered == QVector<float>({0.45f}) && tap.usesNativeInput(),
            "AM and SAM selected audio use native pre-monitor ASR routing");
    }
    chunks -= 2; // Keep the lifecycle census below relative to its original two blocks.
    emit source->sliceAudioFrameReady(3, *pa.produce({0.5f}));
    const int beforeSwitch = resets;
    route.setSlice(b);
    drain();
    check(chunks == 2 && resets > beforeSwitch, "selection retires queued old receiver input");
    emit source->sliceAudioFrameReady(7, *pb.produce({0.6f}));
    drain();
    check(chunks == 3 && rate == 24000 && delivered == QVector<float>({0.6f}), "selected sibling preserves its rate");
    const int beforePark = resets;
    emit source->sliceAudioFrameReady(7, *pb.produce({0.7f}));
    SliceDelta parked; parked.inCapture = false;
    emit source->sliceChanged(7, parked);
    drain();
    check(chunks == 3 && resets > beforePark && route.routeStatus() == DecoderAudioModel::RouteStatus::Inactive,
          "park closes input immediately and drops its pending audio");
    emit source->sliceAudioFrameReady(3, *pa.produce({0.8f}));
    drain();
    check(chunks == 3, "parked selection never falls back to a live sibling");
    parked.inCapture = true; emit source->sliceChanged(7, parked);
    pb.start(PcmPurpose::Slice, 7, {48000, PcmLayout::Mono});
    emit source->sliceAudioFrameReady(7, *pb.produce({0.2f}));
    drain();
    check(chunks == 4 && rate == 48000, "unpark admits new producer epoch");
    emit source->sliceAudioFrameReady(7, *pb.produce({0.3f}));
    pb.invalidate();
    drain();
    check(chunks == 4, "lease revoked between enqueue and drain cannot reach ASR");
    pb.start(PcmPurpose::Slice, 7, {48000, PcmLayout::Mono});
    emit source->sliceAudioFrameReady(7, *pb.produce({0.4f}));
    source->disconnectRadio();
    drain();
    check(chunks == 4, "backend disconnect rejects queued selected input before the lifecycle notification");
    // setBackendForTest injects PCM/receiver wiring, not the normal backend
    // lifecycle connections. Supply the normalized edge used in production.
    const int beforeDisconnect = resets;
    emit radio.connectionStateChanged(false);
    drain();
    check(chunks == 4 && resets > beforeDisconnect
        && route.routeStatus() == DecoderAudioModel::RouteStatus::Inactive,
          "normalized disconnect resets and withdraws the selected route");
    tap.setEnabled(false);
    std::printf("Native ASR receiver: %d failures\n", failures);
    return failures ? 1 : 0;
}
