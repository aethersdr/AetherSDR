// Data2gReceiveModel (RFC #7159, Stage 1): the threaded receive-only monitor
// AetherModem drives. Injected 24 kHz PCM and the real worker thread; no radio,
// no sockets, no sound device, and nothing that can transmit.

// The core's headers first: they have members named `slots`, which Qt's
// keyword macros would rewrite once any Qt header has been seen.
#include "data2g_test_signal.h"

#include "models/Data2gReceiveModel.h"

#include <QCoreApplication>
#include <QThread>
#include <QDeadlineTimer>
#include <QEventLoop>
#include <QVector>

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>

using AetherSDR::Data2gHeardBurst;
using AetherSDR::Data2gReceiveModel;

namespace {

int g_failures = 0;

void check(bool ok, const char* what)
{
    if (ok)
        return;
    ++g_failures;
    std::fprintf(stderr, "FAIL: %s\n", what);
}

bool waitFor(const std::function<bool()>& done, int ms = 20000)
{
    QDeadlineTimer deadline(ms);
    while (!done() && !deadline.hasExpired())
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
}

void settle(int ms = 300)
{
    QDeadlineTimer deadline(ms);
    while (!deadline.hasExpired())
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

// The tap's shape: small blocks, as the 24 kHz decoder feed delivers them.
void feedBlocks(Data2gReceiveModel& model, const std::vector<float>& pcm,
                std::size_t from = 0, std::size_t to = SIZE_MAX)
{
    static constexpr std::size_t kBlock = 256;
    to = std::min(to, pcm.size());
    for (std::size_t offset = from; offset < to; offset += kBlock) {
        const std::size_t n = std::min(kBlock, to - offset);
        model.feed(QVector<float>(pcm.begin() + static_cast<std::ptrdiff_t>(offset),
                                  pcm.begin() + static_cast<std::ptrdiff_t>(offset + n)));
    }
}

bool isBeacon(const Data2gHeardBurst& burst)
{
    return burst.head.contains(QStringLiteral("BCAST \"KISS 0\": 1 frame(s)"))
        && burst.text.contains(QStringLiteral("W1AW>APRS"))
        && burst.text.contains(QStringLiteral("!beacon"));
}

void identity()
{
    check(Data2gReceiveModel::available(), "the model is available in this build");
    check(Data2gReceiveModel::upstreamVersion() == QStringLiteral("0.4.3"),
          "the model reports the vendored release");
}

void decodes(const std::vector<float>& pcm)
{
    Data2gReceiveModel model;
    QVector<Data2gHeardBurst> heard;
    int statsSignals = 0;
    QObject::connect(&model, &Data2gReceiveModel::burstHeard,
                     [&](const Data2gHeardBurst& burst) { heard.append(burst); });
    QObject::connect(&model, &Data2gReceiveModel::statsChanged, [&] { ++statsSignals; });

    feedBlocks(model, pcm);
    settle();
    check(heard.isEmpty(), "a disabled model decodes nothing");
    check(!model.isEnabled(), "the model starts disabled");

    model.setEnabled(true);
    check(model.isEnabled(), "the model enables");
    check(!model.audioFlowing(), "enabled with no audio yet is not flowing");
    feedBlocks(model, pcm);
    check(model.audioFlowing(), "audio is flowing once blocks arrive");
    check(waitFor([&] { return !heard.isEmpty(); }), "an enabled model reports the burst");
    settle();

    int beacons = 0;
    for (const Data2gHeardBurst& burst : heard) {
        if (isBeacon(burst))
            ++beacons;
    }
    check(beacons == 1, "the broadcast frame is reported exactly once");
    check(!heard.isEmpty() && heard.first().heardUtc.isValid()
              && heard.first().heardUtc.timeSpec() == Qt::UTC,
          "the burst carries a UTC time");
    check(!heard.isEmpty() && heard.first().head.contains(QLatin1Char('Z')),
          "the burst line opens with a UTC stamp");
    check(model.stats().bursts == static_cast<quint64>(heard.size()),
          "the burst count matches what was reported");
    check(statsSignals > 0, "statsChanged fired");
    model.setEnabled(false);
    check(!model.audioFlowing(), "a disabled model reports no audio");
    check(model.stats().faults == 0 && model.stats().overflows == 0,
          "a clean stream counts no fault or overflow");
}

// The stamp is when the burst began on the air, not when its decode finished:
// the whole signal arrives as one block, so the burst began before that block
// landed, by at least the burst's own length.
// Returns where the burst sits in the signal, in ms from its first sample.
qint64 timesBurstStart(const std::vector<float>& pcm)
{
    Data2gReceiveModel model;
    QVector<Data2gHeardBurst> heard;
    QObject::connect(&model, &Data2gReceiveModel::burstHeard,
                     [&](const Data2gHeardBurst& burst) { if (isBeacon(burst)) heard.append(burst); });
    model.setEnabled(true);
    const QDateTime before = QDateTime::currentDateTimeUtc();
    model.feed(QVector<float>(pcm.begin(), pcm.end()));
    const QDateTime fed = QDateTime::currentDateTimeUtc();
    check(waitFor([&] { return !heard.isEmpty(); }), "start time: the burst is reported");
    if (heard.isEmpty())
        return 0;
    const Data2gHeardBurst& burst = heard.first();
    const qint64 lengthMs = static_cast<qint64>(pcm.size()) * 1000 / Data2gReceiveModel::kSampleRateHz;
    check(burst.startedUtc.isValid() && burst.startedUtc.timeSpec() == Qt::UTC,
          "start time: the burst carries a UTC start");
    check(burst.startedUtc >= before.addMSecs(-lengthMs),
          "start time: not before the stream began");
    check(burst.startedUtc <= fed.addMSecs(-300),
          "start time: before the audio finished arriving, not when the decode ended");
    check(burst.startedUtc <= burst.heardUtc, "start time: the burst starts before it is decoded");
    check(burst.head.startsWith(burst.startedUtc.toString(QStringLiteral("HH:mm:ss'Z'"))),
          "start time: the burst line opens with its start");
    return fed.addMSecs(-lengthMs).msecsTo(burst.startedUtc);
}

// A gap in one stream is not a reset, and it does not shift the clock: a burst
// heard after a pause with no audio is stamped by when it arrived, not by how
// much audio came before the pause.
void survivesGap(const std::vector<float>& pcm, qint64 burstOffsetMs)
{
    AetherSDR::PcmStreamDescriptor slice;
    slice.source = 1;
    slice.sliceId = 0;
    const qint64 lengthMs = static_cast<qint64>(pcm.size()) * 1000 / Data2gReceiveModel::kSampleRateHz;
    const QVector<float> signal(pcm.begin(), pcm.end());

    Data2gReceiveModel model;
    QVector<Data2gHeardBurst> heard;
    QObject::connect(&model, &Data2gReceiveModel::burstHeard,
                     [&](const Data2gHeardBurst& burst) { if (isBeacon(burst)) heard.append(burst); });
    model.setEnabled(true);
    model.feedFrom(slice, signal);
    check(waitFor([&] { return heard.size() == 1; }), "gap: the burst before it is reported");
    settle(2500);   // the gap: wall time passes, no audio does
    model.feedFrom(slice, signal);
    const QDateTime fed = QDateTime::currentDateTimeUtc();
    check(waitFor([&] { return heard.size() == 2; }), "gap: the burst after it is reported");
    settle();
    check(heard.size() == 2, "gap: each burst is reported exactly once");
    check(model.stats().faults == 0, "gap: no fault was counted");
    if (heard.size() < 2)
        return;
    const QDateTime expected = fed.addMSecs(-lengthMs + burstOffsetMs);
    check(std::llabs(expected.msecsTo(heard.at(1).startedUtc)) < 500,
          "gap: the burst after it is stamped by when it arrived");
}

// A stream change mid-burst, then the whole burst: reported exactly once.
void resets(const std::vector<float>& pcm)
{
    Data2gReceiveModel model;
    int beacons = 0;
    QObject::connect(&model, &Data2gReceiveModel::burstHeard,
                     [&](const Data2gHeardBurst& burst) { beacons += isBeacon(burst) ? 1 : 0; });
    model.setEnabled(true);

    std::size_t loudest = 0;
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        if (std::fabs(pcm[i]) > std::fabs(pcm[loudest]))
            loudest = i;
    }
    for (int round = 1; round <= 2; ++round) {
        feedBlocks(model, pcm, 0, loudest);
        settle();
        model.feed(QVector<float>(256, 0.0f), true);
        feedBlocks(model, pcm);
        check(waitFor([&] { return beacons == round; }),
              "after a stream change the whole burst is reported");
        settle();
        check(beacons == round, "after a stream change the burst is reported exactly once");
    }
}

// A burst that finished decoding is still reported when a stream change lands before
// it is published. The owner's event loop is held off while the worker
// decodes, so the result is queued when the reset arrives. How long the decode
// takes depends on the machine, so the wait doubles until it was long enough.
void keepsDecodedAcrossReset(const std::vector<float>& pcm)
{
    bool reported = false;
    for (int waitMs = 500; waitMs <= 16000 && !reported; waitMs *= 2) {
        Data2gReceiveModel model;
        int beacons = 0;
        QObject::connect(&model, &Data2gReceiveModel::burstHeard,
                         [&](const Data2gHeardBurst& burst) { beacons += isBeacon(burst) ? 1 : 0; });
        model.setEnabled(true);
        feedBlocks(model, pcm);
        QThread::msleep(static_cast<unsigned long>(waitMs));   // no events: nothing is published
        model.feed(QVector<float>(256, 0.0f), true);
        settle(500);
        reported = beacons == 1;
    }
    check(reported, "a burst decoded before a stream change is still reported");
}

// The monitor resets when the stream changes, and only then. Half a burst
// from one stream and the rest from the same stream is one burst; the rest
// from another stream is not.
void resetsOnSourceChangeOnly(const std::vector<float>& pcm)
{
    std::size_t loudest = 0;
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        if (std::fabs(pcm[i]) > std::fabs(pcm[loudest]))
            loudest = i;
    }
    const auto part = [&](std::size_t from, std::size_t to) {
        return QVector<float>(pcm.begin() + static_cast<std::ptrdiff_t>(from),
                              pcm.begin() + static_cast<std::ptrdiff_t>(to));
    };
    AetherSDR::PcmStreamDescriptor sliceA;
    sliceA.source = 1;
    sliceA.sliceId = 0;
    AetherSDR::PcmStreamDescriptor sliceB = sliceA;
    sliceB.sliceId = 1;

