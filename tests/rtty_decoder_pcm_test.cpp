// Fixed decoder-domain and history tests through the actual worker. Generated
// mark/space PCM is fed directly; no peer, sound device, radio or TX path.
#include "core/DecoderPcmAdapter.h"
#include "core/RttyDecoder.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QThread>

#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <numbers>

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
QVector<float> signal(int rate, PcmLayout layout, const QVector<int>& codes,
                      bool startDuringSpace = false)
{
    QVector<float> samples;
    double phase = 0;
    const int channels = layout == PcmLayout::Mono ? 1 : 2;
    auto append = [&](bool mark, double seconds) {
        const int count = static_cast<int>(std::lround(rate * seconds));
        const double step = 2.0 * std::numbers::pi * (mark ? 2125 : 1955) / rate;
        for (int i = 0; i < count; ++i) {
            const float sample = static_cast<float>(0.5 * std::sin(phase));
            phase = std::fmod(phase + step, 2.0 * std::numbers::pi);
            samples.append(sample);
            if (channels == 2) {
                samples.append(sample * 0.5f);
            }
        }
    };
    // Source selection can occur in the middle of a space. Let both filters
    // settle before idle mark; a cold mark-only preamble can accidentally decode
    // a LTRS shift, concealing a missing explicit FIGS-history reset.
    if (startDuringSpace) {
        append(false, 0.25);
    }
    append(true, 0.25);
    constexpr double bitSeconds = 1.0 / 45.45;
    for (int code : codes) {
        append(false, bitSeconds);
        for (int bit = 0; bit < 5; ++bit) {
            append((code & (1 << bit)) != 0, bitSeconds);
        }
        append(true, 1.5 * bitSeconds);
    }
    append(true, 0.35);
    return samples;
}

void feed(RttyDecoder& decoder, DecoderPcmAdapter& adapter, PcmProducer& producer,
          const QVector<float>& samples, int channels)
{
    int offset = 0;
    constexpr int chunks[] = {1, 17, 255, 11, 1024, 513};
    int chunk = 0;
    while (offset < samples.size()) {
        const int frames = std::min(chunks[chunk++ % 6],
                                    static_cast<int>((samples.size() - offset) / channels));
        const std::optional<PcmFrame> frame = producer.produce(samples.mid(offset, frames * channels));
        const std::optional<DecoderPcmBlock> block = adapter.accept(*frame);
        if (block) {
            decoder.feedPcmBlock(*block);
        }
        offset += frames * channels;
    }
}

