// Socket-free typed-input lifecycle checks against the real CwDecoder worker.
#include "core/CwDecoder.h"
#include "ggmorse/ggmorse.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLoggingCategory>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

using namespace AetherSDR;

namespace AetherSDR {
Q_LOGGING_CATEGORY(lcDsp, "tests.cwdecoder.pcm", QtWarningMsg)
}

namespace {
int failures = 0;
int checks = 0;

void check(bool condition, const char* message)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
}

void pump(int milliseconds)
{
    QElapsedTimer timer;
    timer.start();
    do {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    } while (timer.elapsed() < milliseconds);
}

QVector<float> makeMorse()
{
    GGMorse::Parameters parameters = GGMorse::getDefaultParameters();
    parameters.sampleRateOut = 24000.0f;
    parameters.sampleFormatOut = GGMORSE_SAMPLE_FORMAT_F32;
    GGMorse encoder(parameters);
    GGMorse::ParametersEncode encoding = GGMorse::getDefaultParametersEncode();
    encoding.volume = 0.6f;
    encoding.frequency_hz = 600.0f;
    encoding.speedCharacters_wpm = 20.0f;
    encoding.speedFarnsworth_wpm = 20.0f;
    constexpr char message[] = "VVV VVV VVV VVV VVV";
    if (!encoder.setParametersEncode(encoding)
        || !encoder.init(sizeof(message) - 1, message)) { return {}; }
    QByteArray bytes;
    if (!encoder.encode([&](const void* data, uint32_t count) {
        bytes.append(static_cast<const char*>(data), count);
    })) { return {}; }
    QVector<float> samples(bytes.size() / sizeof(float));
    std::memcpy(samples.data(), bytes.constData(), bytes.size());
    samples.resize(samples.size() + 24000); // Allow the final mark to settle.
    return samples;
}

struct Fixture {
    CwDecoder decoder;
    PcmProducer producer;
    PcmEpochLease source;
    QString text;
    QVector<std::pair<float, float>> statistics;

    Fixture()
    {
        check(producer.start(PcmPurpose::Slice, 3, {24000, PcmLayout::Mono}),
              "typed source starts");
        const auto frame = producer.produce(QVector<float>(1, 0.0f));
        check(frame.has_value(), "typed source supplies a live lease");
        if (frame) { source = frame->epochLease(); }
        QObject::connect(&decoder, &CwDecoder::textDecoded, &decoder,
                         [this](const QString& value, float) { text += value; });
        QObject::connect(&decoder, &CwDecoder::statsUpdated, &decoder,
                         [this](float pitch, float speed) { statistics.append({pitch, speed}); });
        decoder.setPitchRange(550, 650);
        decoder.setSpeedRange(18, 22);
        decoder.start();
    }

    void feed(const QVector<float>& samples, bool discontinuity = false)
    {
        DecoderPcmBlock block;
        block.source = source;
        block.samples = samples;
        block.discontinuity = discontinuity;
        decoder.feedPcmBlock(block);
    }

    void feedLegacy(const QVector<float>& samples)
    {
        QByteArray stereo(samples.size() * 2 * sizeof(float), Qt::Uninitialized);
        for (qsizetype i = 0; i < samples.size(); ++i) {
            const float pair[] = {samples[i], samples[i]};
            std::memcpy(stereo.data() + i * sizeof(pair), pair, sizeof(pair));
        }
        decoder.feedAudio(stereo);
    }

    bool feedWithoutDelivery(const QVector<float>& samples, bool typed = true)
    {
        // Do not run the owner event loop: both the first typed-feed neutral
        // callback and any worker results remain queued until the test acts.
        for (qsizetype offset = 0; offset < samples.size(); offset += 2400) {
            if (typed) {
                feed(samples.mid(offset, 2400));
            } else {
                feedLegacy(samples.mid(offset, 2400));
            }
            QThread::msleep(20);
        }
        QElapsedTimer timer;
        timer.start();
        while (decoder.estimatedPitch() <= 0 && timer.elapsed() < 1500) {
            QThread::msleep(5);
        }
        QThread::msleep(100);
        const bool live = decoder.estimatedPitch() > 550 && decoder.estimatedPitch() < 650;
        check(live, "real worker obtains a live estimate before lifecycle action");
        check(text.isEmpty() && statistics.isEmpty(), "owner-thread result delivery is withheld");
        return live;
    }

    void expectNeutral()
    {
        pump(100);
        check(text.isEmpty(), "invalidated generation publishes no queued decoded text");
        check(!statistics.isEmpty(), "queued neutral callback actually executes");
        check(std::all_of(statistics.begin(), statistics.end(), [](const auto& value) {
            return value.first == 0.0f && value.second == 0.0f;
        }), "neutral callback never republishes revoked unlocked estimates");
        check(decoder.estimatedPitch() == 0 && decoder.estimatedSpeed() == 0,
              "unlocked estimates are neutral after lifecycle action");
    }
};

void positiveControl(const QVector<float>& samples)
{
    Fixture fixture;
    if (!fixture.feedWithoutDelivery(samples)) { return; }
    pump(200);
    check(fixture.text.count('V') >= 3, "queued production worker results contain generated Morse V");
    check(std::any_of(fixture.statistics.begin(), fixture.statistics.end(), [](const auto& value) {
        return value.first > 550 && value.first < 650 && value.second > 0;
    }), "live source publishes positive pitch and speed");
    std::fprintf(stderr, "positive Morse: %s\n", qPrintable(fixture.text.simplified()));
}

