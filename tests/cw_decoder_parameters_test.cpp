#include "core/CwDecoder.h"
#include "ggmorse/ggmorse.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLoggingCategory>
#include <QThread>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>

namespace {

int g_failures = 0;

void expect(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++g_failures;
    }
}

bool near(float actual, float expected)
{
    return std::abs(actual - expected) < 0.01f;
}

void pumpEvents(int milliseconds)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < milliseconds) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
}

QByteArray makeCwPcm(float frequencyHz)
{
    GGMorse::Parameters parameters = GGMorse::getDefaultParameters();
    parameters.sampleRateOut = 24000.0f;
    parameters.sampleFormatOut = GGMORSE_SAMPLE_FORMAT_F32;
    GGMorse encoder(parameters);

    GGMorse::ParametersEncode encodeParameters = GGMorse::getDefaultParametersEncode();
    encodeParameters.volume = 0.60f;
    encodeParameters.frequency_hz = frequencyHz;
    encodeParameters.speedCharacters_wpm = 20.0f;
    encodeParameters.speedFarnsworth_wpm = 20.0f;
    if (!encoder.setParametersEncode(encodeParameters)) {
        return {};
    }

    constexpr char kMessage[] = "VVV VVV VVV VVV VVV";
    if (!encoder.init(static_cast<int>(sizeof(kMessage) - 1), kMessage)) {
        return {};
    }

    QByteArray mono;
    if (!encoder.encode([&mono](const void* data, uint32_t byteCount) {
            mono.append(static_cast<const char*>(data), static_cast<int>(byteCount));
        })) {
        return {};
    }

    const int sampleCount = mono.size() / static_cast<int>(sizeof(float));
    QByteArray stereo(sampleCount * 2 * static_cast<int>(sizeof(float)), Qt::Uninitialized);
    const auto* source = reinterpret_cast<const float*>(mono.constData());
    auto* destination = reinterpret_cast<float*>(stereo.data());
    for (int sample = 0; sample < sampleCount; ++sample) {
        destination[2 * sample] = source[sample];
        destination[2 * sample + 1] = source[sample];
    }
    return stereo;
}

void feedChunk(AetherSDR::CwDecoder& decoder, const QByteArray& pcm, int chunkIndex)
{
    constexpr int kChunkSamples = 2400; // 100 ms at the public 24 kHz stereo float format.
    const int bytesPerSample = 2 * static_cast<int>(sizeof(float));
    const int offset = (chunkIndex * kChunkSamples * bytesPerSample) % pcm.size();
    const int byteCount = std::min(kChunkSamples * bytesPerSample,
                                   static_cast<int>(pcm.size()) - offset);
    decoder.feedAudio(pcm.mid(offset, byteCount));
}

void churnParameters(AetherSDR::CwDecoder& decoder, const QByteArray& pcm, int iterations = 200)
{
    for (int iteration = 0; iteration < iterations; ++iteration) {
        feedChunk(decoder, pcm, iteration);

        // Every owning-thread setter runs while the decoder worker has queued
        // audio. The duplicate known-value calls exercise the documented no-op.
        decoder.setKnownParameters(650.0f, 25.0f);
        decoder.setKnownParameters(650.0f, 25.0f);
        decoder.lockPitch(false);
        decoder.lockSpeed(false);
        decoder.setPitchRange(450 + (iteration % 3) * 25, 850);
        decoder.setSpeedRange(10 + (iteration % 3), 40);
        decoder.lockPitch(true);
        decoder.lockSpeed(true);
        decoder.setKnownParameters(600.0f, 20.0f);
        decoder.setKnownParameters(600.0f, 20.0f);
        pumpEvents(5);
    }
}

void feedKnownSignal(AetherSDR::CwDecoder& decoder, const QByteArray& pcm)
{
    const int bytesPerChunk = 2400 * 2 * static_cast<int>(sizeof(float));
    for (int offset = 0; offset < pcm.size(); offset += bytesPerChunk) {
        decoder.feedAudio(pcm.mid(offset, bytesPerChunk));
        pumpEvents(5);
    }
}

} // namespace

namespace AetherSDR {
Q_LOGGING_CATEGORY(lcDsp, "tests.cwdecoder", QtWarningMsg)
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    const QByteArray pcm = makeCwPcm(600.0f);
    expect(!pcm.isEmpty(), "fixture generates 24 kHz stereo float CW PCM");
    if (pcm.isEmpty()) {
        return 1;
    }

