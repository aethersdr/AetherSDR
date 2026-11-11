// Data2gReceiveModel (RFC #7159, Stage 1): the threaded receive-only monitor
// AetherModem drives. Injected 24 kHz PCM and the real worker thread; no radio,
// no sockets, no sound device, and nothing that can transmit.

// The core's headers first: they have members named `slots`, which Qt's
// keyword macros would rewrite once any Qt header has been seen.
#include "data2g_test_signal.h"

#include "models/Data2gReceiveModel.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QEventLoop>
#include <QVector>

#include <cstdio>
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

// A source change mid-burst, then the whole burst: reported exactly once.
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
    feedBlocks(model, pcm, 0, loudest);
    settle();
    model.reset();
    feedBlocks(model, pcm);
    check(waitFor([&] { return beacons > 0; }), "after a reset the whole burst is reported");
    settle();
    check(beacons == 1, "after a reset the burst is reported exactly once");

    // The same through a block marked discontinuous.
    feedBlocks(model, pcm, 0, loudest);
    settle();
    model.feed(QVector<float>(256, 0.0f), true);
    feedBlocks(model, pcm);
    check(waitFor([&] { return beacons > 1; }), "after a discontinuity the whole burst is reported");
    settle();
    check(beacons == 2, "after a discontinuity the burst is reported exactly once");
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
    resets(pcm);
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
