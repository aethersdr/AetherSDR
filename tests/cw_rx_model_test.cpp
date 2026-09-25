#include "models/CwRxModel.h"
#include <QCoreApplication>
#include <iostream>
#include <QPointer>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <numbers>

using namespace AetherSDR;

namespace {
bool require(bool condition, const char* message)
{
    if (!condition) { std::cerr << message << '\n'; }
    return condition;
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

bool retainLockedEstimates()
{
    CwRxModel decoder;
    QString decoded;
    QObject::connect(&decoder, &CwRxModel::textDecoded, &decoder,
                     [&](const QString& text, float) { decoded += text; });
    decoder.setPitchRange(550, 650);
    decoder.setSpeedRange(18, 22);
    decoder.start();
    // Exercise fixed24 receive ingress with the original source lease.
    // Backend switches below use the inert stub; no model is downloaded.
    const PcmFormat format{24000, PcmLayout::Stereo};
    PcmProducer producer;
    DecoderPcmAdapter adapter;
    adapter.selectRoute(DecoderPcmAdapter::RouteLane::NativeSlice, 11);
    if (!require(producer.start(PcmPurpose::Slice, 11, format),
                 "Selected PCM producer did not start")) { return false; }
    const QVector<float> samples = letterV(format);
    constexpr qsizetype chunk = 2400 * 2;
    for (qsizetype offset = 0; offset < samples.size(); offset += chunk) {
        const auto frame = producer.produce(samples.mid(offset, chunk));
        if (!require(frame.has_value(), "Morse PCM frame rejected")) { return false; }
        const auto block = adapter.accept(*frame);
        if (!require(block.has_value(), "Morse PCM conversion rejected")) { return false; }
        decoder.feedFixed24(*block);
        pump(10);
    }
    pump(300);
    if (!require(decoded.count('V') >= 3, "Real fixed24 worker did not decode Morse V")
        || !require(decoder.estimatedPitch() > 550 && decoder.estimatedPitch() < 650
                        && decoder.estimatedSpeed() > 0,
                    "Worker did not establish positive pitch and speed")) { return false; }
    decoder.lockPitch(true);
    decoder.lockSpeed(true);
    const float pitch = decoder.estimatedPitch();
    const float speed = decoder.estimatedSpeed();
    // Switching away and back must not discard the operator's locks. Uses the
    // inert test backend rather than DeepFist so this runs in the default
    // build — the property belongs to CwRxModel, not to any one backend.
    if (!require(decoder.selectBackend("stub") && !decoder.isRunning(),
                 "Second-backend selection started an inactive worker")
        || !require(decoder.selectBackend("ggmorse") && !decoder.isRunning(),
                    "GGMorse selection started an inactive worker")
        || !require(decoder.estimatedPitch() == pitch && decoder.estimatedSpeed() == speed,
                    "Backend switch discarded locked pitch or speed")) { return false; }
    decoder.start();
    decoder.reset();
    if (!require(decoder.estimatedPitch() == pitch && decoder.estimatedSpeed() == speed,
                 "Reset discarded locked pitch or speed")) { return false; }
    decoder.stop();
    decoder.start();
    if (!require(decoder.estimatedPitch() == pitch && decoder.estimatedSpeed() == speed,
                 "Restart discarded locked pitch or speed")) { return false; }
    decoder.lockPitch(false);
    decoder.lockSpeed(false);
    decoder.stop();
    pump(20);
    return require(decoder.estimatedPitch() == 0 && decoder.estimatedSpeed() == 0,
                   "Unlocked stopped decoder kept old estimates");
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir cache;
    qputenv("AETHER_DEEPFIST_MODEL_DIR", cache.path().toUtf8());
    CwRxModel decoder;
    if (!require(decoder.backendKey() == "ggmorse" && decoder.supportsTuning()
                 && !decoder.isRunning(), "Default RX backend changed")) { return 1; }
    decoder.start();
    if (!require(decoder.isRunning(), "ggmorse did not start")) { return 1; }
    if (!require(!decoder.selectBackend("unknown") && decoder.isRunning()
                 && decoder.backendKey() == "ggmorse", "Invalid selection interrupted RX")) { return 1; }
    decoder.reset();
    if (!require(decoder.isRunning(), "Reset disabled RX")) { return 1; }
    decoder.stop();
    decoder.reset();
    if (!require(!decoder.isRunning(), "Reset enabled stopped RX")) { return 1; }
#ifdef HAVE_DEEPFIST
    if (!require(decoder.selectBackend("deepfist") && !decoder.supportsTuning()
                 && !decoder.isRunning(), "DeepFist selection failed")) { return 1; }
    if (!require(decoder.selectBackend("ggmorse") && decoder.supportsTuning(),
                 "Cannot return to ggmorse")) { return 1; }
#else
    if (!require(!decoder.selectBackend("deepfist")
                 && CwRxModel::availableBackends() == QStringList{"ggmorse"},
                 "Unavailable backend advertised")) { return 1; }
#endif
    int neutral = 0;
    QObject::connect(&decoder, &CwRxModel::statsUpdated, &app,
                     [&](float pitch, float speed) { if (pitch == 0 && speed == 0) { ++neutral; } });
    decoder.start();
    decoder.stop();
    QCoreApplication::processEvents();
    if (!require(neutral > 0, "Stop did not publish neutral RX readings")) { return 1; }

    // A state observer can replace the operation or destroy its facade. The
    // superseded callback must not continue publishing a second notification.
    auto disposable = std::make_unique<CwRxModel>();
    int statuses = 0;
    QObject::connect(disposable.get(), &CwRxModel::statusChanged, &app, [&] { ++statuses; });
    QObject::connect(disposable.get(), &CwRxModel::statsUpdated, &app,
                     [&](float, float) { disposable.reset(); });
    disposable->reset();
    QCoreApplication::processEvents();
    if (!require(!disposable && statuses == 0, "State notification survived facade deletion")) { return 1; }
    // A state observer replacing the backend mid-notification must not let the
    // superseded backend keep publishing. Also on the test backend so it runs
    // without the optional experiment.
    CwRxModel nested;
    bool replaced = false;
    QObject::connect(&nested, &CwRxModel::statsUpdated, &app, [&](float, float) {
        if (!replaced) { replaced = true; nested.selectBackend("ggmorse"); }
    });
    QObject::connect(&nested, &CwRxModel::statusChanged, &app, [&] {
        if (nested.backendKey() != "ggmorse") { ++statuses; }
    });
    nested.selectBackend("stub");
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
    if (!require(replaced && nested.backendKey() == "ggmorse" && statuses == 0,
                 "Superseded backend state continued publishing")) { return 1; }

    // The statusChanged relay is a QueuedConnection, and disconnecting a
    // backend does not retract an event already posted for it. So: make the
    // backend post one, supersede it before the loop runs, and require that the
    // stale post is dropped. Only the generation check can do that.
    CwRxModel superseded;
    int posts = 0;
    QObject::connect(&superseded, &CwRxModel::statusChanged, &app, [&] { ++posts; });
    superseded.selectBackend("stub");
    QCoreApplication::processEvents();
    const int settled = posts;
    superseded.start();                 // stub emits statusChanged -> event queued
    superseded.selectBackend("ggmorse");  // bumps the generation first
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
    if (!require(posts == settled + 1,
                 "Stale queued statusChanged survived the backend switch")) { return 1; }
    return require(!decoder.isRunning(), "Stopped decoder restarted") && retainLockedEstimates() ? 0 : 1;
}
