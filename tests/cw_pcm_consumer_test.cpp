// The A5 CW consumer boundary and real receive decoder, without radio/TX.
// Checks stereo-averaged carrier/interference, Morse envelope timing, and
// rejection of a retired receiver before the replacement word arrives.
#include "CwPcmFixture.h"
#include "TestSettingsProfile.h"
#include "core/backends/IRadioBackend.h"
#include "models/DecoderAudioModel.h"
#include "models/CwRxModel.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QCoreApplication>
#include <QEvent>
#include <QThread>
#include <QElapsedTimer>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <numbers>

using namespace AetherSDR;

namespace {
int failures = 0;
int checks = 0;
void check(bool value, const char* message)
{
    ++checks;
    if (!value) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
}

// Inject only the transport observations; RadioModel, selection, inbox,
// revocation and conversion are the real production implementations.
class Source final : public IRadioBackend {
public:
    RadioCapabilities capabilities() const override { return {}; }
    bool ownsRxAudio() const override { return true; }
    void connectRadio(const RadioConnectRequest&) override { live = true; emit connected(); }
    void disconnectRadio() override { live = false; emit disconnected(); }
    bool isConnected() const override { return live; }
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const TxCoordinator::Operation&,
                   const TxCoordinator::Completion&) override {} // no TX transport
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
    bool live = false;
};

struct Capture {
    QVector<float> samples;
    QVector<float> nativeSamples;
    PcmFormat nativeFormat;
    PcmEpochLease source;
    double delaySeconds = 0;
    int resets = 0;
};

struct Fixture {
    RadioModel radio;
    Source* source = nullptr;
    DecoderAudioModel consumer{radio, DecoderAudioModel::Consumer::Cw};
    Capture captured;
    CwRxModel decoder;
    QString decoded;

    Fixture()
    {
        auto backend = std::make_unique<Source>();
        source = backend.get();
        radio.setBackendForTest(std::move(backend), QStringLiteral("rtl"));
        source->connectRadio({});
        QObject::connect(&consumer, &DecoderAudioModel::sourceReset, &consumer, [this] {
            ++captured.resets;
            decoder.reset();
        });
        QObject::connect(&consumer, &DecoderAudioModel::nativePcmReady, &consumer,
                         [this](const PcmFrame& frame) {
            captured.nativeSamples += frame.samples();
            captured.nativeFormat = frame.stream().format;
        });
        QObject::connect(&consumer, &DecoderAudioModel::nativePcmReady, &decoder, &CwRxModel::feed);
        QObject::connect(&consumer, &DecoderAudioModel::pcmReady, &decoder, &CwRxModel::feedFixed24);
        QObject::connect(&decoder, &CwRxModel::textDecoded, &consumer,
                         [this](const QString& text, float) { decoded += text; });
        QObject::connect(&consumer, &DecoderAudioModel::pcmReady, &consumer,
                         [this](const DecoderPcmBlock& block) {
            captured.samples += block.samples;
            captured.source = block.source;
            captured.delaySeconds = double(block.groupDelayInputFrames) / block.inputSampleRateHz;
        });
    }

    SliceModel* addSlice(int id)
    {
        SliceDelta delta;
        delta.panId = QStringLiteral("cw-consumer-pan");
        delta.mode = QStringLiteral("CW");
        delta.frequency = 14.02;
        delta.inUse = true;
        emit source->sliceChanged(id, delta);
        return radio.slice(id);
    }

    void feed(PcmProducer& producer, int slot, const QVector<float>& samples,
              int channels, bool uneven)
    {
        constexpr std::array<int, 4> kChunks{17, 509, 73, 1024};
        int chunk = 0;
        for (qsizetype offset = 0; offset < samples.size();) {
            const qsizetype count = std::min<qsizetype>(
                (uneven ? kChunks[chunk++ % kChunks.size()] : 4096) * channels,
                samples.size() - offset);
            const auto frame = producer.produce(samples.mid(offset, count));
            check(frame.has_value(), "CW fixture produces a valid frame");
            if (!frame) { return; }
            emit source->sliceAudioFrameReady(slot, *frame);
            QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
            offset += count;
        }
    }
};

double carrierAmplitude(const QVector<float>& samples, double frequency, double delay)
{
    // Interior of the first dash (0.78–0.96 s), away from filter transients.
    const int first = int((0.81 + delay) * 24000);
    const int count = 2400;
    if (samples.size() < first + count) { return -1; }
    double sine = 0;
    double cosine = 0;
    for (int i = 0; i < count; ++i) {
        const double phase = 2 * std::numbers::pi * frequency * i / 24000;
        sine += samples[first + i] * std::sin(phase);
        cosine += samples[first + i] * std::cos(phase);
    }
    return 2 * std::hypot(sine, cosine) / count;
}

