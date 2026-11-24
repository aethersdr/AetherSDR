// ReceiverAudioMixer: the speaker mix of host-demodulated receivers. Pins the
// pass-through with one receiver, the sum and its clamp, alignment across
// unequal blocks, an input that has not delivered yet, a stalled input, the
// whole-frame rule, and activation. Pure: no Qt, no DSP.

#include "core/ReceiverAudioMixer.h"

#include <cmath>
#include <cstdio>
#include <span>
#include <vector>

using AetherSDR::ReceiverAudioMixer;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

static std::vector<float> frames(std::size_t n, float left, float right)
{
    std::vector<float> v;
    for (std::size_t i = 0; i < n; ++i) {
        v.push_back(left);
        v.push_back(right);
    }
    return v;
}

static bool allEqual(std::span<const float> s, float left, float right)
{
    for (std::size_t i = 0; i + 1 < s.size(); i += 2) {
        if (s[i] != left || s[i + 1] != right) {
            return false;
        }
    }
    return true;
}

int main()
{
    // ---- one receiver: its own block, untouched ----
    {
        ReceiverAudioMixer mix(100);
        mix.setInputActive(0, true);
        const std::vector<float> block = frames(64, 1.5f, -0.25f);
        const std::span<const float> out = mix.push(0, block);
        check(out.data() == block.data() && out.size() == block.size(),
              "one receiver gets its own block back, not a copy");
        check(allEqual(out, 1.5f, -0.25f),
              "and unclamped: a single receiver's level is not touched");
    }

    // ---- two receivers: summed, aligned, clamped ----
    {
        ReceiverAudioMixer mix(1000);
        mix.setInputActive(0, true);
        mix.setInputActive(1, true);
        const std::vector<float> a = frames(100, 0.25f, 0.5f);
        check(mix.push(0, a).size() == a.size(),
              "until the second receiver delivers, the first is not held back");

        check(mix.push(1, frames(40, 0.25f, 0.125f)).empty(),
              "once both deliver, the mix waits for the one behind");
        const std::span<const float> out = mix.push(0, frames(60, 0.25f, 0.5f));
        check(out.size() == 2 * 40 && allEqual(out, 0.5f, 0.625f),
              "the mix advances as far as both reach, summing frame by frame");
        const std::span<const float> rest = mix.push(1, frames(20, 0.25f, 0.125f));
        check(rest.size() == 2 * 20 && allEqual(rest, 0.5f, 0.625f),
              "the leftover of the one ahead pairs with the next block of the other");

        const std::span<const float> loud = mix.push(1, frames(10, 0.75f, -0.75f));
        check(loud.empty(), "the one behind is now receiver 0");
        const std::span<const float> clamped = mix.push(0, frames(10, 0.75f, -0.75f));
        check(clamped.size() == 2 * 10 && allEqual(clamped, 1.0f, -1.0f),
              "a sum past full scale is clamped, not scaled down by the receiver count");
    }

    // ---- a stalled receiver mutes the other no longer than the lag ----
    {
        ReceiverAudioMixer mix(50);
        mix.setInputActive(0, true);
        mix.setInputActive(1, true);
        (void)mix.push(1, frames(1, 0.0f, 0.0f));   // delivers once, then stalls
        check(mix.push(0, frames(50, 0.1f, 0.1f)).empty(),
              "within the lag the mix waits for the stalled receiver");
        const std::span<const float> out = mix.push(0, frames(30, 0.1f, 0.1f));
        check(out.size() == 2 * 30 && std::abs(out[0] - 0.1f) < 1e-6f,
              "past it, the excess plays with the stalled receiver as silence");
    }

    // ---- the lag a stall leaves drains once the stalled receiver resumes ----
    // DDC2 and DDC3 are separate UDP streams: RX2 can lose packets RX1 does not.
    {
        constexpr std::size_t kLag = 2400;   // 100 ms at 24 kHz, as the ANAN backend
        constexpr std::size_t kBlock = 512;
        ReceiverAudioMixer mix(kLag);
        mix.setInputActive(0, true);
        mix.setInputActive(1, true);
        for (int block = 0; block < 4; ++block) {
            (void)mix.push(0, frames(kBlock, 0.1f, 0.1f));
            (void)mix.push(1, frames(kBlock, 0.1f, 0.1f));
        }
        // RX2 loses twenty blocks. Once RX1 is the lag ahead, each of its
        // blocks plays at once, with RX2 as silence: a stall is not a lag to drop.
        bool steady = true;
        for (int block = 0; block < 20; ++block) {
            const std::size_t out = mix.push(0, frames(kBlock, 0.1f, 0.1f)).size();
            steady = steady && (block < 6 || out == 2 * kBlock);
        }
        check(steady, "through a stall longer than the lag window, RX1 plays block for block");
        check(mix.queuedFrames(0) > kBlock, "precondition: the stall leaves RX1 queued ahead");
        for (int block = 0; block < 20; ++block) {
            (void)mix.push(0, frames(kBlock, 0.1f, 0.1f));
            (void)mix.push(1, frames(kBlock, 0.1f, 0.1f));
        }
        check(mix.queuedFrames(0) <= kBlock && mix.queuedFrames(1) <= kBlock,
              "back in step, RX1's lag drains to a block within the lag window, "
              "not kept for the rest of the session");
    }

    // ---- an ordinary skew of a block between receivers drops nothing ----
    {
        ReceiverAudioMixer mix(2400);
        mix.setInputActive(0, true);
        mix.setInputActive(1, true);
        (void)mix.push(0, frames(256, 0.1f, 0.1f));   // alone, so passed through
        (void)mix.push(1, frames(512, 0.1f, 0.1f));   // both deliver from here on
        std::size_t pushed0 = mix.queuedFrames(0);
        std::size_t pushed1 = mix.queuedFrames(1);
        std::size_t mixed = 0;
        // RX1 at twice RX2's rate delivers half-size blocks, sometimes two
        // ahead of RX2's; RX2 sometimes delivers first.
        for (int round = 0; round < 400; ++round) {
            const bool rx2First = round % 7 == 0;
            if (rx2First) {
                mixed += mix.push(1, frames(512, 0.1f, 0.1f)).size() / 2;
                pushed1 += 512;
            }
            for (int half = 0; half < 2; ++half) {
                mixed += mix.push(0, frames(256, 0.1f, 0.1f)).size() / 2;
                pushed0 += 256;
            }
            if (!rx2First) {
                mixed += mix.push(1, frames(512, 0.1f, 0.1f)).size() / 2;
                pushed1 += 512;
            }
        }
        check(mixed + mix.queuedFrames(0) == pushed0 && mixed + mix.queuedFrames(1) == pushed1,
              "every frame either receiver delivered is mixed or still queued: none dropped");
    }
    {
        // A steady skew under a block: RX1 stays half a block ahead for good.
        ReceiverAudioMixer mix(2400);
        mix.setInputActive(0, true);
        mix.setInputActive(1, true);
        (void)mix.push(1, frames(512, 0.1f, 0.1f));   // alone, so passed through
        (void)mix.push(0, frames(256, 0.1f, 0.1f));
        for (int round = 0; round < 400; ++round) {
            (void)mix.push(0, frames(256, 0.1f, 0.1f));
            (void)mix.push(0, frames(256, 0.1f, 0.1f));
            (void)mix.push(1, frames(512, 0.1f, 0.1f));
        }
        check(mix.queuedFrames(0) == 256,
              "a skew of less than a block is the receivers' own timing, and is kept");
    }

    // ---- whole frames only ----
    {
        ReceiverAudioMixer mix(100);
        mix.setInputActive(0, true);
        mix.setInputActive(1, true);
        (void)mix.push(0, frames(4, 0.0f, 0.0f));   // receiver 1 has not delivered yet
        std::vector<float> odd = frames(4, 0.25f, 0.5f);
        odd.push_back(0.9f);   // half a fifth frame
        (void)mix.push(1, odd);
        const std::span<const float> first = mix.push(0, frames(4, 0.0f, 0.0f));
        check(first.size() == 2 * 4 && allEqual(first, 0.25f, 0.5f),
              "the four whole frames mix");
        (void)mix.push(1, frames(2, 0.25f, 0.5f));
        const std::span<const float> tail = mix.push(0, frames(2, 0.0f, 0.0f));
        check(tail.size() == 2 * 2 && allEqual(tail, 0.25f, 0.5f),
              "a trailing half frame is dropped, so the next block's channels do not swap");
    }

    // ---- activation ----
    {
        ReceiverAudioMixer mix(100);
        mix.setInputActive(0, true);
        mix.setInputActive(1, true);
        (void)mix.push(1, frames(8, 0.5f, 0.5f));
        mix.setInputActive(1, false);
        check(!mix.isInputActive(1), "an input can be deactivated");
        const std::vector<float> alone = frames(8, 0.25f, 0.25f);
        check(mix.push(0, alone).data() == alone.data(),
              "and the remaining receiver is passed through again, its queue dropped");
        check(mix.push(1, frames(8, 0.5f, 0.5f)).empty(),
              "an inactive input's blocks are ignored");
        check(mix.push(9, alone).empty() && !mix.isInputActive(-1),
              "an input outside the mixer's range is ignored");

        mix.setInputActive(1, true);
        (void)mix.push(1, frames(8, 0.5f, 0.5f));
        mix.reset();
        check(mix.push(0, alone).data() == alone.data(),
              "reset empties the queues and waits for every input's first block again");
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "receiver_audio_mixer_test: all checks passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