void revokeWithoutReset(const QVector<float>& samples)
{
    Fixture fixture;
    if (!fixture.feedWithoutDelivery(samples)) { return; }
    fixture.producer.invalidate();
    check(fixture.decoder.estimatedPitch() == 0 && fixture.decoder.estimatedSpeed() == 0,
          "lease revocation neutralizes getters before owner event delivery");
    // The first typed-feed reset callback is still queued, while raw worker
    // estimates are positive. Delivery must read the guarded public values.
    fixture.expectNeutral();
    fixture.decoder.lockPitch(true);
    check(fixture.decoder.estimatedPitch() == 0 && fixture.decoder.estimatedSpeed() == 0,
          "pitch lock cannot resurrect a revoked unlocked estimate");
    fixture.decoder.lockSpeed(true);
    check(fixture.decoder.estimatedPitch() == 0 && fixture.decoder.estimatedSpeed() == 0,
          "speed lock cannot resurrect a revoked unlocked estimate");
    fixture.decoder.stop();
    fixture.decoder.start();
    check(fixture.decoder.isPitchLocked() && fixture.decoder.isSpeedLocked()
              && fixture.decoder.estimatedPitch() == 0 && fixture.decoder.estimatedSpeed() == 0,
          "restart preserves neutral locks rather than retired setpoints");
    fixture.decoder.setKnownParameters(650.0f, 25.0f);
    check(fixture.decoder.estimatedPitch() == 650 && fixture.decoder.estimatedSpeed() == 25,
          "explicit operator setpoints still replace neutral locks");
}

enum class Boundary { Reset, Discontinuity, Nonfinite, Overflow };

void discardGeneration(const QVector<float>& samples, Boundary boundary)
{
    Fixture fixture;
    if (!fixture.feedWithoutDelivery(samples)) { return; }
    switch (boundary) {
    case Boundary::Reset:
        fixture.decoder.resetInput();
        break;
    case Boundary::Discontinuity:
        // A converter can stage input without producing samples. Its empty
        // discontinuity block must still invalidate already queued results.
        fixture.feed({}, true);
        break;
    case Boundary::Nonfinite:
        fixture.feed({0.0f, std::numeric_limits<float>::quiet_NaN()});
        break;
    case Boundary::Overflow:
        // A bounded burst greatly exceeds the four-second input ring. It
        // must invalidate the old result generation, not just trim bytes.
        for (int i = 0; i < 8; ++i) {
            fixture.feed(QVector<float>(PcmFrame::kMaxFrames, 0.0f));
        }
        break;
    }
    fixture.expectNeutral();
}

void stopRestart(const QVector<float>& samples)
{
    Fixture fixture;
    if (!fixture.feedWithoutDelivery(samples)) { return; }
    fixture.decoder.stop();
    fixture.decoder.start();
    pump(100);
    check(fixture.text.isEmpty() && fixture.statistics.isEmpty(),
          "restart rejects prior text, worker statistics and queued stop callback");
    check(fixture.decoder.estimatedPitch() == 0 && fixture.decoder.estimatedSpeed() == 0,
          "restart begins with no unlocked estimates");
    if (!fixture.feedWithoutDelivery(samples)) { return; }
    pump(200);
    check(fixture.text.count('V') >= 3, "restarted worker recognizes new typed Morse input");
}

void preserveLocksAndNumericBoundary()
{
    Fixture fixture;
    fixture.decoder.setKnownParameters(650.0f, 25.0f);
    fixture.feed({});
    fixture.feed({std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), 0.0f});
    fixture.feed({std::numeric_limits<float>::infinity()});
    fixture.decoder.resetInput();
    fixture.feed({}, true);
    fixture.producer.invalidate();
    fixture.decoder.stop();
    fixture.decoder.start();
    pump(100);
    check(fixture.decoder.isPitchLocked() && fixture.decoder.isSpeedLocked(),
          "typed reset, invalid input, retirement and restart preserve operator locks");
    check(fixture.decoder.estimatedPitch() == 650 && fixture.decoder.estimatedSpeed() == 25,
          "locked setpoints survive typed lifecycle changes");
    check(fixture.text.isEmpty(), "empty and invalid typed input produces no Morse text");
}

void preserveLegacyOverflow(const QVector<float>& samples)
{
    Fixture fixture;
    if (!fixture.feedWithoutDelivery(samples, false)) { return; }
    // TX sidetone's byte API historically trims old buffered audio. A backlog
    // must not retire the detector or decoded results already queued for the UI.
    const QVector<float> silence(PcmFrame::kMaxFrames, 0.0f);
    for (int i = 0; i < 32; ++i) {
        fixture.feedLegacy(silence);
    }
    pump(200);
    check(fixture.text.count('V') >= 3,
          "legacy sidetone overflow preserves queued decoded Morse results");
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QVector<float> samples = makeMorse();
    check(!samples.isEmpty(), "Morse fixture generates finite mono 24 kHz PCM");
    if (samples.isEmpty()) { return 1; }
    positiveControl(samples);
    revokeWithoutReset(samples);
    for (Boundary boundary : {Boundary::Reset, Boundary::Discontinuity,
                              Boundary::Nonfinite, Boundary::Overflow}) {
        discardGeneration(samples, boundary);
    }
    stopRestart(samples);
    preserveLocksAndNumericBoundary();
    preserveLegacyOverflow(samples);
    std::printf("cw_decoder_pcm_lifecycle_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
