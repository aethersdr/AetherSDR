// Data2gSessionModel (RFC #7159, Stage 2): the admission gate between the
// Data2G session engine and the transmitter.
//
// The transmitter here is a fake that "plays" whatever it is handed into a
// simulated audio channel; the other station is the vendored core's own
// Engine as W1AW. The model runs its real worker thread, stepped
// deterministically through settle(), on a clock the test owns. No radio and
// no sound card are involved.

// The core's headers have members named `slots`, which Qt's keyword macros
// rewrite: they come before any Qt header.
#include "arq/engine.hpp"
#include "arq/session.hpp"
#include "audio/filters.hpp"
#include "config/config.hpp"

#include "models/Data2gSessionModel.h"

#include <QCoreApplication>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <random>
#include <string>
#include <vector>

using AetherSDR::Data2gSessionModel;
using AetherSDR::Data2gTransmitSink;
using AetherSDR::PcmStreamDescriptor;

namespace {

int g_failures = 0;

void check(bool ok, const char* what)
{
    if (ok)
        return;
    ++g_failures;
    std::fprintf(stderr, "FAIL: %s\n", what);
}

struct FakeSink : Data2gTransmitSink {
    QString refusal;
    bool accepts = true;
    int starts = 0;
    int stops = 0;
    float peak = 0.0f;
    std::deque<float> playing;

    QString startRefusal() const override { return refusal; }
    bool start(const QByteArray& pcm, QString& error) override
    {
        if (!accepts) {
            error = QStringLiteral("the radio said no");
            return false;
        }
        ++starts;
        const auto* frames = reinterpret_cast<const float*>(pcm.constData());
        const qsizetype n = pcm.size() / qsizetype(2 * sizeof(float));
        for (qsizetype i = 0; i < n; ++i) {
            playing.push_back(frames[2 * i]);
            peak = std::max(peak, std::fabs(frames[2 * i]));
        }
        return true;
    }
    void stop() override
    {
        ++stops;
        playing.clear();
    }
    bool isActive() const override { return !playing.empty(); }

    // What is on the air for the next n samples.
    std::vector<double> take(std::size_t n)
    {
        std::vector<double> out(n, 0.0);
        const bool was = !playing.empty();
        for (std::size_t i = 0; i < n && !playing.empty(); ++i) {
            out[i] = playing.front();
            playing.pop_front();
        }
        if (was && playing.empty() && onFinished)
            onFinished(true, {});
        return out;
    }
};

struct Bench {
    static constexpr int kRate = 24000;
    static constexpr int kBlock = kRate / 10;
    static constexpr double kLevel = 0.1;
    static constexpr double kNoise = 0.002;

    qint64 nowMs = 0;
    FakeSink sink;
    Data2gSessionModel model;
    data2g::arq::Engine theirs;
    data2g::audio::Decimator toTheirs{kRate};
    data2g::audio::Interpolator toOurs{kRate};
    std::mt19937 rng{7};
    std::normal_distribution<double> noise{0.0, kNoise};
    std::deque<double> fromTheirs;
    PcmStreamDescriptor source;
    bool channelOpen = true;     // false: neither station hears the other
    bool feedOurs = true;        // false: the receive tap has stopped

    QStringList events;
    QStringList aborts;
    QByteArray received;
    std::vector<std::uint8_t> theirReceived;
    std::vector<std::string> theirEvents;

    static data2g::arq::EngineConfig theirConfig()
    {
        data2g::arq::EngineConfig config;
        config.seed = 23;
        return config;
    }

    explicit Bench(Data2gSessionModel::Limits limits = {})
        : model(&sink, limits, [this] { return nowMs; })
        , theirs("W1AW", theirConfig())
    {
        theirs.listen(true);
        source.source = 1;
        source.sliceId = 0;
        model.setMyCall(QStringLiteral("N0CALL"));
        QObject::connect(&model, &Data2gSessionModel::sessionEvent,
                         [this](const QString& e) { events << e; });
        QObject::connect(&model, &Data2gSessionModel::sessionAborted,
                         [this](const QString& why) { aborts << why; });
        QObject::connect(&model, &Data2gSessionModel::dataReceived,
                         [this](const QByteArray& bytes) { received += bytes; });
    }

