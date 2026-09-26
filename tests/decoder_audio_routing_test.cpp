// Socket-free production binding. The injected backend publishes normalized
// lifecycle/PCM signals; it has no socket, hardware, DSP worker or TX transport.
#include "TestSettingsProfile.h"
#include "core/backends/IRadioBackend.h"
#include "models/DecoderAudioModel.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QCoreApplication>
#include <QEvent>
#include <QPointer>

#include <cstdio>
#include <barrier>
#include <memory>
#include <thread>

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

SliceModel* addSlice(RadioModel& radio, Source& source, int id)
{
    SliceDelta delta;
    delta.panId = QStringLiteral("decoder-pan");
    delta.frequency = 14.1;
    delta.mode = QStringLiteral("RTTY");
    delta.inUse = true;
    emit source.sliceChanged(id, delta);
    return radio.slice(id);
}

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
        a = addSlice(radio, *source, 3);
        b = addSlice(radio, *source, 7);
    }
};

class DrainCounter final : public QObject {
public:
    int calls = 0;
    bool eventFilter(QObject*, QEvent* event) override
    {
        if (event->type() == QEvent::MetaCall) {
            ++calls;
        }
        return false;
    }
};

void nestedRebind(QCoreApplication& app)
{
    Fixture fixture;
    DecoderAudioModel route(fixture.radio, DecoderAudioModel::Consumer::Rtty);
    route.setSlice(fixture.a);
    bool switchOnce = true;
    QObject::connect(&route, &DecoderAudioModel::sourceReset, &app, [&] {
        if (switchOnce) {
            switchOnce = false;
            route.setSlice(fixture.b);
        }
    });
    route.setEnabled(true);
    DrainCounter counter;
    route.installEventFilter(&counter);
    int delivered = 0;
    QObject::connect(&route, &DecoderAudioModel::pcmReady, &app,
                     [&](const DecoderPcmBlock&) { ++delivered; });
    PcmProducer source;
    source.start(PcmPurpose::Slice, 7);
    emit fixture.source->sliceAudioFrameReady(7, *source.produce({0.5f, 0.5f}));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    check(delivered == 1 && counter.calls == 1,
          "nested source-reset selection installs exactly one bounded drain subscription");
}

void deletionDuringNotifications(QCoreApplication& app, const QString& scenario)
{
    Fixture fixture;
    auto route = std::make_unique<DecoderAudioModel>(fixture.radio,
                                                   DecoderAudioModel::Consumer::Rtty);
    route->setSlice(fixture.a);
    route->setEnabled(true);
    QPointer<DecoderAudioModel> guard(route.get());
    if (scenario == QStringLiteral("delete-on-pcm")) {
        QObject::connect(route.get(), &DecoderAudioModel::pcmReady, &app,
                         [&](const DecoderPcmBlock&) { route.reset(); });
    } else {
        QObject::connect(route.get(), &DecoderAudioModel::sourceReset, &app,
                         [&] { route.reset(); });
    }
    if (scenario == QStringLiteral("delete-on-rebind")) {
        route->setSlice(fixture.b);
    } else {
        PcmProducer source;
        source.start(PcmPurpose::Slice, 3);
        emit fixture.source->sliceAudioFrameReady(3, *source.produce({0.5f, 0.5f}));
        emit fixture.source->sliceAudioFrameReady(3, *source.produce({0.25f, 0.25f}));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    }
    check(!guard, "a direct notification may destroy the route without a stale continuation");
}

