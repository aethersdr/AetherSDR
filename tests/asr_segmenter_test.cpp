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

int totalSamples(const std::vector<AsrSegmenter::ClosedSegment>& segs)
{
    int n = 0;
    for (const auto& s : segs) {
        n += static_cast<int>(s.samples.size());
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
            const int len = static_cast<int>(out[0].samples.size());
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

    // Segment overlap (RFC #4821): a cap-forced close carries trailing audio into
    // the next segment and flags it as a continuation; consecutive segments then
    // share that audio, so the emitted total exceeds a no-overlap run of the same
    // input. Overlap fires only on the cap path, never on a hangover close.
    {
        std::vector<float> audio;
        appendTone(audio, 1600); // continuous speech — forces repeated cap closes

        AsrSegmenter plain;
        plain.setMaxSegmentMs(500);
        auto plainOut = plain.feed(audio.data(), static_cast<int>(audio.size()));

        AsrSegmenter ov;
        ov.setMaxSegmentMs(500);
        ov.setOverlapMs(200);
        auto ovOut = ov.feed(audio.data(), static_cast<int>(audio.size()));

        expect(ovOut.size() >= 2, "overlap: long over still splits into segments");
        expect(!ovOut.empty() && !ovOut.front().continuesPrevious,
               "overlap: first segment is not a continuation");
        bool laterAllContinue = ovOut.size() >= 2;
        bool laterWindowRecorded = ovOut.size() >= 2;
        for (std::size_t i = 1; i < ovOut.size(); ++i) {
            if (!ovOut[i].continuesPrevious) {
                laterAllContinue = false;
            }
            // 200 ms carried (< half of the 500 ms budget, so unclamped here).
            if (ovOut[i].overlapMs != 200) {
                laterWindowRecorded = false;
            }
        }
        expect(laterAllContinue, "overlap: later segments are flagged as continuations");
        expect(laterWindowRecorded, "overlap: continuations record the carried window (200 ms)");
        expect(!ovOut.empty() && ovOut.front().overlapMs == 0,
               "overlap: a non-continuation segment records no carried window");
        // No segment in the plain run is ever flagged a continuation.
        bool plainNoneContinue = true;
        for (const auto& s : plainOut) {
            if (s.continuesPrevious) {
                plainNoneContinue = false;
            }
        }
        expect(plainNoneContinue, "no-overlap: no segment is flagged as a continuation");
        expect(totalSamples(ovOut) > totalSamples(plainOut),
               "overlap: carried audio duplicates across the cut (more total samples)");
    }

    // Overlap must NOT trigger on a hangover-based close (speech already stopped).
    {
        AsrSegmenter seg;
        seg.setOverlapMs(200); // default 20 s cap won't fire on this short over
        std::vector<float> audio;
        appendSilence(audio, 100);
        appendTone(audio, 500);
        appendSilence(audio, 400); // closes by hangover
        auto out = seg.feed(audio.data(), static_cast<int>(audio.size()));
        expect(out.size() == 1 && !out[0].continuesPrevious,
               "overlap: a hangover close is not a continuation");
    }

    // Overlap must NOT carry when the cap fires DURING the hangover silence (the
    // trailing window is silence — no split word to recover). Here the cap (400
    // ms) fires ~150 ms into the hangover, so the close must not spawn a spurious
    // silence-only continuation segment.
    {
        AsrSegmenter seg;
        seg.setMaxSegmentMs(400);
        seg.setOverlapMs(200);
        std::vector<float> audio;
        appendTone(audio, 250);    // speech, then...
        appendSilence(audio, 400); // ...silence; cap fires mid-hangover at 400 ms total
        auto out = seg.feed(audio.data(), static_cast<int>(audio.size()));
        auto tail = seg.flush();
        out.insert(out.end(), tail.begin(), tail.end());
        expect(out.size() == 1, "overlap: cap during hangover does not spawn a silence continuation");
        bool anyContinue = false;
        for (const auto& s : out) {
            if (s.continuesPrevious) {
                anyContinue = true;
            }
        }
        expect(!anyContinue, "overlap: a silence-tail cap close is not flagged a continuation");
    }

    // Overlap ≥ maxSegmentMs must stay bounded: the carry is clamped to half the
    // segment budget, so each reseeded segment is ≥ half fresh audio and the
    // decode count stays near ~2× realtime instead of exploding (a runaway
    // "decode every frame" backlog). 2 s of continuous speech at a 300 ms cap
    // yields ~13 segments here; without the clamp it would be ~170.
    {
        AsrSegmenter seg;
        seg.setMaxSegmentMs(300);
        seg.setOverlapMs(1000); // >> the 300 ms cap
        std::vector<float> audio;
        appendTone(audio, 2000); // continuous speech
        auto out = seg.feed(audio.data(), static_cast<int>(audio.size()));
        // ~2× realtime bound: 2000 ms / (300/2 ms fresh per segment) ≈ 13, well
        // under 30; the pre-clamp bug produced ~170.
        expect(out.size() < 30, "overlap: overlap >= cap stays bounded (carry clamped to half budget)");
        expect(out.size() >= 2, "overlap: overlap >= cap still splits into segments");
    }

    std::printf(g_failures == 0 ? "\nASR segmenter: ALL PASS\n"
                                : "\nASR segmenter: %d FAILURE(S)\n",
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
