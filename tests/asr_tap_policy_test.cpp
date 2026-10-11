// The two decisions the Copy Assist audio tap makes about incoming RX blocks:
// which receiver to follow, and how to collapse a post-DSP stereo block to the
// mono float the ASR engine consumes. (#4486)
//
// These are unit-tested here because they are pure decisions with no engine
// in them. The emit they consume IS driven end-to-end against a real
// AudioEngine in asr_pre_dsp_emit_test, on the socket-free friend seam
// audio_engine_rates_test uses; what remains uncovered is the tap's own
// connect(), a single line visible in review.

#include "gui/AsrTapPolicy.h"

#include <QCoreApplication>

#include <cmath>
#include <cstdio>
#include <limits>

namespace AetherSDR {

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_failures; }
}

static QByteArray stereoBlock(std::initializer_list<std::pair<float, float>> frames)
{
    QByteArray out(static_cast<int>(frames.size()) * 2
                       * static_cast<int>(sizeof(float)),
                   Qt::Uninitialized);
    auto* f = reinterpret_cast<float*>(out.data());
    int i = 0;
    for (const auto& [l, r] : frames) {
        f[i++] = l;
        f[i++] = r;
    }
    return out;
}

// ── Source lock ───────────────────────────────────────────────────────────

// receivePresentationPostDspAudioReady fires once per RX source. Taking all of
// them hands a recogniser two receivers interleaved into one stream, which is
// the state Copy Assist shipped in. One source, consistently, is the fix.
static void testLocksOntoOneSource()
{
    AsrTapPolicy p;
    check(!p.hasLock(), "a fresh policy follows nothing yet");

    check(p.accepts(QStringLiteral("flex"), QString(), 0),
          "the first block claims the lock");
    check(p.hasLock() && p.lockedSource() == QLatin1String("flex"),
          "the lock records which source claimed it");

    // A Kiwi running alongside the Flex must not be mixed in.
    check(!p.accepts(QStringLiteral("kiwi"), QString(), 5),
          "a second, different source is refused while the first is live");
    check(!p.accepts(QStringLiteral("kiwi"), QStringLiteral("ant-2"), 10),
          "an external Kiwi antenna is refused for the same reason");

    check(p.accepts(QStringLiteral("flex"), QString(), 15),
          "the locked source keeps being accepted");
}

// A Kiwi-only station has no Flex blocks at all. Hard-coding "flex" would have
// silently disabled Copy Assist for them, which is why the first block claims
// the lock rather than the policy naming a preferred source.
static void testAnySourceMayClaimTheLock()
{
    AsrTapPolicy p;
    check(p.accepts(QStringLiteral("kiwi"), QStringLiteral("ant-1"), 0),
          "a Kiwi-only station locks its Kiwi");
    check(p.lockedSource() == QLatin1String("kiwi")
              && p.lockedSourceId() == QLatin1String("ant-1"),
          "the sourceId is part of the identity, not just the family");

    // Same family, different antenna, is a DIFFERENT receiver.
    check(!p.accepts(QStringLiteral("kiwi"), QStringLiteral("ant-2"), 5),
          "a sibling Kiwi antenna does not share the lock");
}

// An operator who switches receivers mid-session must not be left transcribing
// silence forever. Post-DSP audio flows continuously while a receiver is up —
// a quiet band still produces blocks — so a long gap means the source went
// away, and only then does the lock move.
static void testSilentSourceReleasesTheLock()
{
    AsrTapPolicy p;
    check(p.accepts(QStringLiteral("flex"), QString(), 1000), "flex claims the lock");

    // Just under the window: still the Flex's lock, even though it has been
    // quiet for nearly two seconds.
    check(!p.accepts(QStringLiteral("kiwi"), QString(),
                     1000 + AsrTapPolicy::kSourceReleaseMs - 1),
          "the lock is not stolen before the release window elapses");

    check(p.accepts(QStringLiteral("kiwi"), QString(),
                    1000 + AsrTapPolicy::kSourceReleaseMs),
          "a source silent for the whole window releases its lock");
    check(p.lockedSource() == QLatin1String("kiwi"),
          "the new source takes the lock over");

    // And the window is measured from the last ACCEPTED block, so a stream of
    // refused blocks from the other source cannot keep resetting it.
    check(!p.accepts(QStringLiteral("flex"), QString(),
                     1000 + AsrTapPolicy::kSourceReleaseMs + 1),
          "the previous owner is now the one being refused");
}

// Disabling Copy Assist and re-enabling it must not make the operator wait out
// a release window against a receiver that is no longer there.
static void testResetDropsTheLock()
{
    AsrTapPolicy p;
    p.accepts(QStringLiteral("flex"), QString(), 0);
    p.reset();
    check(!p.hasLock(), "reset drops the claim");
    check(p.accepts(QStringLiteral("kiwi"), QString(), 1),
          "the next session locks whichever receiver is live then, immediately");
}

