#include "core/PacketLossConcealment.h"

#include <QByteArray>
#include <cmath>
#include <cstdio>

using namespace MasterSDR;

namespace {

constexpr int kChannels = 2;

// Build a stereo PCM QByteArray of `frames` frames where every sample
// equals `value`.  Used as a constant-amplitude input so fade-window
// behaviour shows up cleanly in the output.
QByteArray makePcm(int frames, float value)
{
    QByteArray out(frames * kChannels * static_cast<int>(sizeof(float)),
                   Qt::Uninitialized);
    auto* dst = reinterpret_cast<float*>(out.data());
    for (int i = 0; i < frames * kChannels; ++i) dst[i] = value;
    return out;
}

const float* asFloat(const QByteArray& pcm) {
    return reinterpret_cast<const float*>(pcm.constData());
}

int g_failures = 0;

void fail(const char* file, int line, const char* what)
{
    std::fprintf(stderr, "FAIL %s:%d  %s\n", file, line, what);
    ++g_failures;
}

#define EXPECT(cond, msg) do { \
    if (!(cond)) fail(__FILE__, __LINE__, msg); \
} while (0)

#define EXPECT_NEAR(a, b, tol, msg) do { \
    if (std::fabs((a) - (b)) > (tol)) { \
        char buf[256]; \
        std::snprintf(buf, sizeof(buf), "%s (got %f, want %f, tol %f)", \
                      (msg), (double)(a), (double)(b), (double)(tol)); \
        fail(__FILE__, __LINE__, buf); \
    } \
} while (0)

// 1. No-loss path: pendingMissed=0 → output == input, tail/frameCount updated.
void test_no_loss_passes_through()
{
    AudioPlcState plc;
    plc.lastFrames = 240;
    plc.tailL = 0.5f;
    plc.tailR = 0.5f;
    plc.pendingMissed = 0;

    const QByteArray in = makePcm(240, 0.25f);
    const QByteArray out = applyConcealmentFade(in, plc, true);

    EXPECT(out == in, "no-loss output must equal input verbatim");
    EXPECT(plc.lastFrames == 240, "lastFrames updated to new frame count");
    EXPECT(plc.pendingMissed == 0, "pendingMissed stays 0");
    EXPECT_NEAR(plc.tailL, 0.25f, 1e-6f, "tailL updated to input's last sample");
    EXPECT_NEAR(plc.tailR, 0.25f, 1e-6f, "tailR updated to input's last sample");
}

// 2. First-packet path: lastFrames=0 → no concealment even if missed > 0,
//    because we have no prior tail to fade from.
void test_first_packet_skips_concealment()
{
    AudioPlcState plc;
    plc.lastFrames = 0;
    plc.pendingMissed = 5;

    const QByteArray in = makePcm(240, 0.3f);
    const QByteArray out = applyConcealmentFade(in, plc, true);

    EXPECT(out == in, "first-packet output must equal input verbatim");
    EXPECT(plc.lastFrames == 240, "lastFrames updated to new frame count");
    EXPECT(plc.pendingMissed == 0, "pendingMissed reset to 0");
}

// 3. Disabled path: enabled=false → input passes through regardless.
void test_disabled_passes_through()
{
    AudioPlcState plc;
    plc.lastFrames = 240;
    plc.tailL = 0.5f;
    plc.tailR = 0.5f;
    plc.pendingMissed = 3;

    const QByteArray in = makePcm(240, 0.0f);
    const QByteArray out = applyConcealmentFade(in, plc, false);

    EXPECT(out == in, "disabled output must equal input verbatim");
    EXPECT(plc.pendingMissed == 0, "pendingMissed reset to 0 even when disabled");
}

// 4. One-packet loss: pendingMissed=1, lastFrames=240 → output is
//    240 + 240 = 480 frames.
void test_single_packet_loss_doubles_output()
{
    AudioPlcState plc;
    plc.lastFrames = 240;
    plc.tailL = 1.0f;
    plc.tailR = 1.0f;
    plc.pendingMissed = 1;

    const QByteArray in = makePcm(240, 1.0f);
    const QByteArray out = applyConcealmentFade(in, plc, true);

    const int outFrames = out.size() / (kChannels * static_cast<int>(sizeof(float)));
    EXPECT(outFrames == 480, "one packet loss prepends one frame of concealment");
    EXPECT(plc.pendingMissed == 0, "pendingMissed reset after handling loss");
}

// 5. Fade-down envelope: first 48 frames should monotonically decrease
//    from tail to ~0 when starting with tail=1.0.
void test_fade_down_envelope_is_monotone()
{
    AudioPlcState plc;
    plc.lastFrames = 240;
    plc.tailL = 1.0f;
    plc.tailR = 1.0f;
    plc.pendingMissed = 1;

    const QByteArray in = makePcm(240, 0.0f);  // input zero — isolates fade-down
    const QByteArray out = applyConcealmentFade(in, plc, true);
    const float* dst = asFloat(out);

    // Fade-down covers first 48 frames; check monotone decrease on L channel.
    constexpr int kFade = 48;
    for (int i = 1; i < kFade; ++i) {
        const float prev = dst[(i - 1) * kChannels];
        const float curr = dst[i * kChannels];
        if (curr > prev + 1e-6f) {
            fail(__FILE__, __LINE__, "fade-down samples not monotone decreasing");
            return;
        }
    }
    // Boundary checks: frame 0 ≈ tailL, frame 47 ≈ 0.
    EXPECT_NEAR(dst[0],                 1.0f, 1e-4f, "fade-down starts near tail");
    EXPECT_NEAR(dst[(kFade - 1) * 2],   0.0f, 0.1f,  "fade-down approaches zero by end");
}

