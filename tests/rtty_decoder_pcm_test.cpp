// Fixed decoder-domain and history tests through the actual worker. Generated
// mark/space PCM is fed directly; no peer, sound device, radio or TX path.
#include "core/DecoderPcmAdapter.h"
#include "core/RttyDecoder.h"
#include "core/backends/sim/NoiseMixer.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <numbers>
#include <utility>
#include <vector>

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
                      bool startDuringSpace = false, int markHz = 2125, int spaceHz = 1955)
{
    QVector<float> samples;
    double phase = 0;
    const int channels = layout == PcmLayout::Mono ? 1 : 2;
    auto append = [&](bool mark, double seconds) {
        const int count = static_cast<int>(std::lround(rate * seconds));
        const double step = 2.0 * std::numbers::pi * (mark ? markHz : spaceHz) / rate;
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

// Like signal(), with the other tone mixed into every bit at `other` times the read tone's
// amplitude: each character then scores by how much of the other tone is heard.
QVector<float> blendedSignal(const QVector<std::pair<int, double>>& codes)
{
    constexpr int rate = 24000;
    QVector<float> samples;
    double markPhase = 0;
    double spacePhase = 0;
    auto append = [&](bool mark, double other, double seconds) {
        const int count = static_cast<int>(std::lround(rate * seconds));
        const double markStep = 2.0 * std::numbers::pi * 2125 / rate;
        const double spaceStep = 2.0 * std::numbers::pi * 1955 / rate;
        for (int i = 0; i < count; ++i) {
            const double m = std::sin(markPhase);
            const double sp = std::sin(spacePhase);
            samples.append(static_cast<float>(0.25 * (mark ? m + other * sp : sp + other * m)));
            markPhase = std::fmod(markPhase + markStep, 2.0 * std::numbers::pi);
            spacePhase = std::fmod(spacePhase + spaceStep, 2.0 * std::numbers::pi);
        }
    };
    // Space, then idle mark, as signal(..., startDuringSpace): from a cold start on mark
    // alone the filters can frame a LTRS, which would count toward the average.
    append(false, 0.0, 0.25);
    append(true, 0.0, 0.25);
    constexpr double bitSeconds = 1.0 / 45.45;
    for (const auto& [code, other] : codes) {
        append(false, other, bitSeconds);
        for (int bit = 0; bit < 5; ++bit) {
            append((code & (1 << bit)) != 0, other, bitSeconds);
        }
        append(true, other, 1.5 * bitSeconds);
    }
    append(true, 0.0, 0.35);
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

bool pumpUntil(const std::function<bool()>& done)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (!done() && elapsed.elapsed() < 3000) {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QThread::msleep(2);
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    return done();
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

    // The demo radio's RTTY stations (#7420) through the real decoder. NoiseMixer
    // keys them; the tone pitches are what SimSignalSource sets in RTTY mode with
    // the dial on the upper tone (2125 Hz; the tone 170 Hz below it at 2295 Hz),
    // or on the lower tone (2125 Hz; the upper one then at 1955 Hz).
    {
        struct DemoRun {
            bool copied = false;
            int chars = 0;
            float minConf = 1.0f;
            double sumConf = 0.0;
            int readings = 0;       // every statsUpdated, the two neutral ones at the start included
            int ticks = 0;          // statistics after the first character
            int lockedTicks = 0;
            bool everLocked = false;
            bool lastLocked = false;
            float lastLevel = 0.0f;
            float minLevel = 1.0f;
            float maxLevel = 0.0f;
            std::vector<std::pair<bool, float>> series;   // every reading: LOCKED, level
        };
        bool allProcessed = true;
        // The demo's rows mixed for `seconds`, and silence; both in whole 0.5 s blocks here.
        auto mix = [&](std::vector<NoiseMixer::Channel> channels, double upperHz, double lowerHz, double seconds,
                       double whiteDb = 0.0) {
            NoiseMixer mixer;
            for (NoiseMixer::Channel channel : channels) {
                mixer.setEnabled(channel, true);
            }
            mixer.setLevelDb(NoiseMixer::Channel::White, whiteDb);
            mixer.setKnob(NoiseMixer::Channel::Birdie, QStringLiteral("hz"), upperHz);
            mixer.setRttyTones(upperHz, lowerHz, 0.0, -170.0);
            const auto samples = static_cast<qsizetype>(seconds * 24000);
            QVector<float> pcm;
            while (pcm.size() < samples) {
                pcm += mixer.mixFrame();
            }
            pcm.resize(samples);
            return pcm;
        };
        auto silence = [](double seconds) { return QVector<float>(static_cast<qsizetype>(seconds * 24000), 0.0f); };
        auto decode = [&](const QVector<float>& pcm, bool rev) {
            RttyDecoder demo;
            QString text;
            DemoRun result;
            QObject::connect(&demo, &RttyDecoder::textDecoded, &app, [&](const QString& t, float conf) {
                text += t;
                ++result.chars;
                result.minConf = std::min(result.minConf, conf);
                result.sumConf += conf;
            });
            QObject::connect(&demo, &RttyDecoder::statsUpdated, &app, [&](float level, float, bool locked) {
                ++result.readings;
                result.series.emplace_back(locked, level);
                if (result.chars > 0) {
                    ++result.ticks;
                    result.lockedTicks += locked ? 1 : 0;
                }
                result.everLocked |= locked;
                result.lastLocked = locked;
                result.lastLevel = level;
                result.minLevel = std::min(result.minLevel, level);
                result.maxLevel = std::max(result.maxLevel, level);
            });
            DecoderPcmAdapter demoAdapter;
            demoAdapter.selectRoute(DecoderPcmAdapter::RouteLane::NativeSlice, 8);
            PcmProducer demoProducer;
            demoProducer.start(PcmPurpose::Slice, 8, {24000, PcmLayout::Mono});
            demo.setReversePolarity(rev);
            demo.start();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
            // Paced by the decoder's own readings: a neutral one from start() (delivered just
            // above), another when the first block arrives (a new source), then one per 12000
            // samples processed, and every run is a whole number of those. Half a second at a
            // time, at most 2.5 s ahead: the typed input ring holds 4 s and a source that outruns
            // it is treated as a discontinuity (reset). The run ends when every sample is processed.
            const int blocks = static_cast<int>(pcm.size() / 12000);
            bool paced = true;
            for (int block = 0; block < blocks && paced; ++block) {
                paced = pumpUntil([&] { return block - (result.readings - 2) <= 4; });
                feed(demo, demoAdapter, demoProducer, pcm.mid(block * 12000, 12000), 1);
            }
            paced = paced && pumpUntil([&] { return result.readings >= 2 + blocks; });
            demo.stop();
            allProcessed = allProcessed && paced && result.readings == 2 + blocks;
            result.copied = text.contains(QStringLiteral("DE DEMO DEMO K"));
            return result;
        };
        // ~14 s: two passes of the station's text.
        auto run = [&](std::vector<NoiseMixer::Channel> channels, double upperHz, double lowerHz, bool rev,
                       double whiteDb = 0.0, double silenceAfterSeconds = 0.0,
                       double silenceBeforeSeconds = 0.0) {
            return decode(silence(silenceBeforeSeconds) + mix(channels, upperHz, lowerHz, 14.0, whiteDb)
                              + silence(silenceAfterSeconds), rev);
        };
        // LOCKED at any reading that ends after `seconds` of input (readings 0 and 1 are the
        // neutral ones; reading k ends at (k - 1) * 0.5 s).
        auto lockedAfter = [](const DemoRun& r, double seconds) {
            for (size_t k = 2; k < r.series.size(); ++k) {
                if ((k - 1) * 0.5 > seconds && r.series[k].first) {
                    return true;
                }
            }
            return false;
        };
        auto copies = [&](NoiseMixer::Channel channel, double upperHz, double lowerHz, bool rev) {
            return run({channel}, upperHz, lowerHz, rev).copied;
        };
        // Today's decoder: REV on listens for space 170 Hz above the mark in audio.
        check(copies(NoiseMixer::Channel::Rtty, 2125, 2295, true),
              "demo RTTY normal, dial on its upper tone: copies with REV on");
        check(!copies(NoiseMixer::Channel::Rtty, 2125, 2295, false),
              "demo RTTY normal, dial on its upper tone: no copy with REV off");
        check(!copies(NoiseMixer::Channel::RttyReversed, 2125, 2295, true)
                  && !copies(NoiseMixer::Channel::RttyReversed, 2125, 2295, false),
              "demo RTTY reversed, dial on its upper tone: no copy in either REV state (#7408)");
        check(copies(NoiseMixer::Channel::RttyReversed, 1955, 2125, false),
              "demo RTTY reversed, dial retuned onto its mark (the lower tone): copies with REV off");

        // #7422: the pane's indicators on the same stations.
        const DemoRun clean = run({NoiseMixer::Channel::Rtty}, 2125, 2295, true);
        check(clean.copied && clean.minConf > 0.85f,
              "clean demo station: every character is green (confidence above 0.85)");
        check(clean.ticks > 0 && clean.lockedTicks == clean.ticks,
              "clean demo station: LOCKED at every reading after the first character");
        check(clean.maxLevel > 0.9f, "clean demo station: the level bar is nearly full");
        // The demo's own seeded white noise against the station at its default -16 dB.
        const DemoRun noisy = run({NoiseMixer::Channel::Rtty, NoiseMixer::Channel::White}, 2125, 2295, true, -10.0);
        check(noisy.copied && noisy.sumConf / noisy.chars < clean.sumConf / clean.chars - 0.05,
              "demo station in white noise at -10 dB: still copies, the average confidence falls");
        const DemoRun buried = run({NoiseMixer::Channel::Rtty, NoiseMixer::Channel::White}, 2125, 2295, true, -3.0);
        check(!buried.copied && !buried.everLocked && buried.maxLevel < 0.5f,
              "demo station in white noise at -3 dB: no copy, LOCKED never, the level bar under half");
        const DemoRun carrier = run({NoiseMixer::Channel::Birdie}, 2125, 2295, true);
        check(carrier.chars == 0 && !carrier.everLocked && carrier.maxLevel == 0.0f,
              "steady carrier on the mark tone: no text, LOCKED never, level 0");
        // REV off, the app's default and #7422's setting: a carrier switching on there can
        // frame one character; LOCKED and the level need two.
        const DemoRun carrierRevOff = run({NoiseMixer::Channel::Birdie}, 2125, 2295, false);
        check(carrierRevOff.chars == 0 && !carrierRevOff.everLocked && carrierRevOff.maxLevel == 0.0f,
              "steady carrier on the mark tone, REV off: no text, LOCKED never, level 0");
        const DemoRun carrierOnset = run({NoiseMixer::Channel::Birdie}, 2125, 2295, false, 0.0, 0.0, 1.0);
        check(carrierOnset.chars == 0 && !carrierOnset.everLocked && carrierOnset.maxLevel == 0.0f,
              "carrier switching on after 1 s on the mark tone, REV off: no text, LOCKED never, level 0");
        // After 1.5 s without a character the average starts again, so each switch-on is judged
        // on its own and an earlier copy is not carried over.
        const DemoRun twoOnsets = decode(silence(1.0) + mix({NoiseMixer::Channel::Birdie}, 2125, 2295, 3.0)
                                             + silence(3.0) + mix({NoiseMixer::Channel::Birdie}, 2125, 2295, 3.0), false);
        check(twoOnsets.chars == 0 && !twoOnsets.everLocked && twoOnsets.maxLevel == 0.0f,
              "carrier switched on twice with 3 s between, REV off: no text, LOCKED never, level 0");
        const DemoRun copyThenNoise = decode(mix({NoiseMixer::Channel::RttyReversed}, 1955, 2125, 14.0) + silence(3.0)
                                                 + mix({NoiseMixer::Channel::White}, 2125, 2295, 3.0), false);
        check(copyThenNoise.copied && copyThenNoise.everLocked && !lockedAfter(copyThenNoise, 17.0),
              "a copy with REV off, 3 s of silence, then noise only: LOCKED while it copies, never on the noise");
        const DemoRun stopped = run({NoiseMixer::Channel::Rtty}, 2125, 2295, true, 0.0, 3.0);
        check(stopped.copied && stopped.everLocked && !stopped.lastLocked && stopped.lastLevel == 0.0f,
              "demo station, then 3 s of silence: LOCKED while it copies, then UNLOCK with the level at 0");
        const DemoRun quiet = run({}, 2125, 2295, true);
        check(quiet.chars == 0 && !quiet.everLocked && quiet.maxLevel == 0.0f, "no signal: level 0, LOCKED never");
        check(allProcessed, "every demo run was decoded to its last sample, with no input reset on the way");
    }

    // #7422: the color scale follows the filters. At a 75 Hz shift each filter passes more of
    // the other tone, so a clean tone's share tops out lower than at 170 Hz; a clean A (no
    // lone bit between two opposite ones) still scores green.
    {
        RttyDecoder narrow;
        narrow.setShiftHz(75);
        QString text;
        float minConf = 1.0f;
        QObject::connect(&narrow, &RttyDecoder::textDecoded, &app, [&](const QString& t, float conf) {
            text += t;
            minConf = std::min(minConf, conf);
        });
        DecoderPcmAdapter adapter;
        adapter.selectRoute(DecoderPcmAdapter::RouteLane::NativeSlice, 9);
        PcmProducer producer;
        producer.start(PcmPurpose::Slice, 9, {24000, PcmLayout::Mono});
        narrow.start();
        feed(narrow, adapter, producer, signal(24000, PcmLayout::Mono, {31, 3, 3, 3, 3, 3, 3}, false, 2125, 2050), 1);
        pumpUntil([&] { return text.size() >= 6; });
        check(text == QStringLiteral("AAAAAA") && minConf > 0.85f,
              "75 Hz shift: a clean signal's letters without a lone bit are green");
        narrow.stop();
    }

    // #7422: how the average behind LOCKED and the level moves. Generated 'A's (no lone bit),
    // each with the other tone mixed in: at 0.5 its amplitude a character scores about 0.78,
    // between the unlock (0.70) and lock (0.83) scores; at 0.85 about 0.53.
    {
        auto lockSeries = [&](const QVector<float>& input) {
            QVector<float> pcm = input;
            pcm.resize((pcm.size() + 11999) / 12000 * 12000, 0.0f);
            RttyDecoder decoder;
            int readings = 0;
            std::vector<bool> locked;
            QObject::connect(&decoder, &RttyDecoder::statsUpdated, &app, [&](float, float, bool l) {
                ++readings;
                locked.push_back(l);
            });
            DecoderPcmAdapter adapter;
            adapter.selectRoute(DecoderPcmAdapter::RouteLane::NativeSlice, 10);
            PcmProducer producer;
            producer.start(PcmPurpose::Slice, 10, {24000, PcmLayout::Mono});
            decoder.start();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
            // Paced as the demo runs above: at most 2.5 s ahead of the decoder's readings.
            const int blocks = static_cast<int>(pcm.size() / 12000);
            bool paced = true;
            for (int block = 0; block < blocks && paced; ++block) {
                paced = pumpUntil([&] { return block - (readings - 2) <= 4; });
                feed(decoder, adapter, producer, pcm.mid(block * 12000, 12000), 1);
            }
            paced = paced && pumpUntil([&] { return readings >= 2 + blocks; });
            decoder.stop();
            check(paced && readings == 2 + blocks, "average checks: every sample decoded, no input reset");
            return locked;
        };
        QVector<std::pair<int, double>> codes(6, {3, 0.0});
        codes += QVector<std::pair<int, double>>(30, {3, 0.5});
        const std::vector<bool> slipping = lockSeries(blendedSignal(codes));
        check(std::find(slipping.begin(), slipping.end(), true) != slipping.end() && !slipping.empty()
                  && slipping.back(),
              "LOCKED holds while the average falls between the unlock and lock scores");
        codes = {{3, 0.0}};
        codes += QVector<std::pair<int, double>>(8, {3, 0.85});
        const std::vector<bool> oneClean = lockSeries(blendedSignal(codes));
        check(std::find(oneClean.begin(), oneClean.end(), true) == oneClean.end(),
              "one clean character, then poor ones: the fresh average is a plain mean and never locks");
    }

    RttyDecoder decoder;
    QString decoded;
    bool haveStats = false;
    bool locked = false;
    int resets = 0;
    QObject::connect(&decoder, &RttyDecoder::statsUpdated, &app,
                     [&](float, float, bool value) { haveStats = true; locked = value; });
    QObject::connect(&decoder, &RttyDecoder::statsReset, &app, [&] { ++resets; });
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
    const int resetsBefore = resets;
    decoder.resetInput();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    check(resets == resetsBefore + 1 && !locked, "source reset clears visible detector lock statistics");
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
    // detector state on overflow publishes one (statsReset), trimming the backlog
    // publishes none. Text alone does not separate them -- a decoder that resets on
    // every overflow still emits characters between resets.
    {
        RttyDecoder legacy;
        int neutralResets = 0;
        QObject::connect(&legacy, &RttyDecoder::statsReset, &app, [&] { ++neutralResets; });
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
                         [&](float, float, bool) { ++notifications; });
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
    // Tones at or below 0 Hz hold the decoder idle; valid tones resume it with
    // no restart. CONSTRUCTED signals: Mark 1000 / Shift 1170 puts space at
    // -170 Hz, and the 1000/170 Hz RY below is what a filter built at the
    // mirror image (+170 Hz) would decode.
    {
        RttyDecoder tones;
        QString text;
        bool finiteStats = true;
        bool anyLock = false;
        QObject::connect(&tones, &RttyDecoder::textDecoded, &app,
                         [&](const QString& t, float) { text += t; });
        QObject::connect(&tones, &RttyDecoder::statsUpdated, &app,
                         [&](float level, float snr, bool lockedNow) {
                             finiteStats = finiteStats && std::isfinite(level)
                                 && std::isfinite(snr);
                             anyLock = anyLock || lockedNow;
                         });
        DecoderPcmAdapter toneAdapter;
        toneAdapter.selectRoute(DecoderPcmAdapter::RouteLane::NativeSlice, 5);
        PcmProducer toneProducer;
        toneProducer.start(PcmPurpose::Slice, 5, {24000, PcmLayout::Mono});

        tones.setMarkFreqHz(0);
        tones.setShiftHz(170);
        tones.start();
        feed(tones, toneAdapter, toneProducer, signal(24000, PcmLayout::Mono, {31, 10, 21}), 1);
        pumpUntil([&] { return !text.isEmpty(); });
        check(text.isEmpty(), "Mark 0 holds the decoder idle");
        check(finiteStats && !anyLock, "Mark 0 reports finite, unlocked statistics");

        tones.setMarkFreqHz(1000);
        tones.setShiftHz(1170);
        feed(tones, toneAdapter, toneProducer,
             signal(24000, PcmLayout::Mono, {31, 10, 21}, false, 1000, 170), 1);
        pumpUntil([&] { return !text.isEmpty(); });
        check(text.isEmpty(), "a space tone below 0 Hz holds the decoder idle, not at its mirror");

        tones.setMarkFreqHz(2125);
        tones.setShiftHz(170);
        feed(tones, toneAdapter, toneProducer, signal(24000, PcmLayout::Mono, {31, 10, 21}), 1);
        pumpUntil([&] { return text.size() >= 2; });
        check(text == QStringLiteral("RY"), "valid tones resume decoding without a restart");

        // Stepped down from valid tones, the running decoder must not keep
        // decoding on the 2125/1955 Hz filters it built before.
        text.clear();
        tones.setMarkFreqHz(150);
        feed(tones, toneAdapter, toneProducer, signal(24000, PcmLayout::Mono, {31, 10, 21}), 1);
        pumpUntil([&] { return !text.isEmpty(); });
        check(text.isEmpty(), "Mark 150 / Shift 170 holds a running decoder idle, not on its old filters");
        tones.stop();
    }

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