// ── Stereo → mono ─────────────────────────────────────────────────────────

static void testMonoCollapse()
{
    // L/R averaged, matching what emitRxPostChainScopeFromFloat32Stereo handed
    // over before the tap moved. Asymmetric channels prove it is an average and
    // not a channel pick.
    const QByteArray in = stereoBlock({{1.0f, 0.0f}, {-0.5f, -0.5f}, {0.25f, 0.75f}});
    const QVector<float> mono = AsrTapPolicy::toMono(in, 2);

    check(mono.size() == 3, "one mono sample per stereo frame");
    if (mono.size() != 3) return;
    check(std::fabs(mono[0] - 0.5f) < 1e-6f, "L/R are averaged, not taken from one channel");
    check(std::fabs(mono[1] + 0.5f) < 1e-6f, "a matched pair passes through unchanged");
    check(std::fabs(mono[2] - 0.5f) < 1e-6f, "averaging is correct for unequal channels");
}

// AsrEngine does not sanitise its input, and one NaN reaching the resampler
// poisons every sample after it. The engine-side tap clamped; dropping that on
// the way out would have traded a known bug for a worse one.
static void testMonoCollapseGuardsAgainstNonFiniteAndClipping()
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    const QByteArray in = stereoBlock({{nan, 0.0f}, {inf, inf}, {5.0f, 5.0f}, {-9.0f, -9.0f}});
    const QVector<float> mono = AsrTapPolicy::toMono(in, 2);

    check(mono.size() == 4, "non-finite input still yields one sample per frame");
    if (mono.size() != 4) return;
    check(mono[0] == 0.0f, "a NaN channel becomes silence, not a poisoned sample");
    check(mono[1] == 0.0f, "an infinite channel becomes silence");
    check(std::fabs(mono[2] - 1.0f) < 1e-6f, "over-range audio clamps to +1.0");
    check(std::fabs(mono[3] + 1.0f) < 1e-6f, "under-range audio clamps to -1.0");
}

static void testMonoCollapseEdgeCases()
{
    check(AsrTapPolicy::toMono(QByteArray(), 2).isEmpty(),
          "an empty block produces no samples");
    // Shorter than one float: must not read past the end.
    check(AsrTapPolicy::toMono(QByteArray(3, '\0'), 2).isEmpty(),
          "a sub-sample block produces no samples");
}

// #4489: the caller states the channel count instead of toMono() inferring it
// from the byte count's parity. A genuinely mono block with an even sample
// count used to be misread as stereo and averaged pairwise — silent
// corruption, not a crash, which is why it needed a caller-supplied count
// rather than a better heuristic.
static void testMonoPassthroughForMonoChannelCount()
{
    QByteArray in(4 * static_cast<int>(sizeof(float)), Qt::Uninitialized);
    auto* f = reinterpret_cast<float*>(in.data());
    f[0] = 1.0f; f[1] = -1.0f; f[2] = 0.25f; f[3] = -0.25f;

    const QVector<float> mono = AsrTapPolicy::toMono(in, 1);
    check(mono.size() == 4, "mono in, mono out — one sample per float, none paired off");
    if (mono.size() != 4) return;
    check(std::fabs(mono[0] - 1.0f) < 1e-6f && std::fabs(mono[1] + 1.0f) < 1e-6f,
          "mono samples pass through individually rather than being averaged in pairs");
}

// A block that is not a whole number of frames for the stated channel count is
// malformed. The old parity heuristic would have silently reinterpreted it
// (an odd stereo block read as mono); the fix rejects it instead.
static void testMalformedLengthIsRejectedNotReinterpreted()
{
    // 3 floats claimed as stereo: not a whole number of stereo frames.
    QByteArray in(3 * static_cast<int>(sizeof(float)), Qt::Uninitialized);
    auto* f = reinterpret_cast<float*>(in.data());
    f[0] = 1.0f; f[1] = 1.0f; f[2] = 1.0f;
    check(AsrTapPolicy::toMono(in, 2).isEmpty(),
          "an odd-length block claimed as stereo is rejected, not read as mono");

    check(AsrTapPolicy::toMono(in, 0).isEmpty(), "a zero channel count is rejected");
    check(AsrTapPolicy::toMono(in, 3).isEmpty(),
          "an unsupported channel count is rejected rather than guessed at");

    // A byte count that isn't a whole number of floats truncates on the way
    // to totalFloats, so it can pass the frame check on a shorter, silently
    // wrong count. 9 bytes claimed as stereo would truncate to 2 floats (one
    // frame, accepted) with the trailing byte dropped without a word — this
    // must be caught before the frame check ever sees it.
    QByteArray partial(9, '\0');
    check(AsrTapPolicy::toMono(partial, 2).isEmpty(),
          "a byte count that isn't a whole number of floats is rejected, not truncated");
}

