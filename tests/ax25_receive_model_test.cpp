// Socket-free normalized PCM -> production selected route -> real modem worker.
// The independent fixture does not use the application's transmit encoder.
#include "TestSettingsProfile.h"
#include "Ax25ReceiveFixture.h"
#include "core/backends/IRadioBackend.h"
#include "models/Ax25ReceiveModel.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QPointer>
#include <QSemaphore>
#include <QThread>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <functional>
#include <memory>

namespace AetherSDR {
struct Ax25ReceiveModelTestAccess {
    static QObject* worker(Ax25ReceiveModel& model) { return model.workerForTest(); }
    static auto queues(const Ax25ReceiveModel& model) { return model.queuesForTest(); }
};
}
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
template<class Predicate> bool waitFor(Predicate predicate, int milliseconds = 4000)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < milliseconds) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    return predicate();
}
void settle(int milliseconds = 30)
{
    QElapsedTimer timer;
    timer.start();
    waitFor([&] { return timer.elapsed() >= milliseconds; }, milliseconds + 100);
}
class Source final : public IRadioBackend {
public:
    RadioCapabilities capabilities() const override
    {
        RadioCapabilities caps;
        caps.hasDaxStreams = dax;
        caps.maxSlices = 8;
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
    void setKeying(bool, const TxCoordinator::Operation&,
                   const TxCoordinator::Completion&) override {} // no TX transport
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
    bool live = false;
    bool dax = false;
    std::unique_ptr<PanadapterStream> stream;
};
struct Fixture {
    RadioModel radio;
    Source* source = nullptr;
    SliceModel* a = nullptr;
    SliceModel* b = nullptr;
    explicit Fixture(bool withDax = false)
    {
        auto backend = std::make_unique<Source>();
        source = backend.get();
        source->dax = withDax;
        if (withDax) {
            source->stream = std::make_unique<PanadapterStream>();
        }
        radio.setBackendForTest(std::move(backend), QStringLiteral("rtl"), source->stream.get());
        source->connectRadio({});
        a = add(3);
        b = add(7);
    }
    SliceModel* add(int id)
    {
        SliceDelta delta;
        delta.panId = QStringLiteral("aprs-pan");
        delta.frequency = 144.390;
        delta.mode = QStringLiteral("FM");
        delta.inUse = true;
        emit source->sliceChanged(id, delta);
        return radio.slice(id);
    }
};
void bind(Ax25ReceiveModel& model, Fixture& fixture)
{
    model.configure(ax25DemodConfigForProfile(Ax25ModemProfile::Vhf1200));
    model.setSlice(fixture.a);
    model.setEnabled(true);
}
void feed(Fixture& fixture, PcmProducer& producer, const QVector<float>& samples,
          int channels, const std::function<void(const PcmFrame&)>& deliver = {})
{
    constexpr int chunks[] = {1, 17, 255, 11, 1024, 513, 37};
    qsizetype offset = 0;
    int chunk = 0;
    while (offset < samples.size()) {
        const qsizetype count = std::min<qsizetype>(chunks[chunk++ % 7] * channels,
                                                  samples.size() - offset);
        const auto frame = producer.produce(samples.mid(offset, count));
        check(frame.has_value(), "fixture creates a valid typed frame");
        if (!frame) {
            return;
        }
        if (deliver) {
            deliver(*frame);
        } else {
            emit fixture.source->sliceAudioFrameReady(frame->stream().sliceId, *frame);
        }
        offset += count;
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
}
void packetMatrix(QCoreApplication& app)
{
    check(test::ax25rx::fcs(test::ax25rx::frame()) == 0x1e9b,
          "independent fixed AX25 payload has expected X25 FCS 9b1e on wire");
    for (const int rate : {24000, 48000}) {
        for (const int channels : {1, 2}) {
            Fixture fixture;
            Ax25ReceiveModel model(fixture.radio);
            bind(model, fixture);
            fixture.radio.setPcAudioEnabled(false);
            fixture.a->setAudioGain(0);
            fixture.a->setAudioMute(true);
            int frames = 0;
            int pcm = 0;
            int resets = 0;
            Ax25ReceiveContext retained;
            QObject::connect(&model, &Ax25ReceiveModel::sourceReset, &app, [&] { ++resets; });
            QObject::connect(&model, &Ax25ReceiveModel::pcmReady, &app,
                [&](const DecoderPcmBlock& block, const Ax25ReceiveContext& context) {
                    ++pcm;
                    retained = context;
                    check(context.current() && block.current()
                          && block.source.stream().sliceId == 3
                          && block.inputSampleRateHz == rate,
                          "mono24 output retains native selected source identity and rate");
                });
            QObject::connect(&model, &Ax25ReceiveModel::frameDecoded, &app,
                [&](const Ax25DecodedFrame& frame, const Ax25ReceiveContext& context) {
                    ++frames;
                    check(context.current() && frame.fcsOk
                          && frame.ax25FrameNoFcs == test::ax25rx::frame(),
                          "known Bell202 packet survives production conversion with exact FCS");
                });
            PcmProducer producer;
            producer.start(PcmPurpose::Slice, 3,
                {rate, channels == 1 ? PcmLayout::Mono : PcmLayout::Stereo});
            feed(fixture, producer, test::ax25rx::afsk(rate, channels), channels);
            check(waitFor([&] { return frames == 1; }) && pcm > 0,
                  "24/48 mono/stereo decode with awkward chunk boundaries and monitoring off");
            const int before = resets;
            const Ax25ReceiveContext live = retained;
            model.configure(ax25DemodConfigForProfile(Ax25ModemProfile::Vhf1200));
            model.setDiagnosticsLoggingEnabled(true);
            model.setDiagnosticsLoggingEnabled(false);
            fixture.a->setAudioGain(80);
            fixture.a->setAudioMute(false);
            fixture.radio.setPcAudioEnabled(true);
            check(resets == before && live.current(),
                  "unchanged config, logging and monitor controls preserve receive context");
            model.reset();
            check(!live.current(), "explicit reset immediately retires already delivered contexts");
            settle();
            check(frames == 1, "one fixture yields one accepted packet");
        }
    }
}
void routingAndDax(QCoreApplication& app)
{
    Fixture fixture;
    Ax25ReceiveModel model(fixture.radio);
    bind(model, fixture);
    int pcm = 0;
    QObject::connect(&model, &Ax25ReceiveModel::pcmReady, &app,
        [&](const DecoderPcmBlock&, const Ax25ReceiveContext&) { ++pcm; });
    PcmProducer other;
    PcmProducer speaker;
    other.start(PcmPurpose::Slice, 7, {24000, PcmLayout::Mono});
    speaker.start();
    emit fixture.source->sliceAudioFrameReady(7, *other.produce({0.5f}));
    emit fixture.source->audioFrameReady(*speaker.produce({0.5f, 0.5f}));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    check(pcm == 0, "native selection rejects unrelated slice and speaker mix");

    Fixture flex(true);
    Ax25ReceiveModel ax25(flex.radio);
    DecoderAudioModel cw(flex.radio, DecoderAudioModel::Consumer::Cw);
    DecoderAudioModel rtty(flex.radio, DecoderAudioModel::Consumer::Rtty);
    bind(ax25, flex);
    check(ax25.routeStatus() == DecoderAudioModel::RouteStatus::SharedRxAudio
          && flex.a->daxChannel() == 0, "Flex retains explicit shared fallback without auto assignment");
    int shared = 0;
    QObject::connect(&ax25, &Ax25ReceiveModel::pcmReady, &app,
        [&](const DecoderPcmBlock&, const Ax25ReceiveContext&) { ++shared; });
    PcmProducer sharedProducer;
    sharedProducer.start();
    emit flex.source->audioFrameReady(*sharedProducer.produce({0.5f, 0.5f}));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    check(shared == 1, "unassigned Flex fallback remains usable");
    int decoded = 0;
    QObject::connect(&ax25, &Ax25ReceiveModel::frameDecoded, &app,
        [&](const Ax25DecodedFrame& frame, const Ax25ReceiveContext& context) {
            if (context.current() && frame.ax25FrameNoFcs == test::ax25rx::frame() && frame.fcsOk) {
                ++decoded;
            }
        });
    feed(flex, sharedProducer, test::ax25rx::afsk(24000, 2), 2,
         [&](const PcmFrame& frame) { emit flex.source->audioFrameReady(frame); });
    check(waitFor([&] { return decoded == 1; }), "shared Flex fallback decodes exact packet and FCS");
    flex.a->setDaxChannel(8);
    cw.setSlice(flex.a);
    rtty.setSlice(flex.a);
    cw.setEnabled(true);
    rtty.setEnabled(true);
    using Holder = PanadapterStream::DaxConsumer;
    PanadapterStream& stream = *flex.source->stream;
    stream.acquireDaxChannel(8, Holder::Clock);
    check(stream.daxChannelHeldBy(8, Holder::Ax25Decoder)
          && stream.daxChannelHeldBy(8, Holder::CwDecoder)
          && stream.daxChannelHeldBy(8, Holder::RttyDecoder)
          && stream.daxChannelHeldBy(8, Holder::Clock)
          && stream.daxChannelSnapshot().front().holders.contains(QStringLiteral("ax25-decoder")),
          "AX25 has an independent DAX hold alongside CW RTTY and Clock");
    PcmProducer dax;
    dax.start(PcmPurpose::Auxiliary, -1, {24000, PcmLayout::Mono});
    feed(flex, dax, test::ax25rx::afsk(24000), 1,
         [&](const PcmFrame& frame) { emit stream.daxPcmReady(8, frame); });
    check(waitFor([&] { return decoded == 2; }), "assigned DAX path decodes exact packet");
    ax25.setEnabled(false);
    check(!stream.daxChannelHeldBy(8, Holder::Ax25Decoder)
          && stream.daxChannelHeldBy(8, Holder::CwDecoder)
          && stream.daxChannelHeldBy(8, Holder::RttyDecoder)
          && stream.daxChannelHeldBy(8, Holder::Clock),
          "closing AX25 releases only its hold");
    Fixture absent;
    absent.source->dax = true;
    absent.a->setDaxChannel(1);
    Ax25ReceiveModel unavailable(absent.radio);
    bind(unavailable, absent);
    check(unavailable.routeStatus() == DecoderAudioModel::RouteStatus::DaxTransportUnavailable,
          "missing assigned DAX transport reports unavailable instead of changing sources");
}

// The model has only one queued owner call: worker publication. Intercepting
// it retires the lease after worker decode, immediately before owner delivery.
class RetireBeforePublication final : public QObject {
public:
    std::function<void()> retire;
    bool fired = false;
    bool eventFilter(QObject*, QEvent* event) override
    {
        if (!fired && event->type() == QEvent::MetaCall) {
            fired = true;
            retire();
        }
        return false;
    }
};
void stalePublication(QCoreApplication& app, int scenario)
{
    Fixture fixture;
    Ax25ReceiveModel model(fixture.radio);
    bind(model, fixture);
    int frames = 0;
    int diagnostics = 0;
    QObject::connect(&model, &Ax25ReceiveModel::frameDecoded, &app,
        [&](const Ax25DecodedFrame&, const Ax25ReceiveContext&) { ++frames; });
    QObject::connect(&model, &Ax25ReceiveModel::diagnosticsUpdated, &app,
        [&](const Ax25DecoderDiagnostics&, const Ax25ReceiveContext&) { ++diagnostics; });
    PcmProducer producer;
    producer.start(PcmPurpose::Slice, 3, {24000, PcmLayout::Mono});
    RetireBeforePublication filter;
    filter.retire = [&] {
        switch (scenario) {
        case 0: model.setEnabled(false); break;
        case 1: model.setSlice(fixture.b); break;
        case 2: model.configure(ax25DemodConfigForProfile(Ax25ModemProfile::Hf300)); break;
        case 3: producer.invalidate(); break;
        case 4:
            fixture.source->disconnectRadio();
            // setBackendForTest intentionally bypasses production lifecycle wiring.
            emit fixture.radio.connectionStateChanged(false);
            break;
        default: model.reset(); break;
        }
    };
    model.installEventFilter(&filter);
    // Less than one second: the first worker result is the known packet, not
    // the modem's one-second diagnostic report. One block avoids input overflow.
    const QVector<float> packet = test::ax25rx::afsk(24000, 1, 40);
    check(packet.size() < 24000, "publication fixture ends before diagnostics interval");
    emit fixture.source->sliceAudioFrameReady(3, *producer.produce(packet));
    check(waitFor([&] { return filter.fired; }), "worker decoded before owner retirement hook");
    settle();
    check(frames == 0 && diagnostics == 0,
          "retired decoded output cannot escape after disable selection config epoch or disconnect");
    model.removeEventFilter(&filter);
}
void reentrantConsumers(QCoreApplication& app)
{
    Fixture fixture;
    auto model = std::make_unique<Ax25ReceiveModel>(fixture.radio);
    bind(*model, fixture);
    int first = 0;
    int admittedSecond = 0;
    Ax25ReceiveContext retained;
    QObject::connect(model.get(), &Ax25ReceiveModel::frameDecoded, &app,
        [&](const Ax25DecodedFrame&, const Ax25ReceiveContext& context) {
            ++first;
            retained = context;
            model->reset();
        });
    QObject::connect(model.get(), &Ax25ReceiveModel::frameDecoded, &app,
        [&](const Ax25DecodedFrame&, const Ax25ReceiveContext& context) {
            if (context.current()) {
                ++admittedSecond;
            }
        });
    PcmProducer producer;
    producer.start(PcmPurpose::Slice, 3, {24000, PcmLayout::Mono});
    feed(fixture, producer, test::ax25rx::afsk(24000), 1);
    check(waitFor([&] { return first == 1; }) && admittedSecond == 0 && !retained.current(),
          "later synchronous listeners reject context retired by the first listener");
    model.reset();
    check(!retained.current(), "retained context is safe and stale after model destruction");

    auto doomed = std::make_unique<Ax25ReceiveModel>(fixture.radio);
    bind(*doomed, fixture);
    QPointer<Ax25ReceiveModel> guard(doomed.get());
    QObject::connect(doomed.get(), &Ax25ReceiveModel::frameDecoded, &app,
        [&](const Ax25DecodedFrame&, const Ax25ReceiveContext& context) {
            retained = context;
            doomed.reset();
        });
    PcmProducer fresh;
    fresh.start(PcmPurpose::Slice, 3, {24000, PcmLayout::Mono});
    feed(fixture, fresh, test::ax25rx::afsk(24000), 1);
    check(waitFor([&] { return !guard; }) && !retained.current(),
          "frame callback may destroy model and join its worker without stale continuation");
}
void multipleResultsOneBlock(QCoreApplication& app, bool resetFirst)
{
    Fixture fixture;
    Ax25ReceiveModel model(fixture.radio);
    bind(model, fixture);
    QByteArray second = test::ax25rx::frame();
    second.back() = '2';
    QVector<float> samples = test::ax25rx::afsk(24000, 1, 40);
    samples += test::ax25rx::afsk(24000, 1, 40, second);
    int frames = 0;
    QObject::connect(&model, &Ax25ReceiveModel::frameDecoded, &app,
        [&](const Ax25DecodedFrame& frame, const Ax25ReceiveContext& context) {
            ++frames;
            check(context.current() && frame.fcsOk
                  && frame.ax25FrameNoFcs == (frames == 1 ? test::ax25rx::frame() : second),
                  "single PCM block yields independently framed exact AX25 payloads");
            if (resetFirst) {
                model.reset();
            }
        });
    PcmProducer producer;
    producer.start(PcmPurpose::Slice, 3, {24000, PcmLayout::Mono});
    check(samples.size() <= PcmFrame::kMaxFrames, "two packet fixture fits one bounded PCM block");
    emit fixture.source->sliceAudioFrameReady(3, *producer.produce(samples));
    check(waitFor([&] { return frames >= (resetFirst ? 1 : 2); }),
          "two-frame control decodes both, or first callback resets model");
    settle();
    check(frames == (resetFirst ? 1 : 2),
          "reset from first result prevents second result in the same worker output batch");
}
class WorkerBarrier {
public:
    struct State {
        QSemaphore entered;
        QSemaphore release;
        std::atomic<bool> timedOut{false};
    };
    explicit WorkerBarrier(Ax25ReceiveModel& model) : state(std::make_shared<State>())
    {
        QMetaObject::invokeMethod(Ax25ReceiveModelTestAccess::worker(model), [box = state] {
            box->entered.release();
            box->timedOut = !box->release.tryAcquire(1, 4000);
        }, Qt::QueuedConnection);
        check(state->entered.tryAcquire(1, 4000), "bounded worker barrier entered");
    }
    ~WorkerBarrier() { state->release.release(); }
    std::shared_ptr<State> state;
};
bool workerIdle(Ax25ReceiveModel& model)
{
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    return waitFor([&] {
        const auto queue = Ax25ReceiveModelTestAccess::queues(model);
        return !queue.workerScheduled && queue.inputBlocks == 0;
    });
}
void boundedInput(QCoreApplication& app, bool frameLimit)
{
    Fixture fixture;
    Ax25ReceiveModel model(fixture.radio);
    bind(model, fixture);
    PcmProducer producer;
    producer.start(PcmPurpose::Slice, 3, {24000, PcmLayout::Mono});
    // Establish the route's initial discontinuity before counting overflow.
    emit fixture.source->sliceAudioFrameReady(3, *producer.produce({0.0f}));
    check(workerIdle(model), "initial route input drains");
    int resets = 0;
    int decoded = 0;
    QObject::connect(&model, &Ax25ReceiveModel::sourceReset, &app, [&] { ++resets; });
    QObject::connect(&model, &Ax25ReceiveModel::frameDecoded, &app,
        [&](const Ax25DecodedFrame& frame, const Ax25ReceiveContext& context) {
            if (context.current() && frame.fcsOk && frame.ax25FrameNoFcs == test::ax25rx::frame()) {
                ++decoded;
            }
        });
    std::shared_ptr<WorkerBarrier::State> barrier;
    {
        WorkerBarrier blocked(model);
        barrier = blocked.state;
        const QVector<float> input(frameLimit ? 65536 : 1, 0.0f);
        const int blocks = frameLimit ? 2 : 257;
        for (int index = 0; index < blocks; ++index) {
            emit fixture.source->sliceAudioFrameReady(3, *producer.produce(input));
            QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
            const auto queue = Ax25ReceiveModelTestAccess::queues(model);
            check(queue.inputFrames <= 65536 && queue.inputBlocks <= 256
                  && queue.workerScheduled,
                  "stalled worker retains bounded PCM and one scheduled drain");
        }
        const auto queue = Ax25ReceiveModelTestAccess::queues(model);
        check(resets == 1 && queue.inputBlocks == 1
              && queue.inputFrames == input.size(),
              "input overflow retires backlog once and retains only the latest block");
    }
    check(workerIdle(model) && !barrier->timedOut, "worker barrier releases and retained input drains");
    feed(fixture, producer, test::ax25rx::afsk(24000), 1);
    check(waitFor([&] { return decoded == 1; }), "exact packet recovers after bounded input overflow");
}
void boundedOutput(QCoreApplication& app)
{
    Fixture fixture;
    Ax25ReceiveModel model(fixture.radio);
    bind(model, fixture);
    PcmProducer producer;
    producer.start(PcmPurpose::Slice, 3, {24000, PcmLayout::Mono});
    emit fixture.source->sliceAudioFrameReady(3, *producer.produce({0.0f}));
    check(workerIdle(model), "initial output route input drains");
    int resets = 0;
    int decoded = 0;
    int diagnostics = 0;
    QObject::connect(&model, &Ax25ReceiveModel::sourceReset, &app, [&] { ++resets; });
    QObject::connect(&model, &Ax25ReceiveModel::diagnosticsUpdated, &app,
        [&](const Ax25DecoderDiagnostics&, const Ax25ReceiveContext& context) {
            check(context.current(), "only current diagnostics escape output overflow");
            ++diagnostics;
        });
    QObject::connect(&model, &Ax25ReceiveModel::frameDecoded, &app,
        [&](const Ax25DecodedFrame& frame, const Ax25ReceiveContext& context) {
            if (context.current() && frame.fcsOk && frame.ax25FrameNoFcs == test::ax25rx::frame()) {
                ++decoded;
            }
        });
    RetireBeforePublication hold;
    hold.retire = [&] {
        // Keep the original owner event on the stack, then allow it to run.
        // This stalls publication without deleting or replacing a queued call.
        const QVector<float> silence(24000, 0.0f);
        for (int index = 0; index < 259; ++index) {
            emit fixture.source->sliceAudioFrameReady(3, *producer.produce(silence));
            QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
            check(workerIdle(model), "bounded output fixture drains one diagnostic input");
            const auto queue = Ax25ReceiveModelTestAccess::queues(model);
            check(queue.outputBlocks <= 256 && queue.outputScheduled,
                  "stalled owner retains bounded results and its single pending publication");
        }
        check(diagnostics == 0 && resets == 0,
              "owner stall delays outward diagnostics and reset notification");
    };
    model.installEventFilter(&hold);
    emit fixture.source->sliceAudioFrameReady(3, *producer.produce(QVector<float>(24000, 0.0f)));
    check(waitFor([&] { return hold.fired; }), "owner publication stall executed");
    model.removeEventFilter(&hold);
    check(resets == 1 && diagnostics > 0 && diagnostics < 10,
          "output overflow resets once and publishes only post-retirement diagnostics");
    feed(fixture, producer, test::ax25rx::afsk(24000), 1);
    check(waitFor([&] { return decoded == 1; }), "exact packet recovers after bounded output overflow");
}
void inputRetirement(QCoreApplication& app)
{
    Fixture fixture;
    Ax25ReceiveModel model(fixture.radio);
    bind(model, fixture);
    int frames = 0;
    QObject::connect(&model, &Ax25ReceiveModel::frameDecoded, &app,
        [&](const Ax25DecodedFrame&, const Ax25ReceiveContext&) { ++frames; });
    PcmProducer producer;
    producer.start(PcmPurpose::Slice, 3, {24000, PcmLayout::Mono});
    emit fixture.source->sliceAudioFrameReady(3, *producer.produce(test::ax25rx::afsk(24000)));
    model.setSlice(fixture.b);
    model.setSlice(fixture.a);
    settle();
    check(frames == 0, "queued selected input is retired even after reselecting the same slice ID");
    feed(fixture, producer, test::ax25rx::afsk(24000), 1);
    check(waitFor([&] { return frames == 1; }), "fresh audio recovers after selection retirement");
}
} // namespace
int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("ax25-receive-model"));
    if (!profile.isValid()) {
        return 2;
    }
    QCoreApplication app(argc, argv);
    packetMatrix(app);
    routingAndDax(app);
    for (int scenario = 0; scenario < 6; ++scenario) {
        stalePublication(app, scenario);
    }
    reentrantConsumers(app);
    multipleResultsOneBlock(app, false);
    multipleResultsOneBlock(app, true);
    boundedInput(app, false);
    boundedInput(app, true);
    boundedOutput(app);
    inputRetirement(app);
    std::fprintf(stdout, "%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
