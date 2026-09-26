// Socket-free tests of the experimental model contract and real worker.
#include "models/DeepFistCwModel.h"
#include "core/deepfist/DeepFistStream.h"
#include "core/Resampler.h"
#include "DeepFistModel.h"
#include <cstring>
#include "DeepFistDownloadTransport.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <cmath>
#include <algorithm>
#include <functional>
#include <QStringList>
#include <cstdio>
#include <limits>
#include <random>

using namespace AetherSDR;
namespace {
void pump(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}
bool waitFor(const std::function<bool()>& predicate, int limitMs = 5000)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < limitMs) { pump(10); }
    return predicate();
}
bool reentrantLifecycle()
{
    QTemporaryDir cache;
    DeepFistTestNetwork network;
    DeepFistCwModel model(cache.path(), QStringLiteral("https://fixture.invalid/v1"), &network);
    // A direct owner callback may cancel or stop before ensure() begins.
    const auto cancel = QObject::connect(&model, &DeepFistCwModel::statusChanged,
        &model, [&](const QString& status) {
            if (status.contains("Checking")) { model.cancelPreparation(); }
        });
    model.start();
    pump(30);
    if (model.preparing() || !model.canRetry() || !model.status().contains("canceled")
        || network.requests != 0) { return false; }
    QObject::disconnect(cancel);
    model.stop();
    const auto stop = QObject::connect(&model, &DeepFistCwModel::statusChanged,
        &model, [&](const QString& status) {
            if (status.contains("Checking")) { model.stop(); }
        });
    model.start();
    pump(30);
    if (model.isRunning() || model.preparing() || !model.status().isEmpty()
        || network.requests != 0) { return false; }
    QObject::disconnect(stop);
    // Destruction from a status observer is allowed by QObject signal delivery.
    auto disposable = std::make_unique<DeepFistCwModel>(cache.path(), QString{}, &network);
    QObject::connect(disposable.get(), &DeepFistCwModel::statusChanged,
        &model, [&](const QString&) { disposable.reset(); });
    disposable->start();
    pump(20);
    return !disposable && network.requests == 0;
}
bool contract(const QByteArray& validDirectory)
{
    // Completed marks need a falling edge. Padding + carrier onset is not CW.
    std::vector<float> carrier(19200, 0.f);
    for (int i = 6400; i < 19200; ++i) {
        carrier[i] = 0.3f * std::sin(2.0 * 3.141592653589793 * 680.0 * i / 3200.0);
    }
    if (DeepFistStream::hasCompletedMark(carrier.data(), carrier.size())) { return false; }
    for (int i = 8000; i < 19200; ++i) { carrier[i] = 0.f; }
    if (!DeepFistStream::hasCompletedMark(carrier.data(), carrier.size())) { return false; }
    if (DeepFistStream::hasCompletedMark(nullptr, 19200)) { return false; }
    // Developer replay parameters must fail closed before entering DSP.
    DeepFistStream::Parameters parameters;
    if (!parameters.valid() || DeepFistStream(parameters).failed()) { return false; }
    parameters.tickSamples = 0;
    if (parameters.valid() || !DeepFistStream(parameters).failed()) { return false; }
    parameters = {};
    parameters.slowGuardSeconds = 0.5;
    if (parameters.valid()) { return false; }
    parameters = {};
    parameters.activityThreshold = std::numeric_limits<float>::quiet_NaN();
    if (parameters.valid()) { return false; }
    parameters = {};
    parameters.recentActivitySamples = 4096;
    if (parameters.valid()) { return false; } // Short-window admission requires the carrier guard.
    parameters.requireCompletedMark = true;
    if (!parameters.valid()) { return false; }
    parameters.recentActivitySamples = 4095;
    if (parameters.valid()) { return false; }
    parameters.recentActivitySamples = 9601;
    if (parameters.valid()) { return false; }

    QTemporaryDir directory;
    qputenv("AETHER_DEEPFIST_MODEL_DIR", directory.path().toUtf8());
    DeepFistCwModel model;
    QString text;
    QObject::connect(&model, &DeepFistCwModel::textDecoded, &model, [&](const QString& s) { text += s; });
    model.start();
    if (!waitFor([&] { return model.status().contains("unavailable"); })) { return false; }
    if (!model.canRetry() || model.preparing()) { return false; }
    model.retry();
    if (!waitFor([&] { return model.canRetry(); })) { return false; }
    model.stop();
    model.start();
    model.cancelPreparation();
    pump(50);
    if (!model.status().contains("canceled") || model.preparing() || !model.canRetry()) { return false; }
    model.stop();
    // A correctly-sized but corrupt model must fail before ONNX parses it.
    QFile file(directory.filePath("deepfist.onnx"));
    if (!file.open(QIODevice::WriteOnly) || !file.resize(13051998)) { return false; }
    file.close();
    model.start();
    if (!waitFor([&] { return model.status().contains("unavailable"); })) { return false; }
    model.stop();
    // When the real bundle is available, independently corrupt each file while
    // leaving its partner valid. A size-only check must not pass either case.
    if (!validDirectory.isEmpty()) {
        for (const QString& corruptName : {QStringLiteral("deepfist.onnx"),
                                         QStringLiteral("deepfist.onnx.json")}) {
            for (const QString& name : {QStringLiteral("deepfist.onnx"),
                                      QStringLiteral("deepfist.onnx.json")}) {
                QFile::remove(directory.filePath(name));
                if (!QFile::copy(QString::fromUtf8(validDirectory) + '/' + name,
                                directory.filePath(name))) { return false; }
            }
            QFile corrupt(directory.filePath(corruptName));
            if (!corrupt.open(QIODevice::ReadWrite)) { return false; }
            QByteArray byte = corrupt.read(1);
            if (byte.size() != 1 || !corrupt.seek(0)) { return false; }
            byte[0] ^= 1;
            if (corrupt.write(byte) != 1) { return false; }
            corrupt.close();
            model.start();
            if (!waitFor([&] { return model.status().contains("unavailable"); })) { return false; }
            model.stop();
        }
    }
    pump(100);
    return text.isEmpty() && model.status().isEmpty() && !model.isRunning();
}
QVector<float> testAudio(int rate)
{
    QVector<float> audio(rate, 0.f);
    const QStringList words{"-.-. --.-", "- . ... -", "-.-. --.-"}; // CQ TEST CQ
    for (qsizetype w = 0; w < words.size(); ++w) {
        if (w) { audio.resize(audio.size() + qRound(7 * .06 * rate)); }
        const QStringList letters = words[w].split(' ');
        for (qsizetype c = 0; c < letters.size(); ++c) {
            if (c) { audio.resize(audio.size() + qRound(3 * .06 * rate)); }
            for (qsizetype e = 0; e < letters[c].size(); ++e) {
                if (e) { audio.resize(audio.size() + qRound(.06 * rate)); }
                const int length = qRound((letters[c][e] == '.' ? 1 : 3) * .06 * rate);
                for (int i = 0; i < length; ++i) {
                    const double edge = std::min({1.0, i / (.005 * rate), (length - 1 - i) / (.005 * rate)});
                    audio.append(.4f * static_cast<float>(edge * std::sin(2 * 3.141592653589793 * 600 * i / rate)));
                }
            }
        }
    }
    audio.resize(audio.size() + 3 * rate);
    return audio;
}
bool decode(int rate, PcmLayout layout, const QString& directory = {},
            QNetworkAccessManager* network = nullptr)
{
    DeepFistCwModel model(directory.isEmpty() ? DeepFistCwModel::modelDirectory() : directory,
        network ? QStringLiteral("https://fixture.invalid/v1") : QString{}, network);
    QString text;
    QObject::connect(&model, &DeepFistCwModel::textDecoded, &model, [&](const QString& s) { text += s; });
    model.start();
    if (!waitFor([&] { return model.status().contains("ready"); })) { return false; }
    PcmProducer producer;
    if (!producer.start(PcmPurpose::Speaker, -1, {rate, layout})) { return false; }
    const QVector<float> audio = testAudio(rate);
    QTimer timer;
    timer.setTimerType(Qt::PreciseTimer);
    QEventLoop loop;
    qsizetype offset = 0;
    QObject::connect(&timer, &QTimer::timeout, &loop, [&] {
        if (offset >= audio.size()) { timer.stop(); loop.quit(); return; }
        QVector<float> samples;
        const int count = static_cast<int>(std::min<qsizetype>(rate / 50, audio.size() - offset));
        for (int i = 0; i < count; ++i) {
            samples.append(audio[offset + i]);
            if (layout == PcmLayout::Stereo) { samples.append(audio[offset + i]); }
        }
        const auto frame = producer.produce(std::move(samples));
        if (frame) { model.feed(*frame); }
        offset += count;
    });
    timer.start(20);
    loop.exec();
    pump(300);
    producer.invalidate();
    model.reset();
    const QString settled = text;
    pump(300);
    model.stop();
    std::fprintf(stderr, "%d Hz: %s\n", rate, qPrintable(text.simplified()));
    return text == settled && text.simplified() == "CQ TEST CQ";
}
bool carrierRegression(const QByteArray& directory)
{
    lyra::dsp::DeepFistModel model;
    if (!model.load(directory.toStdString())) { return false; }
    QString outputs[2];
    for (int fixed = 0; fixed < 2; ++fixed) {
        DeepFistStream::Parameters parameters;
        parameters.tickSamples = 1920;
        parameters.requireCompletedMark = fixed;
        DeepFistStream stream(parameters);
        Resampler resampler(24000, 3200, 4096);
        for (int offset = 0; offset < 240000; offset += 240) {
            float audio[240];
            for (int i = 0; i < 240; ++i) {
                audio[i] = 0.3f * std::sin(2.0 * 3.141592653589793 * 680.0 * (offset+i) / 24000.0);
            }
            const QByteArray bytes = resampler.process(audio, 240);
            std::vector<float> mono(bytes.size()/4);
            std::memcpy(mono.data(), bytes.constData(), bytes.size());
            outputs[fixed] += stream.process(mono.data(), mono.size(), model);
            if (stream.failed()) { return false; }
        }
    }
    // The application's parameters (activityThreshold 3, #5950) must still hold the carrier off.
    QString appOutput;
    {
        DeepFistStream stream(DeepFistCwModel::appParameters());
        Resampler resampler(24000, 3200, 4096);
        for (int offset = 0; offset < 240000; offset += 240) {
            float audio[240];
            for (int i = 0; i < 240; ++i) {
                audio[i] = 0.3f * std::sin(2.0 * 3.141592653589793 * 680.0 * (offset+i) / 24000.0);
            }
            const QByteArray bytes = resampler.process(audio, 240);
            std::vector<float> mono(bytes.size()/4);
            std::memcpy(mono.data(), bytes.constData(), bytes.size());
            appOutput += stream.process(mono.data(), mono.size(), model);
            if (stream.failed()) { return false; }
        }
    }
    std::fprintf(stderr, "carrier guard disabled: '%s'; enabled: '%s'; app parameters: '%s'\n",
        qPrintable(outputs[0].simplified()), qPrintable(outputs[1].simplified()),
        qPrintable(appOutput.simplified()));
    return outputs[0].simplified() == "9" && outputs[1].simplified().isEmpty()
        && appOutput.simplified().isEmpty();
}
// #5950: off-air CW whose keying ratio falls between dead air and a strong signal was gated
// out at activityThreshold 12. CONSTRUCTED fixture: the CQ TEST CQ tone plus in-band noise
// (40 fixed random-phase tones, 550-650 Hz, seed 5950) — it exercises the gate only and does
// not stand for any measured signal; the field evidence is the recordings attached to #5950.
QString weakSignalDecode(lyra::dsp::DeepFistModel& model, DeepFistStream::Parameters parameters)
{
    constexpr int rate = 3200;
    // In-band noise RMS: from 0.06 to 0.13 the final CQ decodes only with the app's parameters
    // (0.05: both thresholds decode it all); 0.09 sits in the middle.
    constexpr double noiseRms = 0.09;
    const QVector<float> clean = testAudio(rate);
    // std::mt19937's output sequence is fixed by the standard; the distribution classes are
    // not, so the uniform values come straight from the engine to keep this fixture identical
    // on every standard library.
    std::mt19937 generator(5950);
    const auto unit = [&generator] { return static_cast<double>(generator()) / 4294967296.0; };
    double frequency[40];
    double phase[40];
    for (int k = 0; k < 40; ++k) {
        frequency[k] = 550.0 + 100.0 * unit();
        phase[k] = 2.0 * 3.141592653589793 * unit();
    }
    std::vector<float> audio(clean.size());
    for (qsizetype i = 0; i < clean.size(); ++i) {
        double noise = 0.0;
        for (int k = 0; k < 40; ++k) {
            noise += std::sin(2.0 * 3.141592653589793 * frequency[k] * i / rate + phase[k]);
        }
        audio[i] = static_cast<float>(clean[i] + noiseRms * noise / std::sqrt(20.0));
    }
    DeepFistStream stream(parameters);
    QString text;
    for (size_t offset = 0; offset < audio.size(); offset += 320) {
        const int count = static_cast<int>(std::min<size_t>(320, audio.size() - offset));
        text += stream.process(audio.data() + offset, count, model);
        if (stream.failed()) { return QStringLiteral("<failed>"); }
    }
    return text.simplified();
}
bool weakSignal(const QByteArray& directory)
{
    lyra::dsp::DeepFistModel model;
    if (!model.load(directory.toStdString())) { return false; }
    const QString app = weakSignalDecode(model, DeepFistCwModel::appParameters());
    DeepFistStream::Parameters strict = DeepFistCwModel::appParameters();
    strict.activityThreshold = 12.f;
    const QString previous = weakSignalDecode(model, strict);
    std::fprintf(stderr, "weak signal: app parameters '%s'; threshold 12 '%s'\n",
        qPrintable(app), qPrintable(previous));
    // The gate effect: the fading final CQ survives only with the app's parameters.
    return app.endsWith(QStringLiteral(" CQ")) && !previous.endsWith(QStringLiteral("CQ"));
}
bool churn()
{
    DeepFistCwModel model;
    QString text;
    QObject::connect(&model, &DeepFistCwModel::textDecoded, &model, [&](const QString& value) { text += value; });
    for (int cycle = 0; cycle < 8; ++cycle) {
        const int rate = cycle % 2 ? 48000 : 24000;
        const PcmLayout layout = cycle % 3 ? PcmLayout::Mono : PcmLayout::Stereo;
        const int channels = layout == PcmLayout::Stereo ? 2 : 1;
        model.start();
        if (!waitFor([&] { return model.status().contains("ready"); })) { std::fprintf(stderr, "churn cycle %d failed at line %d (processing=%d, queued=%lld, status=%s)\n", cycle, __LINE__, model.processing(), static_cast<long long>(model.queuedFrames()), qPrintable(model.status())); return false; }
        PcmProducer producer;
        if (!producer.start(PcmPurpose::Speaker, -1, {rate, layout})) { std::fprintf(stderr, "churn cycle %d failed at line %d (processing=%d, queued=%lld, status=%s)\n", cycle, __LINE__, model.processing(), static_cast<long long>(model.queuedFrames()), qPrintable(model.status())); return false; }
        // Omit the fixture's one-second pre-roll so the initial bounded batch
        // includes several completed marks and reaches a real inference tick.
        const QVector<float> audio = testAudio(rate).mid(rate);
        const int frames = rate * 9 / 10;
        QVector<float> first;
        first.reserve(frames * channels);
        for (int i = 0; i < frames; ++i) {
            first.append(audio[i]);
            if (channels == 2) { first.append(audio[i]); }
        }
        const auto oldFrame = producer.produce(std::move(first));
        if (!oldFrame) { std::fprintf(stderr, "churn cycle %d failed at line %d (processing=%d, queued=%lld, status=%s)\n", cycle, __LINE__, model.processing(), static_cast<long long>(model.queuedFrames()), qPrintable(model.status())); return false; }
        model.feed(*oldFrame);
        QVector<float> second;
        second.reserve(frames * channels);
        for (int i = frames; i < frames * 2; ++i) {
            second.append(audio[i]);
            if (channels == 2) { second.append(audio[i]); }
        }
        const auto nextFrame = producer.produce(std::move(second));
        if (!nextFrame) { std::fprintf(stderr, "churn cycle %d failed at line %d (processing=%d, queued=%lld, status=%s)\n", cycle, __LINE__, model.processing(), static_cast<long long>(model.queuedFrames()), qPrintable(model.status())); return false; }
        model.feed(*nextFrame);
        if (!waitFor([&] { return model.processing(); })) { std::fprintf(stderr, "churn cycle %d failed at line %d (processing=%d, queued=%lld, status=%s)\n", cycle, __LINE__, model.processing(), static_cast<long long>(model.queuedFrames()), qPrintable(model.status())); return false; }
        // Revoke/reset while real DSP work is active; late output must not escape.
        producer.invalidate();
        model.reset();
        const QString before = text;
        model.feed(*oldFrame);
        if (!waitFor([&] { return !model.processing(); })) { std::fprintf(stderr, "churn cycle %d failed at line %d (processing=%d, queued=%lld, status=%s)\n", cycle, __LINE__, model.processing(), static_cast<long long>(model.queuedFrames()), qPrintable(model.status())); return false; }
        pump(100);
        if (text != before || model.queuedFrames() != 0) { std::fprintf(stderr, "churn cycle %d failed at line %d (processing=%d, queued=%lld, status=%s)\n", cycle, __LINE__, model.processing(), static_cast<long long>(model.queuedFrames()), qPrintable(model.status())); return false; }
        // Overflow with contiguous audio, then inject a gap on the replacement source.
        if (!producer.start(PcmPurpose::Speaker, -1, {rate, layout})) { std::fprintf(stderr, "churn cycle %d failed at line %d (processing=%d, queued=%lld, status=%s)\n", cycle, __LINE__, model.processing(), static_cast<long long>(model.queuedFrames()), qPrintable(model.status())); return false; }
        for (int i = 0; i < 20; ++i) {
            const auto frame = producer.produce(QVector<float>(rate / 4 * channels, 0.f));
            if (!frame) { std::fprintf(stderr, "churn cycle %d failed at line %d (processing=%d, queued=%lld, status=%s)\n", cycle, __LINE__, model.processing(), static_cast<long long>(model.queuedFrames()), qPrintable(model.status())); return false; }
            model.feed(*frame);
            if (model.queuedFrames() > rate * 2) { std::fprintf(stderr, "churn cycle %d failed at line %d (processing=%d, queued=%lld, status=%s)\n", cycle, __LINE__, model.processing(), static_cast<long long>(model.queuedFrames()), qPrintable(model.status())); return false; }
        }
        const auto gap = producer.produce(QVector<float>(rate / 4 * channels, 0.f),
            static_cast<quint64>(rate) * 7, true);
        if (!gap) { return false; }
        model.feed(*gap);
        producer.invalidate();
        model.reset();
        if (!producer.start(PcmPurpose::Speaker, -1, {rate, layout})) { return false; }
        for (int block = 0; block < 2; ++block) {
            QVector<float> samples;
            samples.reserve(frames * channels);
            for (int i = block * frames; i < (block + 1) * frames; ++i) {
                samples.append(audio[i]);
                if (channels == 2) { samples.append(audio[i]); }
            }
            const auto frame = producer.produce(std::move(samples));
            if (!frame) { return false; }
            model.feed(*frame);
        }
        if (!waitFor([&] { return model.processing(); })) { return false; }
        QElapsedTimer timer;
        timer.start();
        model.stop();
        if (timer.elapsed() > 5000 || model.processing() || model.queuedFrames() != 0) { std::fprintf(stderr, "churn cycle %d failed at line %d (processing=%d, queued=%lld, status=%s)\n", cycle, __LINE__, model.processing(), static_cast<long long>(model.queuedFrames()), qPrintable(model.status())); return false; }
        pump(50);
        if (text != before) { std::fprintf(stderr, "churn cycle %d failed at line %d (processing=%d, queued=%lld, status=%s)\n", cycle, __LINE__, model.processing(), static_cast<long long>(model.queuedFrames()), qPrintable(model.status())); return false; }
    }
    return true;
}
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QByteArray directory = qgetenv("AETHER_DEEPFIST_MODEL_DIR");
    // Each test process owns its cache: parallel CTest/sanitizer runs must not
    // compete for the production downloader's exclusive cache lock.
    QTemporaryDir cache;
    if (!directory.isEmpty()) {
        if (!cache.isValid()) { return 1; }
        for (const QString& name : {QStringLiteral("deepfist.onnx"),
                                  QStringLiteral("deepfist.onnx.json"), QStringLiteral("LICENSE")}) {
            if (!QFile::copy(QString::fromUtf8(directory) + '/' + name, cache.filePath(name))) { return 1; }
        }
        directory = cache.path().toUtf8();
        qputenv("AETHER_DEEPFIST_MODEL_DIR", directory);
    }
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == "--carrier") {
        if (directory.isEmpty()) { return 77; }
        return carrierRegression(directory) ? 0 : 1;
    }
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == "--weak") {
        if (directory.isEmpty()) { return 77; }
        return weakSignal(directory) ? 0 : 1;
    }
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == "--churn") {
        if (directory.isEmpty()) { return 77; }
        return churn() ? 0 : 1;
    }
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == "--download-infer") {
        if (directory.isEmpty()) { return 77; }
        QTemporaryDir cache;
        DeepFistTestNetwork network;
        for (const QString& name : {QStringLiteral("deepfist.onnx"),
                                  QStringLiteral("deepfist.onnx.json"), QStringLiteral("LICENSE")}) {
            QFile file(QString::fromUtf8(directory) + '/' + name);
            if (!file.open(QIODevice::ReadOnly)) { return 1; }
            network.files[name] = {file.readAll()};
        }
        const bool downloaded = decode(24000, PcmLayout::Stereo, cache.path(), &network);
        const bool threeFiles = network.requests == 3;
        network.files.clear(); // Cached second run must work with an unavailable source.
        const bool offline = decode(48000, PcmLayout::Mono, cache.path(), &network);
        return downloaded && offline && threeFiles && network.requests == 3 ? 0 : 1;
    }
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == "--infer") {
        if (directory.isEmpty()) { return 77; }
        return decode(24000, PcmLayout::Mono) && decode(24000, PcmLayout::Stereo)
            && decode(48000, PcmLayout::Mono) && decode(48000, PcmLayout::Stereo) ? 0 : 1;
    }
    return reentrantLifecycle() && contract(directory) ? 0 : 1;
}