void pumpUntil(const std::function<bool()>& done)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (!done() && elapsed.elapsed() < 3000) {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QThread::msleep(2);
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    for (int rate : {24000, 48000}) {
        for (PcmLayout layout : {PcmLayout::Mono, PcmLayout::Stereo}) {
            RttyDecoder decoder;
            QString decoded;
            QObject::connect(&decoder, &RttyDecoder::textDecoded, &app,
                             [&](const QString& text, float) { decoded += text; });
            DecoderPcmAdapter adapter;
            adapter.selectRoute(DecoderPcmAdapter::RouteLane::NativeSlice, 7);
            PcmProducer producer;
            producer.start(PcmPurpose::Slice, 7, {rate, layout});
            decoder.start();
            feed(decoder, adapter, producer, signal(rate, layout, {31, 10, 21}),
                 layout == PcmLayout::Mono ? 1 : 2);
            pumpUntil([&] { return decoded.size() >= 2; });
            check(decoded == QStringLiteral("RY"),
                  "actual RTTY worker decodes RY at fixed24 for every producer rate/layout");
            decoder.stop();
        }
    }

    RttyDecoder decoder;
    QString decoded;
    bool haveStats = false;
    bool locked = false;
    QObject::connect(&decoder, &RttyDecoder::statsUpdated, &app,
                     [&](float, float, float, bool value) { haveStats = true; locked = value; });
    QObject::connect(&decoder, &RttyDecoder::textDecoded, &app,
                     [&](const QString& text, float) { decoded += text; });
    DecoderPcmAdapter adapter;
    adapter.selectRoute(DecoderPcmAdapter::RouteLane::NativeSlice, 3);
    PcmProducer producer;
    producer.start(PcmPurpose::Slice, 3, {24000, PcmLayout::Mono});
    decoder.start();
    feed(decoder, adapter, producer, signal(24000, PcmLayout::Mono, {27, 1}), 1);
    pumpUntil([&] { return !decoded.isEmpty(); });
    check(decoded == QStringLiteral("3"), "FIGS character establishes real detector history");
    pumpUntil([&] { return haveStats && locked; });
    check(haveStats && locked, "real mark/space processing establishes visible signal statistics");
    decoded.clear();
    decoder.resetInput();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    check(!locked, "source reset clears visible detector lock statistics");
    adapter.reset();
    feed(decoder, adapter, producer, signal(24000, PcmLayout::Mono, {1}, true), 1);
    pumpUntil([&] { return !decoded.isEmpty(); });
    check(decoded == QStringLiteral("E"), "source reset retires Baudot FIGS and filter history together");
    decoder.stop();

    // Let worker results queue without delivering them, then revoke the source.
    decoded.clear();
    decoder.start();
    adapter.reset();
    feed(decoder, adapter, producer, signal(24000, PcmLayout::Mono, {31, 10, 21}), 1);
    QThread::msleep(200);
    producer.invalidate();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    check(decoded.isEmpty(), "revoked source cannot publish queued detector results");
    decoder.stop();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    check(!locked, "stop clears visible detector statistics");

    for (int i = 0; i < 12; ++i) {
        decoded.clear();
        decoder.start();
        producer.start(PcmPurpose::Slice, 3, {24000, PcmLayout::Mono});
        adapter.reset();
        feed(decoder, adapter, producer, signal(24000, PcmLayout::Mono, {27, 1}), 1);
        QThread::msleep(25);
        decoder.resetInput();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        check(decoded.isEmpty(), "reset generation refuses queued text while producer remains live");
        decoder.stop();
    }

    decoded.clear();
    decoder.start();
    producer.start(PcmPurpose::Slice, 3, {24000, PcmLayout::Mono});
    adapter.reset();
    feed(decoder, adapter, producer, signal(24000, PcmLayout::Mono, {31, 10, 21}), 1);
    QThread::msleep(25);
    decoder.stop();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    check(decoded.isEmpty(), "stop refuses queued text even while its producer remains live");

    // The legacy stereo24 byte API keeps trim-oldest on overflow, as CwDecoder's
    // does. The observable difference is the neutral reset statistic: retiring
    // detector state on overflow publishes one, trimming the backlog publishes
    // none. Text alone does not separate them -- a decoder that resets on every
    // overflow still emits characters between resets.
    {
        RttyDecoder legacy;
        int neutralResets = 0;
        QObject::connect(&legacy, &RttyDecoder::statsUpdated, &app,
                         [&](float mark, float space, float snr, bool lockedNow) {
                             if (mark == 0.5f && space == 0.5f && snr == 0.0f && !lockedNow) {
                                 ++neutralResets;
                             }
                         });
        legacy.start();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        neutralResets = 0;   // start() publishes one reset of its own

        const QVector<float> mono = signal(24000, PcmLayout::Mono, {31, 10, 21});
        QByteArray stereo(mono.size() * 2 * qsizetype(sizeof(float)), Qt::Uninitialized);
        auto* out = reinterpret_cast<float*>(stereo.data());
        for (qsizetype i = 0; i < mono.size(); ++i) {
            out[2 * i] = mono[i];
            out[2 * i + 1] = mono[i];
        }
        // Tens of seconds of audio into a four-second ring, with no event pump
        // in between, so the worker cannot drain the backlog as it arrives.
        for (int pass = 0; pass < 40; ++pass) {
            legacy.feedAudio(stereo);
        }
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        check(neutralResets == 0,
              "legacy byte overflow trims the oldest samples instead of retiring detector state");
        legacy.stop();
    }

    decoder.start();
    adapter.reset();
    const QMetaObject::Connection stopOnText = QObject::connect(
        &decoder, &RttyDecoder::textDecoded, &app,
        [&](const QString&, float) { decoder.stop(); });
    feed(decoder, adapter, producer, signal(24000, PcmLayout::Mono, {31, 10, 21}), 1);
    pumpUntil([&] { return !decoder.isRunning(); });
    check(decoded == QStringLiteral("R"),
          "stop from first owner-thread text callback joins and suppresses remaining queued text");
    QObject::disconnect(stopOnText);

    // A stopped worker must remain inert even when a live producer supplies
    // malformed PCM. In particular, it must not publish a new reset statistic.
    {
        RttyDecoder stopped;
        int notifications = 0;
        QObject::connect(&stopped, &RttyDecoder::statsUpdated, &app,
                         [&](float, float, float, bool) { ++notifications; });
        DecoderPcmBlock invalid;
        producer.start(PcmPurpose::Slice, 3, {24000, PcmLayout::Mono});
        const std::optional<PcmFrame> frame = producer.produce({0.0f});
        check(frame.has_value(), "stopped-input probe has a live producer lease");
        if (frame) {
            invalid.source = frame->epochLease();
            invalid.samples = {std::numeric_limits<float>::quiet_NaN()};
            stopped.feedPcmBlock(invalid);
            QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
            check(notifications == 0, "stopped RTTY decoder ignores non-finite typed input");
        }
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