    Data2gReceiveModel model;
    int beacons = 0;
    QObject::connect(&model, &Data2gReceiveModel::burstHeard,
                     [&](const Data2gHeardBurst& burst) { beacons += isBeacon(burst) ? 1 : 0; });
    model.setEnabled(true);

    model.feedFrom(sliceA, part(0, loudest));
    model.feedFrom(sliceA, part(loudest, pcm.size()));
    check(waitFor([&] { return beacons > 0; }), "one stream in two blocks is one burst");
    settle();
    check(beacons == 1, "one stream in two blocks is reported exactly once");

    model.feedFrom(sliceA, part(0, loudest));
    model.feedFrom(sliceB, part(loudest, pcm.size()));
    settle(1500);
    check(beacons == 1, "a burst is not joined across a change of stream");

    model.feedFrom(sliceB, part(0, pcm.size()));
    check(waitFor([&] { return beacons > 1; }), "the new stream decodes on its own");
}

// Each enable is a new run with its own worker: off and on again decodes again.
void restarts(const std::vector<float>& pcm)
{
    Data2gReceiveModel model;
    int beacons = 0;
    QObject::connect(&model, &Data2gReceiveModel::burstHeard,
                     [&](const Data2gHeardBurst& burst) { beacons += isBeacon(burst) ? 1 : 0; });
    for (int run = 1; run <= 3; ++run) {
        model.setEnabled(true);
        feedBlocks(model, pcm);
        check(waitFor([&] { return beacons == run; }), "each run reports the burst");
        model.setEnabled(false);
        model.feed(QVector<float>(256, 0.0f));
        model.feed(QVector<float>(256, 0.0f), true);
    }
    settle();
    check(beacons == 3, "three runs report three bursts");
}