bool expectedMarks(const Capture& captured)
{
    // Independent hand-counted SOS edges in seconds, not fixture-derived.
    constexpr std::array<double, 18> kEdges{
        .30, .36, .42, .48, .54, .60, .78, .96, 1.02, 1.20,
        1.26, 1.44, 1.62, 1.68, 1.74, 1.80, 1.86, 1.92};
    QVector<double> edges;
    bool active = false;
    constexpr int kWindow = 120; // 5 ms; exactly three carrier periods.
    for (qsizetype first = 0; first + kWindow <= captured.samples.size(); first += kWindow) {
        double energy = 0;
        for (int i = 0; i < kWindow; ++i) {
            energy += double(captured.samples[first + i]) * captured.samples[first + i];
        }
        const bool mark = energy / kWindow > .01;
        if (mark != active) {
            edges.append(double(first) / 24000 - captured.delaySeconds);
            active = mark;
        }
    }
    if (edges.size() != kEdges.size()) { return false; }
    for (qsizetype i = 0; i < edges.size(); ++i) {
        if (std::abs(edges[i] - kEdges[i]) > .010) { return false; }
    }
    return !active;
}

Capture converted(PcmFormat format, bool uneven)
{
    Fixture fixture;
    fixture.consumer.setSlice(fixture.addSlice(3));
    fixture.consumer.setEnabled(true);
    PcmProducer producer;
    producer.start(PcmPurpose::Slice, 3, format);
    fixture.feed(producer, 3, CwPcmFixture::sos(format), format.channels(), uneven);
    return fixture.captured;
}

void waveformContract()
{
    for (int rate : {24000, 48000}) {
        for (PcmLayout layout : {PcmLayout::Mono, PcmLayout::Stereo}) {
            const Capture regular = converted({rate, layout}, false);
            const Capture uneven = converted({rate, layout}, true);
            check(regular.samples == uneven.samples, "CW waveform is invariant under producer chunking");
            check(regular.nativeFormat == PcmFormat{rate, layout}
                  && regular.nativeSamples == CwPcmFixture::sos({rate, layout}),
                  "native DeepFist input preserves original format and samples");
            check(expectedMarks(regular), "CW dit/dah and letter gaps survive rate/layout conversion");
            check(std::abs(carrierAmplitude(regular.samples, 600, regular.delaySeconds) - .4) < .015,
                  "CW carrier frequency and level survive mono/stereo conversion");
            check(carrierAmplitude(regular.samples, 1100, regular.delaySeconds) < .002,
                  "opposite stereo interferer cancels before CW consumption");
        }
    }
}

void retiredReceiverCannotOwnNextWord()
{
    Fixture fixture;
    fixture.consumer.setSlice(fixture.addSlice(3));
    fixture.addSlice(7);
    fixture.consumer.setEnabled(true);
    PcmProducer retired;
    retired.start(PcmPurpose::Slice, 3, {48000, PcmLayout::Mono});
    fixture.feed(retired, 3, QVector<float>(2048, .8f), 1, false);
    const auto queued = retired.produce(QVector<float>(1024, .8f));
    emit fixture.source->sliceAudioFrameReady(3, *queued);
    emit fixture.source->sliceRemoved(3);
    fixture.consumer.setSlice(fixture.addSlice(3));
    fixture.captured.samples.clear();
    const int resets = fixture.captured.resets;
    // Producer deliberately stays current. Route retirement, not revocation,
    // must prevent its next block from taking over the replacement slice.
    fixture.feed(retired, 3, QVector<float>(1024, .8f), 1, false);
    check(fixture.captured.samples.isEmpty(), "retired CW receiver cannot deliver queued or new mark samples");
    PcmProducer wrongSlice;
    wrongSlice.start(PcmPurpose::Slice, 7, {24000, PcmLayout::Mono});
    fixture.feed(wrongSlice, 7, QVector<float>(1024, .8f), 1, false);
    check(fixture.captured.samples.isEmpty(), "another CW slice cannot replace the selected receiver");
    PcmProducer replacement;
    replacement.start(PcmPurpose::Slice, 3, {48000, PcmLayout::Stereo});
    fixture.feed(replacement, 3, CwPcmFixture::sos({48000, PcmLayout::Stereo}), 2, true);
    check(expectedMarks(fixture.captured) && fixture.captured.resets > resets,
          "replacement CW word starts with fresh filter/timing history after retirement");
}
void pump(int milliseconds)
{
    QElapsedTimer timer;
    timer.start();
    do {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    } while (timer.elapsed() < milliseconds);
}