    bool sawEvent(const char* prefix) const
    {
        return std::any_of(events.begin(), events.end(),
                           [&](const QString& e) { return e.startsWith(QLatin1String(prefix)); });
    }

    void step()
    {
        QVector<float> weHear(kBlock);
        for (float& v : weHear) {
            double tx = 0.0;
            if (!fromTheirs.empty()) {
                tx = fromTheirs.front();
                fromTheirs.pop_front();
            }
            v = static_cast<float>((channelOpen ? kLevel * tx : 0.0) + noise(rng));
        }
        if (feedOurs)
            model.feedFrom(source, weHear);
        model.settle();

        std::vector<double> theyHear = sink.take(kBlock);
        for (double& v : theyHear)
            v = (channelOpen ? kLevel * v : 0.0) + noise(rng);
        const data2g::arq::Engine::Out out = theirs.step(toTheirs(theyHear));
        for (std::string& event : theirs.events())
            theirEvents.push_back(std::move(event));
        const data2g::arq::Bytes got = theirs.session().read();
        theirReceived.insert(theirReceived.end(), got.begin(), got.end());
        const std::vector<double> at24k = toOurs(out.audio);
        fromTheirs.insert(fromTheirs.end(), at24k.begin(), at24k.end());
        nowMs += 100;
    }

    template <typename Done>
    bool runUntil(double seconds, Done done)
    {
        const qint64 end = nowMs + static_cast<qint64>(seconds * 1000);
        while (nowMs < end) {
            step();
            if (done())
                return true;
        }
        return false;
    }

    void run(double seconds)
    {
        runUntil(seconds, [] { return false; });
    }