// Disabling drops what is queued: nothing is reported afterwards.
void disables(const std::vector<float>& pcm)
{
    Data2gReceiveModel model;
    int heard = 0;
    QObject::connect(&model, &Data2gReceiveModel::burstHeard, [&](const Data2gHeardBurst&) { ++heard; });
    model.setEnabled(true);
    feedBlocks(model, pcm);
    model.setEnabled(false);
    settle(1500);
    check(heard == 0, "a model disabled before publishing reports nothing");
}

void overflows()
{
    Data2gReceiveModel model;
    int statsSignals = 0;
    QObject::connect(&model, &Data2gReceiveModel::statsChanged, [&] { ++statsSignals; });
    model.setEnabled(true);
    const qsizetype tooMany = static_cast<qsizetype>(Data2gReceiveModel::kSampleRateHz)
        * Data2gReceiveModel::kMaxQueuedSeconds + 1;
    model.feed(QVector<float>(tooMany, 0.0f));
    check(model.stats().overflows == 1, "a block past the queue bound counts one overflow");
    check(statsSignals == 1, "the overflow is announced");
    model.feed(QVector<float>(256, 0.0f));
    settle(200);
    check(model.stats().overflows == 1, "the model carries on after an overflow");
}

// Destroyed with audio queued and a decode possibly in flight.
void destroyedBusy(const std::vector<float>& pcm)
{
    for (int i = 0; i < 3; ++i) {
        auto model = std::make_unique<Data2gReceiveModel>();
        model->setEnabled(true);
        feedBlocks(*model, pcm);
        if (i == 1)
            settle(50);
        model.reset();
    }
    settle(200);
    check(true, "destroyed while busy");
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const std::vector<std::uint8_t> frame = AetherSDR::test::uiFrame();
    bool sent = false;
    const std::vector<float> pcm = AetherSDR::test::broadcastOnAir(frame, sent);
    check(sent, "the transmitting station sent its frame");

    identity();
    decodes(pcm);
    const qint64 burstOffsetMs = timesBurstStart(pcm);
    survivesGap(pcm, burstOffsetMs);
    resets(pcm);
    keepsDecodedAcrossReset(pcm);
    resetsOnSourceChangeOnly(pcm);
    restarts(pcm);
    disables(pcm);
    overflows();
    destroyedBusy(pcm);
    if (g_failures) {
        std::fprintf(stderr, "data2g_receive_model_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("ok: data2g_receive_model_test");
    return 0;
}
