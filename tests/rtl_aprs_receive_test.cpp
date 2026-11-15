// Socket-free end-to-end RX regression: independent AX.25/Bell202 fixture ->
// CU8 callbacks -> real RTL worker/backend -> native slice PCM -> modem model.
// Only USB device operations are injected. No radio or sound device is opened.
#include "TestSettingsProfile.h"
#include "Ax25ReceiveFixture.h"
#include "core/backends/rtl/RtlSdrBackend.h"
#include "core/backends/rtl/RtlSdrWorker.h"
#include "models/Ax25ReceiveModel.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <numbers>

using namespace AetherSDR;
using Transaction = rtl::RtlCaptureTransaction;
namespace AetherSDR::rtl {
struct RtlCaptureBackendTestAccess {
    static void start(RtlSdrBackend& backend, std::unique_ptr<RtlSdrWorker::Device> device)
    {
        backend.m_receiverCapacity = 2;
        backend.m_capture = RtlCaptureTransaction({8, 2});
        backend.m_requested.hardware = {144290000, 2400000, 0, 0, 0, 240};
        backend.m_requested.receivers = {
            {{3, 144390000, -8000, 8000, 0, 3000, 3000}, Transaction::Mode::Fm, 0, 50, true},
            {{7, 144590000, -8000, 8000, 0, 3000, 3000}, Transaction::Mode::Fm, 0, 50, true}};
        backend.startCapture(std::make_unique<RtlSdrWorker>(std::move(device), nullptr, 2));
    }
    static std::optional<RtlReceivePipeline::Diagnostics> diagnostics(const RtlSdrBackend& backend)
    {
        if (!backend.m_worker) { return {}; }
        return backend.m_worker->diagnostics();
    }
    static quint64 revision(const RtlSdrBackend& backend) { return backend.m_published.revision; }
};
}
namespace {
using Clock = std::chrono::steady_clock;
constexpr int kSampleRate = 2400000;
constexpr int kCallbackFrames = 8192;
int failures = 0;
void check(bool condition, const char* message)
{
    if (!condition) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
template<class Predicate> bool waitFor(Predicate predicate, int milliseconds = 15000)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (!predicate() && elapsed.elapsed() < milliseconds) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    return predicate();
}
struct DeviceState {
    std::mutex mutex;
    std::condition_variable changed;
    bool playing = false;
    bool canceled = false;
    std::atomic<bool> finished{false};
    std::atomic<int> callbacks{0};
    Transaction::Hardware hardware;
    QByteArray iq;
};
class RecordedDevice final : public rtl::RtlSdrWorker::Device {
public:
    explicit RecordedDevice(std::shared_ptr<DeviceState> state) : m_state(std::move(state)) {}
    bool set(Transaction::Control control, std::int64_t value) override
    {
        switch (control) {
        case Transaction::Control::DirectSampling: m_state->hardware.directSampling = int(value); break;
        case Transaction::Control::SampleRate: m_state->hardware.sampleRateHz = std::uint32_t(value); break;
        case Transaction::Control::Ppm: m_state->hardware.ppm = int(value); break;
        case Transaction::Control::OffsetTuning: m_state->hardware.offsetTuning = int(value); break;
        case Transaction::Control::Center: m_state->hardware.centerHz = std::uint32_t(value); break;
        case Transaction::Control::Gain: m_state->hardware.gainTenths = int(value); break;
        }
        return true;
    }
    std::optional<Transaction::Hardware> read() override { return m_state->hardware; }
    bool resetBuffer() override { return true; }
    int readAsync(Callback callback, void* context) override
    {
        std::unique_lock lock(m_state->mutex);
        m_state->changed.wait(lock, [&] { return m_state->playing || m_state->canceled; });
        const auto start = Clock::now();
        for (qsizetype offset = 0; offset < m_state->iq.size() && !m_state->canceled;) {
            const qsizetype count = std::min<qsizetype>(2 * kCallbackFrames, m_state->iq.size() - offset);
            auto* bytes = reinterpret_cast<unsigned char*>(m_state->iq.data() + offset);
            lock.unlock();
            callback(bytes, static_cast<std::uint32_t>(count), context);
            ++m_state->callbacks;
            lock.lock();
            offset += count;
            const auto deadline = start + std::chrono::nanoseconds(
                (offset / 2) * 1000000000LL / kSampleRate);
            m_state->changed.wait_until(lock, deadline, [&] { return m_state->canceled; });
        }
        m_state->finished = true;
        m_state->changed.wait(lock, [&] { return m_state->canceled; });
        return 0;
    }
    void cancelAsync() override
    {
        std::lock_guard lock(m_state->mutex);
        m_state->canceled = true;
        m_state->changed.notify_all();
    }
private:
    std::shared_ptr<DeviceState> m_state;
};
QByteArray captureFixture(std::uint32_t centerHz)
{
    QByteArray iq = test::ax25rx::cu8FmIq(kSampleRate, 2500, 144390000.0 - centerHz);
    double phase = 0;
    // A second real RF receiver carries a different continuous audio tone.
    // Its PCM must exist without entering the selected receiver's decoder.
    for (qsizetype index = 0; index < iq.size() / 2; ++index) {
        const double tone = std::sin(2 * std::numbers::pi * 440 * double(index) / kSampleRate);
        phase = std::remainder(phase + 2 * std::numbers::pi * (144590000.0 - centerHz + 2500 * tone) / kSampleRate,
                               2 * std::numbers::pi);
        const double i = (static_cast<unsigned char>(iq[2 * index]) - 127.5) / 127.5;
        const double q = (static_cast<unsigned char>(iq[2 * index + 1]) - 127.5) / 127.5;
        iq[2 * index] = char(std::clamp(std::lround(127.5 + 127.5 * (0.65 * i + 0.2 * std::cos(phase))), 0L, 255L));
        iq[2 * index + 1] = char(std::clamp(std::lround(127.5 + 127.5 * (0.65 * q + 0.2 * std::sin(phase))), 0L, 255L));
    }
    return iq;
}
}

