// Offline unit test for the ASR VAD segmenter (RFC #4333, Phase 3). Pure C++,
// no Qt, no model: feeds synthetic 16 kHz audio (silence / tones) and asserts
// utterance boundaries, the minimum-speech drop, multi-utterance splitting, and
// flush().

// MSVC's <math.h> only defines M_PI when _USE_MATH_DEFINES is set before the
// first math header, which any of the includes below may transitively pull in.
// (POSIX/Linux headers define it unconditionally.) Must be before ALL #includes.
#define _USE_MATH_DEFINES

#include "asr/AsrSegmenter.h"

#include <cmath>
#include <cstdio>
#include <vector>

using AetherSDR::AsrSegmenter;

namespace {

int g_failures = 0;

void expect(bool condition, const char* description)
{
    std::printf("%s %s\n", condition ? "[ OK ]" : "[FAIL]", description);
    if (!condition) {
        ++g_failures;
    }
}

constexpr int kRate = 16000;

void appendSilence(std::vector<float>& buf, int ms)
{
    const int n = ms * kRate / 1000;
    buf.insert(buf.end(), static_cast<size_t>(n), 0.0f);
}

void appendTone(std::vector<float>& buf, int ms, float amp = 0.3f, float freq = 440.0f)
{
    const int n = ms * kRate / 1000;
    for (int i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / kRate;
        buf.push_back(amp * static_cast<float>(std::sin(2.0 * M_PI * freq * t)));
    }
}

int totalSamples(const std::vector<std::vector<float>>& segs)
{
    int n = 0;
    for (const auto& s : segs) {
        n += static_cast<int>(s.size());
    }
    return n;
}

} // namespace

int main()
{
    // Pure silence -> nothing.
    {
        AsrSegmenter seg;
        std::vector<float> audio;
        appendSilence(audio, 1000);
        auto out = seg.feed(audio.data(), static_cast<int>(audio.size()));
        expect(out.empty(), "pure silence produces no segments");
        expect(seg.flush().empty(), "flush after silence produces nothing");
    }

    // silence -> tone (500 ms) -> silence (400 ms): one utterance closed by
    // hangover. Length ~ tone + hangover(300 ms), within a frame of slop.
    {
        AsrSegmenter seg;
        std::vector<float> audio;
        appendSilence(audio, 200);
        appendTone(audio, 500);
        appendSilence(audio, 400);
        auto out = seg.feed(audio.data(), static_cast<int>(audio.size()));
        expect(out.size() == 1, "one tone burst -> one segment");
        if (out.size() == 1) {
            const int len = static_cast<int>(out[0].size());
            const int lo = 700 * kRate / 1000; // >= tone + most of hangover
            const int hi = 900 * kRate / 1000; // <= tone + hangover + slop
            expect(len >= lo && len <= hi, "segment length is tone + hangover");
        }
        expect(!seg.inSpeech(), "segmenter returns to idle after close");
    }

    // Two tones separated by a long gap -> two utterances.
    {
        AsrSegmenter seg;
        std::vector<float> audio;
        appendSilence(audio, 100);
        appendTone(audio, 400);
        appendSilence(audio, 600);
        appendTone(audio, 400);
        appendSilence(audio, 400);
        auto out = seg.feed(audio.data(), static_cast<int>(audio.size()));
        expect(out.size() == 2, "two separated tones -> two segments");
    }

    // A blip shorter than minSpeechMs (200 ms) is dropped.
    {
        AsrSegmenter seg;
        std::vector<float> audio;
        appendSilence(audio, 100);
        appendTone(audio, 80); // below minSpeech
        appendSilence(audio, 400);
        auto out = seg.feed(audio.data(), static_cast<int>(audio.size()));
        expect(out.empty(), "sub-minimum blip is discarded as noise");
    }

    // flush() closes an in-progress utterance with no trailing silence.
    {
        AsrSegmenter seg;
        std::vector<float> audio;
        appendTone(audio, 500);
        auto out = seg.feed(audio.data(), static_cast<int>(audio.size()));
        expect(out.empty(), "open utterance not yet closed without hangover");
        auto flushed = seg.flush();
        expect(flushed.size() == 1, "flush closes the open utterance");
        expect(totalSamples(flushed) >= 400 * kRate / 1000, "flushed segment holds the speech");
    }

    // Runtime setter: decode-buffer cap force-closes a long, gap-less over.
    {
        AsrSegmenter seg;
        seg.setMaxSegmentMs(500); // force-decode every ~0.5 s
        std::vector<float> audio;
        appendTone(audio, 1600); // continuous speech, no silence
        auto out = seg.feed(audio.data(), static_cast<int>(audio.size()));
        expect(out.size() >= 2, "setMaxSegmentMs force-closes a long over into segments");
    }

    // Runtime setter: raising the RMS threshold makes a moderate tone read as
    // silence (lower VAD sensitivity).
    {
        AsrSegmenter seg;
        seg.setSpeechRms(0.5f); // 0.3-amp tone has RMS ~0.21 < 0.5
        std::vector<float> audio;
        appendSilence(audio, 100);
        appendTone(audio, 500);
        appendSilence(audio, 400);
        auto out = seg.feed(audio.data(), static_cast<int>(audio.size()));
        expect(out.empty(), "raising speechRms makes a moderate tone read as silence");
    }

    // Runtime setter: a shorter hangover closes the utterance sooner.
    {
        AsrSegmenter seg;
        seg.setHangoverMs(100);
        std::vector<float> audio;
        appendTone(audio, 400);
        appendSilence(audio, 150); // 150 ms > 100 ms hangover -> closes
        auto out = seg.feed(audio.data(), static_cast<int>(audio.size()));
        expect(out.size() == 1, "shorter hangover closes the utterance sooner");
    }

    std::printf(g_failures == 0 ? "\nASR segmenter: ALL PASS\n"
                                : "\nASR segmenter: %d FAILURE(S)\n",
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