    // Locking before the first estimate captures zero, which GGMorse treats
    // as auto-detection. The public estimate must still become live.
    {
        AetherSDR::CwDecoder initiallyLocked;
        initiallyLocked.lockPitch(true);
        initiallyLocked.lockSpeed(true);
        initiallyLocked.start();
        feedKnownSignal(initiallyLocked, pcm);
        pumpEvents(1000);
        expect(initiallyLocked.estimatedPitch() > 0.0f,
               "locking an unknown pitch still publishes automatic estimates");
        expect(initiallyLocked.estimatedSpeed() > 0.0f,
               "locking an unknown speed still publishes automatic estimates");
        initiallyLocked.stop();
    }

    AetherSDR::CwDecoder decoder;
    int statsUpdates = 0;
    QString decodedText;
    QVector<float> observedPitches;
    QObject::connect(&decoder, &AetherSDR::CwDecoder::statsUpdated, &app,
                     [&statsUpdates, &observedPitches](float pitchHz, float) {
                         ++statsUpdates;
                         observedPitches.append(pitchHz);
                     });
    QObject::connect(&decoder, &AetherSDR::CwDecoder::textDecoded, &app,
                     [&decodedText](const QString& text, float) { decodedText += text; });

    // Store known parameters before start and retain their public lock state.
    decoder.setPitchRange(450, 850);
    decoder.setSpeedRange(10, 40);
    decoder.setKnownParameters(600.0f, 20.0f);
    decoder.setKnownParameters(600.0f, 20.0f);
    expect(decoder.isPitchLocked() && decoder.isSpeedLocked(),
           "pre-start known parameters lock both dimensions");
    expect(near(decoder.estimatedPitch(), 600.0f) && near(decoder.estimatedSpeed(), 20.0f),
           "pre-start known parameters retain their requested values");

    decoder.start();
    expect(decoder.isRunning(), "decoder starts after pre-start configuration");
    churnParameters(decoder, pcm);

    // Hold the transmitted parameters steady for an independently generated,
    // valid CW signal. A V (rather than GGMorse's pitch-change newline) proves
    // the production worker completed Morse-symbol recognition after churn.
    decoder.setKnownParameters(600.0f, 20.0f);
    decodedText.clear();
    feedKnownSignal(decoder, pcm);
    pumpEvents(3000);
    expect(statsUpdates > 0, "worker consumes PCM and publishes decoder statistics");
    expect(decodedText.contains(QLatin1Char('V')),
           "worker recognizes generated V characters after parameter churn");

    decoder.stop();
    pumpEvents(20);
    expect(!decoder.isRunning(), "decoder stops after active parameter churn");
    expect(decoder.isPitchLocked() && decoder.isSpeedLocked(),
           "locked known parameters survive stop for restart");
    expect(near(decoder.estimatedPitch(), 600.0f) && near(decoder.estimatedSpeed(), 20.0f),
           "stop retains locked known parameter values");

    // Set an auto-pitch range while stopped. The subsequent worker must start
    // from this snapshot; no live setter may republish it after start.
    decoder.lockPitch(false);
    decoder.lockSpeed(false);
    decoder.setPitchRange(800, 900);
    decoder.setSpeedRange(18, 22);
    pumpEvents(20);
    observedPitches.clear();
    decoder.start();
    feedKnownSignal(decoder, pcm);
    pumpEvents(1000);
    expect(!observedPitches.isEmpty(),
           "restart consumes PCM and publishes fresh auto-pitch observations");
    if (!observedPitches.isEmpty()) {
        expect(observedPitches.constLast() >= 795.0f && observedPitches.constLast() <= 905.0f,
               "restart applies the stopped 800--900 Hz auto-pitch range");
    }
    decoder.stop();
    pumpEvents(20);
    expect(!decoder.isPitchLocked() && !decoder.isSpeedLocked(),
           "explicit unlock survives stop");
    expect(near(decoder.estimatedPitch(), 0.0f) && near(decoder.estimatedSpeed(), 0.0f),
           "stop clears only unlocked estimates");

    decoder.stop();

    // Destruction while the worker owns queued audio must join the bounded
    // decode loop before its buffers and pending configuration are destroyed.
    {
        AetherSDR::CwDecoder decoderDestroyedWhileRunning;
        decoderDestroyedWhileRunning.setKnownParameters(600.0f, 20.0f);
        decoderDestroyedWhileRunning.start();
        churnParameters(decoderDestroyedWhileRunning, pcm, 24);
        expect(decoderDestroyedWhileRunning.isRunning(),
               "active decoder reaches its destructor with a running worker");
    }

    return g_failures == 0 ? 0 : 1;
}