    bool connect(int bandwidthHz = 2400)
    {
        QString error;
        if (!model.connectTo(QStringLiteral("W1AW"), bandwidthHz, error))
            return false;
        return runUntil(90.0, [&] { return model.state() == Data2gSessionModel::State::Connected; });
    }
};

// Armed by Connect, a whole session passes the gate; when it ends the arm
// goes with it.
void session()
{
    Bench bench;
    check(Data2gSessionModel::available(), "the model is available in this build");
    check(!bench.model.isArmed(), "a new model is not armed");
    bench.run(2.0);
    check(bench.sink.starts == 0, "audio alone starts nothing");

    check(bench.connect(), "the session connects");
    check(bench.model.isArmed(), "Connect armed the gate");
    check(bench.sawEvent("CONNECTED W1AW"), "the model reports CONNECTED");
    check(bench.sink.starts > 0, "the session's transmissions reached the transmitter");
    check(bench.sink.peak > 0.9f && bench.sink.peak <= 1.0f, "full drive puts the peak at full scale");

    const QByteArray toThem("Hello W1AW from the gate test.\r");
    check(bench.model.sendData(toThem), "data is accepted while connected");
    bench.theirs.session().write(std::vector<std::uint8_t>{'O', 'K', '\r'});
    const bool exchanged = bench.runUntil(120.0, [&] {
        return bench.theirReceived.size() >= static_cast<std::size_t>(toThem.size())
            && bench.received.size() >= 3;
    });
    check(exchanged, "data passes both ways through the gate");

    bench.model.disconnectSession();
    const bool closed = bench.runUntil(120.0, [&] { return bench.sawEvent("DISCONNECTED"); });
    check(closed, "the session disconnects");
    const bool disarmed = bench.runUntil(30.0, [&] { return !bench.model.isArmed(); });
    check(disarmed, "the arm ends with the session");
    check(bench.aborts.isEmpty(), "a clean session aborts nothing");

    const int startsAfter = bench.sink.starts;
    bench.theirs.abort();
    bench.theirs.connect("N0CALL", 2);
    bench.run(40.0);
    check(bench.sink.starts == startsAfter, "a disarmed station does not answer a call");
    check(bench.model.state() == Data2gSessionModel::State::Idle, "and stays idle");
    check(!bench.model.sendData("late"), "data is refused with no session");
}

void drive()
{
    Bench bench;
    bench.model.setDriveDb(-6.0);
    check(bench.connect(), "connects at reduced drive");
    check(bench.sink.peak > 0.45f && bench.sink.peak < 0.55f, "-6 dB of drive halves the peak");
    bench.model.setDriveDb(6.0);
    check(bench.model.driveDb() == 0.0, "drive is clamped at full scale");
    bench.model.setDriveDb(-60.0);
    check(bench.model.driveDb() == -20.0, "drive is clamped at -20 dB");
}

// Everything that refuses the arm.
void refusedArm()
{
    QString error;
    {
        Bench bench;
        bench.sink.refusal = QStringLiteral("The bound receiver is not the transmit slice.");
        check(!bench.model.connectTo(QStringLiteral("W1AW"), 2400, error), "a transmitter refusal refuses Connect");
        check(error == bench.sink.refusal, "and its reason is reported");
        check(!bench.model.isArmed(), "nothing is armed");
    }
    {
        Bench bench;
        qputenv("AETHER_AUTOMATION_NO_TX", "1");
        check(!bench.model.connectTo(QStringLiteral("W1AW"), 2400, error), "AETHER_AUTOMATION_NO_TX refuses Connect");
        qunsetenv("AETHER_AUTOMATION_NO_TX");
        check(!bench.model.isArmed(), "nothing is armed under NO_TX");
        bench.run(1.0);
        check(bench.sink.starts == 0, "and nothing is transmitted");
    }
    {
        Bench bench;
        check(!bench.model.connectTo(QStringLiteral("W1AW"), 1000, error), "an unknown bandwidth is refused");
        bench.model.setMyCall(QString());
        check(!bench.model.connectTo(QStringLiteral("W1AW"), 2400, error), "no call sign is refused");
        bench.model.setMyCall(QStringLiteral("NOT A CALL SIGN!"));
        check(!bench.model.connectTo(QStringLiteral("W1AW"), 2400, error), "a call that will not pack is refused");
        check(!bench.model.isArmed(), "none of them armed anything");
    }
    {
        Bench bench;
        check(bench.model.connectTo(QStringLiteral("NOT A CALL SIGN!"), 2400, error), "a bad peer is found by the engine");
        bench.run(1.0);
        check(!bench.model.isArmed() && bench.aborts.size() == 1, "and the arm is dropped at once");
        check(bench.sink.starts == 0, "with nothing transmitted");
    }
    {
        Bench bench;
        check(bench.model.connectTo(QStringLiteral("W1AW"), 2400, error), "first Connect is taken");
        check(!bench.model.connectTo(QStringLiteral("W1AW"), 2400, error), "a second during a session is refused");
    }
}

// Everything that ends the arm early. After each: the transmitter was told to
// stop, the model is idle and disarmed, and nothing more is transmitted.
void abortPaths()
{
    const auto expectAborted = [](Bench& bench, const char* what) {
        check(!bench.model.isArmed(), what);
        check(bench.model.state() == Data2gSessionModel::State::Idle, "the model is idle after the abort");
        check(bench.aborts.size() == 1, "one abort is reported");
        check(bench.sink.stops > 0, "the transmitter was told to stop");
        const int starts = bench.sink.starts;
        bench.run(30.0);
        check(bench.sink.starts == starts, "nothing is transmitted after the abort");
    };
    {
        Bench bench;
        check(bench.connect(), "connected before an operator abort");
        bench.model.abortSession(QStringLiteral("operator"));
        expectAborted(bench, "an operator abort disarms");
    }
    {
        Bench bench;
        check(bench.connect(), "connected before a retune");
        bench.model.receiverChanged();
        expectAborted(bench, "a retune disarms");
    }
    {
        Bench bench;
        check(bench.connect(), "connected before the source changes");
        bench.source.sliceId = 1;
        bench.step();
        expectAborted(bench, "a different receive source disarms");
    }
    {
        Bench bench;
        check(bench.connect(), "connected before the transmitter refuses");
        bench.sink.accepts = false;
        bench.model.sendData("this will need a transmission");
        bench.runUntil(60.0, [&] { return !bench.aborts.isEmpty(); });
        expectAborted(bench, "a refused transmission disarms");
    }
    {
        Bench bench;
        check(bench.connect(), "connected before the transmitter fails");
        bench.model.sendData("this will need a transmission");
        bench.runUntil(60.0, [&] { return bench.sink.isActive(); });
        check(bench.sink.isActive(), "a transmission is playing");
        bench.sink.onFinished(false, QStringLiteral("the key was released"));
        expectAborted(bench, "a failed transmission disarms");
    }
}

// The watchdog enforces the link-lost and idle limits from outside the engine.
// The limits here are far inside upstream's own (90 s and 300 s), so it is
// the watchdog that acts, not the engine.
void watchdog()
{
    {
        Data2gSessionModel::Limits limits;
        limits.linkLostMs = 20'000;
        Bench bench(limits);
        check(bench.connect(), "connected before the link is cut");
        bench.channelOpen = false;
        const bool aborted = bench.runUntil(40.0, [&] { return !bench.aborts.isEmpty(); });
        check(aborted, "the watchdog aborts a session whose peer has gone");
        check(!bench.aborts.isEmpty() && bench.aborts.first().contains(QLatin1String("not been heard")),
              "and says the peer was not heard");
        check(!bench.model.isArmed() && bench.sink.stops > 0, "disarmed and unkeyed");
    }
    {
        Data2gSessionModel::Limits limits;
        limits.idleMs = 20'000;
        Bench bench(limits);
        check(bench.connect(), "connected before going idle");
        const bool aborted = bench.runUntil(60.0, [&] { return !bench.aborts.isEmpty(); });
        check(aborted, "the watchdog aborts an idle session");
        check(!bench.aborts.isEmpty() && bench.aborts.first().contains(QLatin1String("idle")),
              "and says it was idle");
    }
    {
        Data2gSessionModel::Limits limits;
        limits.connectingMs = 6'000;
        Bench bench(limits);
        bench.channelOpen = false;
        QString error;
        check(bench.model.connectTo(QStringLiteral("W1AW"), 2400, error), "Connect into a dead channel is taken");
        const bool aborted = bench.runUntil(20.0, [&] { return !bench.aborts.isEmpty(); });
        check(aborted, "a connect attempt past its limit is aborted");
        check(!bench.model.isArmed(), "and disarmed");
    }
}

// The engine keeps time by its audio. If the receive tap stops while the
// transmitter is busy, the model feeds silence so the engine's clock runs on.
void silentTap()
{
    Bench bench;
    check(bench.connect(), "connected before the tap stops");
    bench.model.sendData(QByteArray(600, 'x'));
    bench.runUntil(60.0, [&] { return bench.sink.isActive(); });
    check(bench.sink.isActive(), "a transmission is playing");
    bench.feedOurs = false;
    bench.run(1.0);
    check(bench.model.stats().paddedMs >= 500, "silence stands in for the missing audio");
    bench.feedOurs = true;
    const bool delivered = bench.runUntil(120.0, [&] { return bench.theirReceived.size() >= 600; });
    check(delivered, "the session carries on and the data arrives");
    check(bench.aborts.isEmpty(), "with no abort");
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    qunsetenv("AETHER_AUTOMATION_NO_TX");
    session();
    drive();
    refusedArm();
    abortPaths();
    watchdog();
    silentTap();
    if (g_failures) {
        std::fprintf(stderr, "data2g_session_model_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("ok: data2g_session_model_test");
    return 0;
}