// The property the whole change exists to protect: EVERY sample handed to the
// policy comes back out. The old tap dropped whole blocks; nothing here may.
static void testNoSamplesAreDropped()
{
    AsrTapPolicy p;
    // 128 stereo frames is a Flex LAN audio packet — the 5.33 ms block that the
    // 8 ms scope throttle used to discard when NR2 drained several per tick.
    constexpr int kFramesPerPacket = 128;
    QByteArray packet(kFramesPerPacket * 2 * static_cast<int>(sizeof(float)),
                      Qt::Uninitialized);
    auto* f = reinterpret_cast<float*>(packet.data());
    for (int i = 0; i < kFramesPerPacket * 2; ++i) {
        f[i] = 0.1f;
    }

    // Ten packets delivered in the same millisecond, exactly as the NR2 drain
    // loop delivers them.
    int delivered = 0;
    for (int n = 0; n < 10; ++n) {
        if (p.accepts(QStringLiteral("flex"), QString(), 0)) {
            delivered += AsrTapPolicy::toMono(packet, 2).size();
        }
    }
    check(delivered == 10 * kFramesPerPacket,
          "ten packets arriving in the same millisecond all reach the engine");
}

// ── Tap point setting ─────────────────────────────────────────────────────

// The names are what operator profiles store under CopyAssist.AsrTapPoint, so
// they are pinned literally: renaming one would silently move every operator
// who chose the unprocessed tap point back to post-DSP on upgrade.
static void testTapPointSettingNamesArePinned()
{
    check(asrTapPointToSetting(AsrTapPoint::PostDsp) == QLatin1String("PostDsp"),
          "PostDsp is stored as \"PostDsp\"");
    check(asrTapPointToSetting(AsrTapPoint::PreDsp) == QLatin1String("PreDsp"),
          "PreDsp is stored as \"PreDsp\"");
    check(asrTapPointFromSetting(asrTapPointToSetting(AsrTapPoint::PreDsp))
              == AsrTapPoint::PreDsp,
          "PreDsp round-trips through its stored name");
    check(asrTapPointFromSetting(asrTapPointToSetting(AsrTapPoint::PostDsp))
              == AsrTapPoint::PostDsp,
          "PostDsp round-trips through its stored name");
}

// Every build before this setting transcribed post-DSP, so a missing, empty or
// unrecognised value must mean exactly that — never the new behaviour.
static void testUnknownTapPointFallsBackToPostDsp()
{
    check(asrTapPointFromSetting(QString()) == AsrTapPoint::PostDsp,
          "an absent setting reads as PostDsp");
    check(asrTapPointFromSetting(QStringLiteral("predsp")) == AsrTapPoint::PostDsp,
          "a mis-cased value is not guessed at");
    check(asrTapPointFromSetting(QStringLiteral("PostNr")) == AsrTapPoint::PostDsp,
          "a point this build does not know reads as PostDsp");
    check(asrTapPointFromSetting(QStringLiteral("True")) == AsrTapPoint::PostDsp,
          "a bool-style value is not taken as PreDsp");
}