int runScenario(QCoreApplication& app, bool changeMonitor)
{
    const int initialFailures = failures;
    auto device = std::make_shared<DeviceState>();
    check(test::ax25rx::fcs(test::ax25rx::frame()) == 0x1e9b, "independent fixture retains its known FCS");
    RadioModel radio;
    auto backend = std::make_unique<rtl::RtlSdrBackend>();
    rtl::RtlSdrBackend* source = backend.get();
    radio.setBackendForTest(std::move(backend), QStringLiteral("rtl"));
    QString error;
    QObject::connect(source, &IRadioBackend::connectionError, &app, [&](const QString& value) { error = value; });
    quint64 selectedFrames = 0;
    quint64 otherFrames = 0;
    quint64 convertedSamples = 0;
    int wrongInput = 0;
    int decoded = 0;
    int resets = 0;
    int nativeDiscontinuities = 0;
    quint64 nativeNextSample = 0;
    int sourceIdentityChanges = 0;
    QJsonArray nativeBoundaries;
    PcmEpochLease selectedSource;
    QByteArray nativePcm;
    QByteArray decoderPcm;
    const QString dumpRoot = qEnvironmentVariable("AETHER_APRS_TEST_PCM_DIR");
    QObject::connect(&radio, &RadioModel::backendSliceAudioFrameReady, &app,
        [&](int id, const PcmFrame& frame) {
            if (!frame.current() || frame.stream().purpose != PcmPurpose::Slice
                || frame.stream().sliceId != id || frame.stream().format.sampleRateHz != 48000) {
                ++wrongInput;
            }
            if (id == 3) {
                if (selectedFrames && selectedSource.stream() != frame.stream()) { ++sourceIdentityChanges; }
                if (frame.discontinuity() || frame.firstSample() != nativeNextSample) {
                    ++nativeDiscontinuities;
                    const auto& stream = frame.stream();
                    nativeBoundaries.append(QJsonObject{{"first_sample", qint64(frame.firstSample())},
                        {"expected_sample", qint64(nativeNextSample)}, {"explicit_discontinuity", frame.discontinuity()},
                        {"source", qint64(stream.source)}, {"session", qint64(stream.session)},
                        {"format_generation", qint64(stream.formatGeneration)},
                        {"receiver_instance", qint64(stream.receiverInstance)},
                        {"capture_revision", qint64(rtl::RtlCaptureBackendTestAccess::revision(*source))}});
                    std::fprintf(stderr, "NATIVE discontinuity=%d first=%llu expected=%llu frames=%lld\n",
                        frame.discontinuity(), frame.firstSample(), nativeNextSample, qint64(frame.frameCount()));
                }
                nativeNextSample = frame.firstSample() + frame.frameCount();
                selectedFrames += frame.frameCount(); selectedSource = frame.epochLease();
                if (!dumpRoot.isEmpty()) {
                    nativePcm.append(reinterpret_cast<const char*>(frame.samples().constData()),
                        frame.samples().size() * qsizetype(sizeof(float)));
                }
            }
            if (id == 7) { otherFrames += frame.frameCount(); }
        });
    rtl::RtlCaptureBackendTestAccess::start(*source, std::make_unique<RecordedDevice>(device));
    if (!waitFor([&] { return source->isConnected() && radio.slice(3) && radio.slice(7); })) {
        check(false, "actual backend accepts the injected capture and publishes both sparse receivers");
        source->disconnectRadio();
        return 1;
    }
    // Startup deliberately places FM away from converter DC. Generate the
    // independent RF carriers against actual device readback, not requested RF.
    const std::uint32_t captureCenterHz = device->hardware.centerHz;
    device->iq = captureFixture(captureCenterHz);
    const QByteArray fixtureHash = QCryptographicHash::hash(device->iq, QCryptographicHash::Sha256).toHex();
    Ax25ReceiveModel modem(radio);
    modem.configure(ax25DemodConfigForProfile(Ax25ModemProfile::Vhf1200));
    modem.setSlice(radio.slice(3));
    modem.setEnabled(true);
    radio.setPcAudioEnabled(false);
    check(radio.slice(3)->audioGain() == 0 && radio.slice(3)->audioMute()
          && radio.slice(7)->audioGain() == 0 && radio.slice(7)->audioMute(),
          "both receiver monitors are confirmed muted at zero gain before playback");
    QObject::connect(&modem, &Ax25ReceiveModel::sourceReset, &app, [&] {
        ++resets;
        const auto& stream = selectedSource.stream();
        std::fprintf(stderr, "RESET count=%d native=%llu converted=%llu source=%llu session=%llu generation=%llu instance=%llu current=%d\n",
            resets, selectedFrames, convertedSamples, stream.source, stream.session,
            stream.formatGeneration, stream.receiverInstance, selectedSource.current());
    });
    QObject::connect(&modem, &Ax25ReceiveModel::pcmReady, &app,
        [&](const DecoderPcmBlock& block, const Ax25ReceiveContext& context) {
        if (!context.current() || !block.current() || block.inputSampleRateHz != 48000
            || block.source.stream().sliceId != 3 || context.source.stream() != block.source.stream()) {
            ++wrongInput;
        }
        convertedSamples += block.samples.size();
        if (!dumpRoot.isEmpty()) {
            decoderPcm.append(reinterpret_cast<const char*>(block.samples.constData()),
                block.samples.size() * qsizetype(sizeof(float)));
        }
    });
    QObject::connect(&modem, &Ax25ReceiveModel::frameDecoded, &app,
        [&](const Ax25DecodedFrame& frame, const Ax25ReceiveContext& context) {
        ++decoded;
        check(context.current() && context.source.stream() == selectedSource.stream()
              && frame.fcsOk && frame.ax25FrameNoFcs == test::ax25rx::frame(),
              "real RTL FM native48 -> selected mono24 model recovers exact AX25 packet and FCS");
    });
    {
        std::lock_guard lock(device->mutex);
        device->playing = true;
        device->changed.notify_all();
    }
    bool monitorChanged = false;
    const bool completed = waitFor([&] {
        if (changeMonitor && !monitorChanged && selectedFrames > 24000) {
            monitorChanged = true;
            std::fprintf(stderr, "MONITOR change native=%llu converted=%llu resets=%d\n", selectedFrames, convertedSamples, resets);
            radio.slice(3)->setAudioGain(50);
            radio.slice(3)->setAudioGain(0);
            radio.slice(7)->setAudioPan(0);
            radio.slice(7)->setAudioPan(100);
            radio.setPcAudioEnabled(false);
        }
        return device->finished.load() && decoded > 0;
    });
    const auto diagnostics = rtl::RtlCaptureBackendTestAccess::diagnostics(*source);
    check(completed && error.isEmpty(), "paced CU8 capture completes without backend failure");
    check(decoded == 1, "only one exact packet emerges from the selected receiver");
    check(selectedFrames > 24000 && otherFrames > 24000 && convertedSamples > 12000 && !wrongInput,
          "both real native48 slices run while only selected slice supplies continuous mono24");
    check(resets == nativeDiscontinuities && (changeMonitor || resets == 1),
          "decoder resets exactly for native discontinuities, with one initial reset during steady monitoring-off RX");
    check(sourceIdentityChanges == 0, "monitor controls preserve the selected native source identity");
    check(diagnostics && diagnostics->droppedPackets == 0 && diagnostics->mixerRejectedBlocks == 0
          && diagnostics->mixerConfigurationFailures == 0,
          "actual pipeline retains bounded packets and a valid mixer configuration");
    const int streamResets = resets;
    source->disconnectRadio();
    check(!selectedSource.current(), "disconnect revokes the actual native source lease");
    if (!dumpRoot.isEmpty()) {
        for (const auto& output : {std::pair{QStringLiteral("native48.f32"), nativePcm},
                                   std::pair{QStringLiteral("decoder24.f32"), decoderPcm}}) {
            QFile file(dumpRoot + QLatin1Char('/')
                + (changeMonitor ? QStringLiteral("controls-") : QStringLiteral("steady-")) + output.first);
            check(file.open(QIODevice::WriteOnly) && file.write(output.second) == output.second.size(),
                "requested diagnostic PCM is saved completely");
        }
    }
    const QJsonObject receipt{{"scenario", changeMonitor ? QStringLiteral("monitor-controls") : QStringLiteral("steady-monitor-off")},
        {"fixture_cu8_sha256", QString::fromLatin1(fixtureHash)},
        {"fixture_cu8_bytes", device->iq.size()}, {"capture_rate_hz", kSampleRate},
        {"capture_center_hz", qint64(captureCenterHz)},
        {"expected_fcs", QStringLiteral("1e9b")}, {"native_rate_hz", 48000},
        {"decoder_rate_hz", 24000}, {"selected_native_frames", qint64(selectedFrames)},
        {"other_native_frames", qint64(otherFrames)}, {"converted_samples", qint64(convertedSamples)},
        {"decoded_exact_packets", decoded}, {"stream_resets_before_disconnect", streamResets},
        {"native_discontinuities", nativeDiscontinuities},
        {"native_boundaries", nativeBoundaries}, {"source_identity_changes", sourceIdentityChanges},
        {"mixer_late_frames", diagnostics ? qint64(diagnostics->mixerLateFrames) : -1},
        {"dropped_packets", diagnostics ? qint64(diagnostics->droppedPackets) : -1},
        {"backend_error", error}, {"failures", failures - initialFailures}};
    std::puts(QJsonDocument(receipt).toJson().constData());
    return failures > initialFailures ? 1 : 0;
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("rtl-aprs-receive"));
    if (!profile.isValid()) { return 1; }
    QCoreApplication app(argc, argv);
    runScenario(app, false);
    runScenario(app, true);
    return failures ? 1 : 0;
}