QVector<float> letterV(PcmFormat format)
{
    // Independent Morse fixture: V is three dits then one dah, 20 WPM.
    QVector<float> mono(format.sampleRateHz / 2, 0);
    auto silence = [&](int units) {
        mono.resize(mono.size() + qRound(units * .06 * format.sampleRateHz));
    };
    for (int letter = 0; letter < 12; ++letter) {
        if (letter) { silence(letter % 3 ? 3 : 7); }
        for (int mark = 0; mark < 4; ++mark) {
            if (mark) { silence(1); }
            const int count = qRound((mark == 3 ? 3 : 1) * .06 * format.sampleRateHz);
            for (int i = 0; i < count; ++i) {
                const double edge = std::min({1.0, i / (.005 * format.sampleRateHz),
                                              (count - 1 - i) / (.005 * format.sampleRateHz)});
                mono.append(.5f * edge * std::sin(2 * std::numbers::pi * 600 * i / format.sampleRateHz));
            }
        }
    }
    silence(35);
    QVector<float> samples;
    samples.reserve(mono.size() * format.channels());
    for (qsizetype i = 0; i < mono.size(); ++i) {
        const float other = .1f * std::sin(2 * std::numbers::pi * 1100 * i / format.sampleRateHz);
        samples.append(mono[i] + (format.channels() == 2 ? other : 0));
        if (format.channels() == 2) { samples.append(mono[i] - other); }
    }
    return samples;
}

void actualDecodeAndRetirement()
{
    for (int rate : {24000, 48000}) {
        for (PcmLayout layout : {PcmLayout::Mono, PcmLayout::Stereo}) {
            const PcmFormat format{rate, layout};
            Fixture fixture;
            fixture.consumer.setSlice(fixture.addSlice(3));
            fixture.consumer.setEnabled(true);
            fixture.decoder.setPitchRange(550, 650);
            fixture.decoder.setSpeedRange(18, 22);
            fixture.decoder.start();
            PcmProducer producer;
            producer.start(PcmPurpose::Slice, 3, format);
            const QVector<float> samples = letterV(format);
            const qsizetype chunk = rate / 10 * format.channels();
            for (qsizetype offset = 0; offset < samples.size(); offset += chunk) {
                const auto frame = producer.produce(samples.mid(offset, chunk));
                emit fixture.source->sliceAudioFrameReady(3, *frame);
                pump(10);
            }
            pump(300);
            std::fprintf(stderr, "CW RX %d Hz/%d ch: %s\n", rate, format.channels(),
                         qPrintable(fixture.decoded.simplified()));
            check(fixture.decoded.count('V') >= 3,
                  "production selected-source adapter/facade/worker recognizes Morse V");
            check(fixture.decoder.estimatedPitch() > 550 && fixture.decoder.estimatedPitch() < 650,
                  "actual worker reports the selected CW tone");
            fixture.decoder.lockPitch(true);
            fixture.decoder.lockSpeed(true);
            const float pitch = fixture.decoder.estimatedPitch();
            const float speed = fixture.decoder.estimatedSpeed();
#ifdef HAVE_DEEPFIST
            check(fixture.decoder.selectBackend("deepfist"), "optional backend selection succeeds");
            check(fixture.decoder.selectBackend("ggmorse"), "default backend restoration succeeds");
            check(fixture.decoder.estimatedPitch() == pitch && fixture.decoder.estimatedSpeed() == speed,
                  "backend switch retains independently locked estimates");
#endif
            fixture.decoder.start();
            fixture.decoder.reset();
            check(fixture.decoder.estimatedPitch() == pitch && fixture.decoder.estimatedSpeed() == speed,
                  "source reset retains operator locks");
            fixture.decoder.lockPitch(false);
            fixture.decoder.lockSpeed(false);
            fixture.decoder.stop();
            pump(20);
            check(fixture.decoder.estimatedPitch() == 0 && fixture.decoder.estimatedSpeed() == 0,
                  "stop clears unlocked estimates");

            // Fill the real worker while withholding owner-thread result delivery.
            // Revocation must reject already-posted text as well as buffered PCM.
            fixture.decoder.start();
            DecoderPcmAdapter adapter;
            adapter.selectRoute(DecoderPcmAdapter::RouteLane::NativeSlice, 3);
            for (qsizetype offset = 0; offset < samples.size(); offset += chunk) {
                const auto frame = producer.produce(samples.mid(offset, chunk));
                const auto block = adapter.accept(*frame);
                if (block) { fixture.decoder.feedFixed24(*block); }
                QThread::msleep(10);
            }
            QThread::msleep(200);
            producer.invalidate();
            fixture.decoded.clear();
            pump(100);
            check(fixture.decoded.isEmpty(), "revoked source cannot publish already queued CW text");
            fixture.decoder.stop();
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settings(QStringLiteral("cw-pcm-consumer"));
    QCoreApplication app(argc, argv);
    waveformContract();
    actualDecodeAndRetirement();
    retiredReceiverCannotOwnNextWord();
    std::printf("cw_pcm_consumer_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