// Sensitivity must mean the same thing on both sides of the tap-point toggle
// (RFC #4861). The post-DSP feed carries the operator's boost and trim; the
// pre-DSP feed does not, so the THRESHOLD moves instead of the audio.
static void testSpeechRmsTracksTheTapPointGain()
{
    const float base = 0.0193f;   // Sensitivity 63

    check(asrSpeechRmsForTapPoint(base, AsrTapPoint::PostDsp, false, 0.0f) == base,
          "PostDsp is the level the operator tuned against, unscaled");
    check(asrSpeechRmsForTapPoint(base, AsrTapPoint::PostDsp, true, 12.0f) == base,
          "PostDsp ignores boost and trim — they are already in that feed");

    check(std::fabs(asrSpeechRmsForTapPoint(base, AsrTapPoint::PreDsp, false, 0.0f) - base)
              < 1e-6f,
          "PreDsp with no boost and no trim needs no adjustment");

    // Boost on: the post-DSP feed is ~6 dB hotter, so the gate must come down
    // by the same factor or speech falls under it after the toggle.
    check(std::fabs(asrSpeechRmsForTapPoint(base, AsrTapPoint::PreDsp, true, 0.0f)
                    - base / 2.0f) < 1e-6f,
          "PreDsp with boost on halves the threshold");

    // +12 dB trim is the strip's maximum: a factor of ~3.98.
    const float trimmed = asrSpeechRmsForTapPoint(base, AsrTapPoint::PreDsp, false, 12.0f);
    check(std::fabs(trimmed - base / 3.98107f) < 1e-5f,
          "PreDsp divides by the trim's linear gain");

    // Both together, the worst case the review measured at +18 dB.
    const float both = asrSpeechRmsForTapPoint(base, AsrTapPoint::PreDsp, true, 12.0f);
    check(std::fabs(both - base / (2.0f * 3.98107f)) < 1e-5f,
          "boost and trim compound");
    check(both < base, "the worst case lowers the threshold, never raises it");

    // Negative trim makes the post-DSP feed quieter than pre-DSP, so the
    // threshold goes UP. The sign must not be assumed.
    check(asrSpeechRmsForTapPoint(base, AsrTapPoint::PreDsp, false, -12.0f) > base,
          "a negative trim raises the threshold");

    // A corrupt trim must not produce a non-finite or zero threshold, which
    // would make the gate admit everything or nothing.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    check(asrSpeechRmsForTapPoint(base, AsrTapPoint::PreDsp, false, nan) == base,
          "a non-finite trim leaves the threshold alone");
    check(asrSpeechRmsForTapPoint(base, AsrTapPoint::PreDsp, false, -1000.0f) > 0.0f,
          "an absurd trim still yields a positive threshold");

    // ---- The rest of the chain's static gain ------------------------------
    // Boost and trim are not the only gain between the two taps: the EQ's
    // master gain, the compressor's makeup and the tube's output gain are all
    // signal-independent, and AudioEngine::rxStaticChainMakeupDb() sums them.
    // Omitting them left a threshold wrong by exactly that amount for any
    // operator running those stages.
    check(std::fabs(asrSpeechRmsForTapPoint(base, AsrTapPoint::PreDsp, false, 0.0f, 0.0f)
                    - base) < 1e-6f,
          "a chain at unity gain leaves the threshold where the operator put it");

    const float chained =
        asrSpeechRmsForTapPoint(base, AsrTapPoint::PreDsp, false, 0.0f, 6.0206f);
    check(std::fabs(chained - base / 2.0f) < 1e-4f,
          "+6 dB of chain makeup halves the threshold");

    // The three sources of gain are one product, not three cases.
    const float all =
        asrSpeechRmsForTapPoint(base, AsrTapPoint::PreDsp, true, 12.0f, 6.0206f);
    check(std::fabs(all - base / (2.0f * 3.98107f * 2.0f)) < 1e-5f,
          "boost, trim and chain makeup compound into one divisor");

    // The chain can attenuate too (a negative makeup or a master gain below
    // unity), which raises the threshold.
    check(asrSpeechRmsForTapPoint(base, AsrTapPoint::PreDsp, false, 0.0f, -6.0206f) > base,
          "negative chain makeup raises the threshold");

    // PostDsp still ignores every one of them: that path is the level the
    // operator tuned against, by definition.
    check(asrSpeechRmsForTapPoint(base, AsrTapPoint::PostDsp, true, 12.0f, 6.0206f) == base,
          "PostDsp ignores chain makeup as it ignores boost and trim");

    // An EQ master gain of zero is -inf dB: the post-DSP feed is silent, and no
    // threshold derived from it means anything. Fall back to the tuned value
    // rather than emitting zero (gate admits everything) or inf (admits
    // nothing).
    // Boost is on here on purpose: without it this check passes even for an
    // implementation that ignores chain makeup entirely, because the fallback
    // and "no chain gain at all" give the same answer.
    const float negInf = -std::numeric_limits<float>::infinity();
    check(asrSpeechRmsForTapPoint(base, AsrTapPoint::PreDsp, true, 0.0f, negInf) == base,
          "a silenced chain leaves the threshold alone, boost included");
}

}  // namespace AetherSDR

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    AetherSDR::testLocksOntoOneSource();
    AetherSDR::testAnySourceMayClaimTheLock();
    AetherSDR::testSilentSourceReleasesTheLock();
    AetherSDR::testResetDropsTheLock();
    AetherSDR::testMonoCollapse();
    AetherSDR::testMonoCollapseGuardsAgainstNonFiniteAndClipping();
    AetherSDR::testMonoCollapseEdgeCases();
    AetherSDR::testMonoPassthroughForMonoChannelCount();
    AetherSDR::testMalformedLengthIsRejectedNotReinterpreted();
    AetherSDR::testNoSamplesAreDropped();
    AetherSDR::testTapPointSettingNamesArePinned();
    AetherSDR::testUnknownTapPointFallsBackToPostDsp();
    AetherSDR::testSpeechRmsTracksTheTapPointGain();

    if (AetherSDR::g_failures == 0)
        std::fprintf(stderr, "asr_tap_policy_test: all checks passed\n");
    return AetherSDR::g_failures == 0 ? 0 : 1;
}