void boundedInboxAndIndependentConsumers(QCoreApplication& app)
{
    Fixture fixture;
    DecoderAudioModel cw(fixture.radio, DecoderAudioModel::Consumer::Cw);
    DecoderAudioModel rtty(fixture.radio, DecoderAudioModel::Consumer::Rtty);
    cw.setSlice(fixture.a);
    rtty.setSlice(fixture.a);
    cw.setEnabled(true);
    rtty.setEnabled(true);
    DrainCounter counter;
    rtty.installEventFilter(&counter);
    int cwDelivered = 0;
    int rttyDelivered = 0;
    int resets = 0;
    DecoderPcmBlock received;
    QObject::connect(&cw, &DecoderAudioModel::pcmReady, &app,
                     [&](const DecoderPcmBlock&) { ++cwDelivered; });
    QObject::connect(&rtty, &DecoderAudioModel::pcmReady, &app,
                     [&](const DecoderPcmBlock& block) { ++rttyDelivered; received = block; });
    QObject::connect(&rtty, &DecoderAudioModel::sourceReset, &app, [&] { ++resets; });
    PcmProducer producer;
    producer.start(PcmPurpose::Slice, 3, {24000, PcmLayout::Mono});
    for (int index = 0; index < 257; ++index) {
        emit fixture.source->sliceAudioFrameReady(3, *producer.produce({index / 512.0f}));
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    check(cwDelivered == 1 && rttyDelivered == 1 && counter.calls == 1
          && received.samples == QVector<float>({0.5f}) && received.discontinuity
          && received.segmentFirstInputSample == 256 && resets > 0,
          "block overflow retains only latest PCM with one queued drain and an immediate reset");
    cw.setEnabled(false);
    emit fixture.source->sliceAudioFrameReady(3, *producer.produce(QVector<float>(65536, 0.25f)));
    emit fixture.source->sliceAudioFrameReady(3, *producer.produce(QVector<float>(65536, 0.75f)));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    check(cwDelivered == 1 && rttyDelivered == 2 && received.samples.size() == 65536
          && received.samples.front() == 0.75f && received.discontinuity,
          "frame overflow enforces sample memory bound and disabled CW leaves RTTY live");
    emit fixture.source->sliceAudioFrameReady(3, *producer.produce({0.1f}));
    producer.invalidate();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    check(rttyDelivered == 2, "producer revocation between enqueue and drain suppresses delivery");
    SliceModel foreign(3);
    rtty.setSlice(&foreign);
    PcmProducer foreignSource;
    foreignSource.start(PcmPurpose::Slice, 3);
    emit fixture.source->sliceAudioFrameReady(3, *foreignSource.produce({0.2f, 0.2f}));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    check(rttyDelivered == 2, "same-number model outside the owning radio cannot select a route");
}

void daxHoldsAndAbsentRoute(QCoreApplication& app)
{
    // Constructor-only PanadapterStream has no initialized socket. Exercise
    // the real holder registry without satisfying any stream-create request.
    PanadapterStream stream;
    int creates = 0;
    QObject::connect(&stream, &PanadapterStream::daxStreamCreateNeeded, &app,
                     [&](int) { ++creates; });
    using Consumer = PanadapterStream::DaxConsumer;
    stream.acquireDaxChannel(8, Consumer::Clock);
    stream.acquireDaxChannel(8, Consumer::CwDecoder);
    stream.acquireDaxChannel(8, Consumer::RttyDecoder);
    check(creates == 1 && stream.daxChannelHeldBy(8, Consumer::Clock)
          && stream.daxChannelHeldBy(8, Consumer::CwDecoder)
          && stream.daxChannelHeldBy(8, Consumer::RttyDecoder),
          "Clock CW and RTTY have independent bits while sharing one DAX channel stream");
    stream.releaseDaxChannel(8, Consumer::CwDecoder);
    check(!stream.daxChannelHeldBy(8, Consumer::CwDecoder)
          && stream.daxChannelHeldBy(8, Consumer::Clock)
          && stream.daxChannelHeldBy(8, Consumer::RttyDecoder),
          "CW release cannot withdraw Clock or RTTY DAX ownership");
    const auto snapshot = stream.daxChannelSnapshot();
    check(snapshot.size() == 1 && snapshot.front().holders.contains(QStringLiteral("clock"))
          && snapshot.front().holders.contains(QStringLiteral("rtty-decoder"))
          && !snapshot.front().holders.contains(QStringLiteral("cw-decoder")),
          "DAX diagnostic snapshot reports actual independent decoder holders");

    Fixture fixture;
    fixture.source->dax = true;
    fixture.a->setDaxChannel(1);
    DecoderAudioModel route(fixture.radio, DecoderAudioModel::Consumer::Rtty);
    route.setSlice(fixture.a);
    route.setEnabled(true);
    int delivered = 0;
    QObject::connect(&route, &DecoderAudioModel::pcmReady, &app,
                     [&](const DecoderPcmBlock&) { ++delivered; });
    PcmProducer native;
    native.start(PcmPurpose::Slice, 3);
    emit fixture.source->sliceAudioFrameReady(3, *native.produce({0.1f, 0.1f}));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    check(fixture.radio.panStream() == nullptr && delivered == 0,
          "missing DAX transport fails closed without falling back to native or speaker audio");
    PcmProducer unavailableSpeaker;
    unavailableSpeaker.start();
    emit fixture.source->audioFrameReady(*unavailableSpeaker.produce({0.1f, 0.1f}));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    check(delivered == 0,
          "an assigned channel that cannot be acquired does not silently take shared audio");
    check(route.routeStatus() == DecoderAudioModel::RouteStatus::DaxTransportUnavailable,
          "missing DAX transport has an explicit status distinct from channel assignment");
}

void daxAvailabilityStatus(QCoreApplication& app)
{
    using Status = DecoderAudioModel::RouteStatus;
    for (const DecoderAudioModel::Consumer consumer :
         {DecoderAudioModel::Consumer::Cw, DecoderAudioModel::Consumer::Rtty}) {
        Fixture fixture(true);
        PanadapterStream* stream = fixture.source->stream.get();
        const auto holder = consumer == DecoderAudioModel::Consumer::Cw
            ? PanadapterStream::DaxConsumer::CwDecoder
            : PanadapterStream::DaxConsumer::RttyDecoder;
        DecoderAudioModel route(fixture.radio, consumer);
        QVector<Status> statuses;
        QObject::connect(&route, &DecoderAudioModel::routeStatusChanged, &app,
                         [&] { statuses.append(route.routeStatus()); });
        int delivered = 0;
        QObject::connect(&route, &DecoderAudioModel::pcmReady, &app,
                         [&](const DecoderPcmBlock&) { ++delivered; });
        route.setSlice(fixture.a);
        check(fixture.a->daxChannel() == 0 && route.routeStatus() == Status::Inactive
              && statuses.isEmpty(), "disabled decoder does not claim a missing DAX route");
        route.setEnabled(true);
        check(route.routeStatus() == Status::SharedRxAudio
              && statuses == QVector<Status>{Status::SharedRxAudio},
              "an unassigned DAX channel discloses the shared-audio fallback, not a dead route");

        PcmProducer native;
        PcmProducer speaker;
        native.start(PcmPurpose::Slice, 3);
        speaker.start();
        emit fixture.source->sliceAudioFrameReady(3, *native.produce({0.2f, 0.2f}));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        check(delivered == 0,
              "the shared-audio lane refuses per-slice frames from the native lane");
        emit fixture.source->audioFrameReady(*speaker.produce({0.2f, 0.2f}));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        check(delivered == 1,
              "an unassigned DAX slice decodes the radio's shared receive audio");
        fixture.a->setFrequency(14.2);
        check(statuses.size() == 1, "repeated tuning does not repeat the status");

        fixture.a->setDaxChannel(1);
        check(route.routeStatus() == Status::Bound && stream->daxChannelHeldBy(1, holder)
              && statuses.size() == 2,
              "assignment upgrades the shared route to the isolated DAX route");
        PcmProducer dax;
        dax.start(PcmPurpose::Auxiliary, -1, {24000, PcmLayout::Mono});
        emit stream->daxPcmReady(1, *dax.produce({0.5f}));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        check(delivered == 2, "recovered DAX route delivers real adapter PCM");
        emit fixture.source->audioFrameReady(*speaker.produce({0.2f, 0.2f}));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        check(delivered == 2,
              "an assigned DAX route stays isolated from the shared receive audio");

        emit stream->daxPcmReady(1, *dax.produce({0.25f}));
        fixture.a->setDaxChannel(0);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        check(delivered == 2 && route.routeStatus() == Status::SharedRxAudio
              && !stream->daxChannelHeldBy(1, holder) && statuses.size() == 3,
              "unassignment retires queued PCM, releases the hold and returns to shared audio");
        route.setEnabled(false);
        check(route.routeStatus() == Status::Inactive && statuses.size() == 4,
              "disabling the decoder clears the route warning state");
    }
}

void statusNotificationReentrancy(QCoreApplication& app)
{
    using Status = DecoderAudioModel::RouteStatus;
    using Holder = PanadapterStream::DaxConsumer;
    Fixture fixture(true);
    auto route = std::make_unique<DecoderAudioModel>(fixture.radio,
                                                   DecoderAudioModel::Consumer::Cw);
    route->setSlice(fixture.a);
    QMetaObject::Connection repair = QObject::connect(
        route.get(), &DecoderAudioModel::routeStatusChanged, &app, [&] {
            if (route->routeStatus() == Status::SharedRxAudio) {
                fixture.a->setDaxChannel(1);
            }
        });
    route->setEnabled(true);
    check(route->routeStatus() == Status::Bound
          && fixture.source->stream->daxChannelHeldBy(1, Holder::CwDecoder),
          "a status listener can repair the route without an outer stale continuation");
    QObject::disconnect(repair);
    QMetaObject::Connection disable = QObject::connect(
        route.get(), &DecoderAudioModel::routeStatusChanged, &app, [&] {
            if (route->routeStatus() == Status::SharedRxAudio) {
                route->setEnabled(false);
            }
        });
    fixture.a->setDaxChannel(0);
    check(route->routeStatus() == Status::Inactive
          && !fixture.source->stream->daxChannelHeldBy(1, Holder::CwDecoder),
          "a status listener can disable input without an outer stale warning state");
    QObject::disconnect(disable);
    fixture.a->setDaxChannel(1);
    QPointer<DecoderAudioModel> guard(route.get());
    QObject::connect(route.get(), &DecoderAudioModel::routeStatusChanged, &app,
                     [&] { route.reset(); });
    route->setEnabled(true);
    check(!guard && !fixture.source->stream->daxChannelHeldBy(1, Holder::CwDecoder),
          "a status listener can destroy the route and release its acquired hold");
}

void concurrentProducerRetirement(QCoreApplication& app)
{
    Fixture fixture;
    auto route = std::make_unique<DecoderAudioModel>(fixture.radio,
                                                   DecoderAudioModel::Consumer::Rtty);
    route->setSlice(fixture.a);
    route->setEnabled(true);
    int delivered = 0;
    QObject::connect(route.get(), &DecoderAudioModel::pcmReady, &app,
                     [&](const DecoderPcmBlock&) { ++delivered; });
    QPointer<DecoderAudioModel> guard(route.get());
    std::barrier started(2);
    // Direct injection exercises the inbox's producer-thread callback, as DAX
    // does in production. The radio remains alive until the producer joins.
    std::thread producer([&] {
        PcmProducer source;
        source.start(PcmPurpose::Slice, 3, {24000, PcmLayout::Mono});
        for (int index = 0; index < 4096; ++index) {
            emit fixture.radio.backendSliceAudioFrameReady(3, *source.produce({0.25f}));
            if (index == 0) {
                started.arrive_and_wait();
            }
        }
    });
    started.arrive_and_wait();
    for (int index = 0; index < 64; ++index) {
        route->setEnabled(false);
        route->setEnabled(true);
    }
    route.reset();
    producer.join();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    check(!guard && delivered == 0,
          "producer callbacks cannot deliver after owner-thread rebind and destruction");
}

void daxAcquisitionLifecycle(QCoreApplication& app)
{
    Fixture fixture(true);
    PanadapterStream* stream = fixture.source->stream.get();
    using Consumer = PanadapterStream::DaxConsumer;
    fixture.a->setDaxChannel(1);
    fixture.b->setDaxChannel(2);
    DecoderAudioModel cw(fixture.radio, DecoderAudioModel::Consumer::Cw);
    cw.setSlice(fixture.a);
    cw.setEnabled(true);
    auto route = std::make_unique<DecoderAudioModel>(fixture.radio,
                                                   DecoderAudioModel::Consumer::Rtty);
    route->setSlice(fixture.a);
    route->setEnabled(true);
    bool oldHeldDuringNewAcquire = false;
    bool disableOnAcquire = false;
    bool deleteOnAcquire = false;
    QObject::connect(stream, &PanadapterStream::daxStreamCreateNeeded, &app, [&](int channel) {
        if (channel == 2) {
            oldHeldDuringNewAcquire = stream->daxChannelHeldBy(1, Consumer::RttyDecoder);
        } else if (channel == 3 && disableOnAcquire) {
            route->setEnabled(false);
        } else if (channel == 4 && deleteOnAcquire) {
            route.reset();
        }
    });
    route->setSlice(fixture.b);
    check(oldHeldDuringNewAcquire && stream->daxChannelHeldBy(2, Consumer::RttyDecoder)
          && !stream->daxChannelHeldBy(1, Consumer::RttyDecoder)
          && stream->daxChannelHeldBy(1, Consumer::CwDecoder),
          "actual route acquires new DAX channel before releasing old without disturbing CW");
    int delivered = 0;
    DecoderPcmBlock received;
    QObject::connect(route.get(), &DecoderAudioModel::pcmReady, &app,
                     [&](const DecoderPcmBlock& block) { ++delivered; received = block; });
    fixture.b->setAudioGain(0);
    fixture.b->setAudioMute(true);
    PcmProducer selected;
    PcmProducer other;
    selected.start(PcmPurpose::Auxiliary);
    other.start(PcmPurpose::Auxiliary);
    emit stream->daxPcmReady(1, *other.produce({0.9f, 0.9f}));
    emit stream->daxPcmReady(2, *selected.produce({0.75f, 0.25f}));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    check(delivered == 1 && received.samples == QVector<float>({0.5f}),
          "selected DAX channel reaches real adapter independently of speaker gain and mute");
    emit stream->daxPcmReady(2, *selected.produce({0.2f, 0.2f}));
    disableOnAcquire = true;
    fixture.b->setDaxChannel(3);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    check(delivered == 1 && !stream->daxChannelHeldBy(2, Consumer::RttyDecoder)
          && !stream->daxChannelHeldBy(3, Consumer::RttyDecoder)
          && stream->daxChannelHeldBy(1, Consumer::CwDecoder),
          "disable during new-channel acquire releases both tracked holds and retires queued PCM");
    route->setEnabled(true);
    deleteOnAcquire = true;
    fixture.b->setDaxChannel(4);
    check(!route && !stream->daxChannelHeldBy(3, Consumer::RttyDecoder)
          && !stream->daxChannelHeldBy(4, Consumer::RttyDecoder)
          && stream->daxChannelHeldBy(1, Consumer::CwDecoder),
          "destruction during acquisition releases both channels without touching another consumer");
}
}