// 6. Fade-up envelope: last 48 frames before normal input should
//    monotonically increase from 0 to ~input level.
void test_fade_up_envelope_is_monotone()
{
    AudioPlcState plc;
    plc.lastFrames = 240;
    plc.tailL = 0.0f;
    plc.tailR = 0.0f;
    plc.pendingMissed = 1;  // 240-frame gap

    const QByteArray in = makePcm(240, 1.0f);  // input constant 1.0
    const QByteArray out = applyConcealmentFade(in, plc, true);
    const float* dst = asFloat(out);

    // Fade-up starts at frame fillFrames=240 and runs 48 frames.
    constexpr int kFade   = 48;
    constexpr int kStart  = 240;
    for (int i = 1; i < kFade; ++i) {
        const float prev = dst[(kStart + i - 1) * kChannels];
        const float curr = dst[(kStart + i) * kChannels];
        if (curr < prev - 1e-6f) {
            fail(__FILE__, __LINE__, "fade-up samples not monotone increasing");
            return;
        }
    }
    // Boundary checks: first fade-up sample ≈ 0, last ≈ 1.0.
    EXPECT_NEAR(dst[kStart * 2],                   0.0f, 1e-4f, "fade-up starts near zero");
    EXPECT_NEAR(dst[(kStart + kFade - 1) * 2],     1.0f, 0.1f,  "fade-up approaches input by end");
}

// 7. Multi-packet loss: pendingMissed=3 → fillFrames = 3 × 240 = 720.
void test_multi_packet_loss_fill_size()
{
    AudioPlcState plc;
    plc.lastFrames = 240;
    plc.tailL = 0.5f;
    plc.tailR = 0.5f;
    plc.pendingMissed = 3;

    const QByteArray in = makePcm(240, 0.5f);
    const QByteArray out = applyConcealmentFade(in, plc, true);

    const int outFrames = out.size() / (kChannels * static_cast<int>(sizeof(float)));
    EXPECT(outFrames == 3 * 240 + 240, "three-packet loss produces 4×240 frames total");
    EXPECT(plc.pendingMissed == 0, "pendingMissed reset after handling loss");

    // Middle of the fill (well past fade-down, well before fade-up) should
    // be exactly zero — silence pad.
    const float* dst = asFloat(out);
    EXPECT_NEAR(dst[100 * kChannels],     0.0f, 1e-6f, "zero-pad in middle of gap");
    EXPECT_NEAR(dst[300 * kChannels],     0.0f, 1e-6f, "zero-pad in middle of gap");
}

// 8. Tail update: after concealment fires, tail reflects the input's
//    final samples (not the cached tail going in).
void test_tail_updates_to_input_last_sample()
{
    AudioPlcState plc;
    plc.lastFrames = 240;
    plc.tailL = 1.0f;   // cached tail before this packet
    plc.tailR = -1.0f;
    plc.pendingMissed = 1;

    // Input where last frame is (0.7, -0.7) and mid-frames are 0.0 — well
    // past the 48-frame fade-up window.
    QByteArray in = makePcm(240, 0.0f);
    auto* p = reinterpret_cast<float*>(in.data());
    p[(240 - 1) * kChannels]     = 0.7f;
    p[(240 - 1) * kChannels + 1] = -0.7f;

    applyConcealmentFade(in, plc, true);

    EXPECT_NEAR(plc.tailL,  0.7f, 1e-6f, "tailL == input's last L sample");
    EXPECT_NEAR(plc.tailR, -0.7f, 1e-6f, "tailR == input's last R sample");
    EXPECT(plc.lastFrames == 240, "lastFrames updated");
}

// 9. Pending counter resets even when called with the empty-input edge case.
void test_empty_input_resets_pending()
{
    AudioPlcState plc;
    plc.lastFrames = 240;
    plc.pendingMissed = 4;

    const QByteArray out = applyConcealmentFade(QByteArray{}, plc, true);
    EXPECT(out.isEmpty(), "empty input → empty output");
    EXPECT(plc.pendingMissed == 0, "pendingMissed reset on empty input");
}

} // namespace

int main()
{
    test_no_loss_passes_through();
    test_first_packet_skips_concealment();
    test_disabled_passes_through();
    test_single_packet_loss_doubles_output();
    test_fade_down_envelope_is_monotone();
    test_fade_up_envelope_is_monotone();
    test_multi_packet_loss_fill_size();
    test_tail_updates_to_input_last_sample();
    test_empty_input_resets_pending();

    if (g_failures == 0) {
        std::printf("packet_loss_concealment_test: all checks passed\n");
        return 0;
    }
    std::printf("packet_loss_concealment_test: %d failure(s)\n", g_failures);
    return 1;
}