int main(int argc, char** argv)
{
    TestSettingsProfile settings("decoder_audio_routing_test");
    QCoreApplication app(argc, argv);
    if (app.arguments().size() == 2) {
        const QString scenario = app.arguments().at(1);
        if (scenario == QStringLiteral("nested-rebind")) {
            nestedRebind(app);
        } else if (scenario == QStringLiteral("dax-lifecycle")) {
            daxAcquisitionLifecycle(app);
        } else if (scenario == QStringLiteral("dax-status")) {
            daxAvailabilityStatus(app);
            statusNotificationReentrancy(app);
        } else {
            deletionDuringNotifications(app, scenario);
        }
        std::printf("%d checks, %d failures\n", checks, failures);
        return failures ? 1 : 0;
    }
    RadioModel radio;
    auto backend = std::make_unique<Source>();
    Source* source = backend.get();
    radio.setBackendForTest(std::move(backend), QStringLiteral("rtl"));
    source->connectRadio({});
    SliceModel* a = addSlice(radio, *source, 3);
    SliceModel* b = addSlice(radio, *source, 7);
    check(a && b, "production normalized state creates sparse slices");
    if (!a || !b) {
        return 1;
    }

    DecoderAudioModel route(radio, DecoderAudioModel::Consumer::Rtty);
    int delivered = 0;
    int resets = 0;
    DecoderPcmBlock received;
    QObject::connect(&route, &DecoderAudioModel::pcmReady, &app,
                     [&](const DecoderPcmBlock& block) { ++delivered; received = block; });
    QObject::connect(&route, &DecoderAudioModel::sourceReset, &app, [&] { ++resets; });
    route.setSlice(a);
    route.setEnabled(true);
    PcmProducer pa;
    PcmProducer pb;
    PcmProducer speaker;
    pa.start(PcmPurpose::Slice, 3, {48000, PcmLayout::Mono});
    pb.start(PcmPurpose::Slice, 7);
    speaker.start();
    const PcmFrame a0 = *pa.produce({0.125f, 0.25f, 0.5f});
    emit source->sliceAudioFrameReady(3, a0);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    check(delivered == 1 && received.source.stream().sliceId == 3
          && received.inputSampleRateHz == 48000 && received.inputEndSample == 3,
          "intended sparse receiver PCM retains source rate and frame positions");
    emit source->sliceAudioFrameReady(7, *pb.produce({0.8f, 0.8f}));
    emit source->audioFrameReady(*speaker.produce({0.0f, 0.0f}));
    emit source->sliceAudioFrameReady(7, *pa.produce({0.3f}));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    check(delivered == 1, "other receiver, speaker mix and mismatched envelope are refused");
    a->setAudioGain(0);
    a->setAudioMute(true);
    emit source->sliceAudioFrameReady(3, *pa.produce({0.2f}));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    check(delivered == 2 && received.inputEndSample == 5,
          "slice speaker mute/gain cannot alter decoder tap");

    const int beforeSwitch = resets;
    emit source->sliceAudioFrameReady(3, *pa.produce({0.4f}));
    route.setSlice(b);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    check(delivered == 2 && resets > beforeSwitch,
          "selection retires queued old receiver delivery and decoder state");
    emit source->sliceAudioFrameReady(7, *pb.produce({0.6f, 0.2f}));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    check(delivered == 3 && received.source.stream().sliceId == 7,
          "new selected receiver delivers independently");
    const int beforeTune = resets;
    b->setFrequency(14.11);
    check(resets > beforeTune, "retuning selected receiver resets detector context");
    emit source->sliceAudioFrameReady(7, *pb.produce({0.1f, 0.1f}));
    route.setEnabled(false);
    route.setEnabled(true);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    check(delivered == 3, "toggle cycle cannot revive queued old input");

    const int beforeRemoval = resets;
    emit source->sliceRemoved(7);
    emit source->sliceAudioFrameReady(7, *pb.produce({0.1f, 0.1f}));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    check(delivered == 3 && resets > beforeRemoval,
          "removal withdraws old selected model before deferred destruction");
    b = addSlice(radio, *source, 7);
    route.setSlice(b);
    PcmProducer replacement;
    replacement.start(PcmPurpose::Slice, 7);
    emit source->sliceAudioFrameReady(7, *replacement.produce({0.7f, 0.7f}));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    check(delivered == 4, "replacement model can receive the same sparse slot");
    emit source->sliceAudioFrameReady(7, *replacement.produce({0.9f, 0.9f}));
    source->disconnectRadio();
    emit radio.connectionStateChanged(false);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    check(delivered == 4, "disconnect withdraws pending receiver delivery");

    nestedRebind(app);
    deletionDuringNotifications(app, QStringLiteral("delete-on-rebind"));
    deletionDuringNotifications(app, QStringLiteral("delete-on-drain-reset"));
    deletionDuringNotifications(app, QStringLiteral("delete-on-pcm"));
    boundedInboxAndIndependentConsumers(app);
    daxHoldsAndAbsentRoute(app);
    daxAvailabilityStatus(app);
    statusNotificationReentrancy(app);
    concurrentProducerRetirement(app);
    daxAcquisitionLifecycle(app);

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
