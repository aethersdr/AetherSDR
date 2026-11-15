#include "PacketLossConcealment.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace AetherSDR {

namespace {

// M_PI is a POSIX extension; MSVC only exposes it when _USE_MATH_DEFINES
// is set before <cmath>.  Define our own portable constant so this
// translation unit doesn't depend on either path. (#2732 check-windows fix)
constexpr float kPi = 3.14159265358979323846f;

} // namespace

QByteArray applyConcealmentFade(QByteArray pcm, AudioPlcState& plc, bool enabled)
{
    constexpr int kChannels   = 2;
    constexpr int kFadeFrames = 48;  // ~2 ms @ 24 kHz

    const auto* in = reinterpret_cast<const float*>(pcm.constData());
    const int newFrames = pcm.size() / static_cast<int>(kChannels * sizeof(float));
    if (newFrames <= 0) {
        plc.pendingMissed = 0;
        return pcm;
    }

    const bool conceal = enabled
                         && plc.pendingMissed > 0
                         && plc.lastFrames > 0;
    if (!conceal) {
        plc.tailL = in[(newFrames - 1) * kChannels];
        plc.tailR = in[(newFrames - 1) * kChannels + 1];
        plc.lastFrames = newFrames;
        plc.pendingMissed = 0;
        return pcm;
    }

    const int fillFrames = plc.pendingMissed * plc.lastFrames;
    QByteArray out((fillFrames + newFrames) * kChannels * static_cast<int>(sizeof(float)),
                   Qt::Uninitialized);
    auto* dst = reinterpret_cast<float*>(out.data());

    // Cosine fade-down from cached tail to zero (~2 ms).
    const int fadeDown = std::min(kFadeFrames, fillFrames);
    for (int i = 0; i < fadeDown; ++i) {
        const float w =
            0.5f * (1.0f + std::cos(kPi * i / fadeDown));
        dst[i * kChannels]     = plc.tailL * w;
        dst[i * kChannels + 1] = plc.tailR * w;
    }
    if (fillFrames > fadeDown) {
        std::memset(dst + fadeDown * kChannels, 0,
                    (fillFrames - fadeDown) * kChannels * sizeof(float));
    }

    // Cosine fade-up into the new packet's first ~2 ms.
    const int fadeUp = std::min(kFadeFrames, newFrames);
    for (int i = 0; i < fadeUp; ++i) {
        const float w =
            0.5f * (1.0f - std::cos(kPi * i / fadeUp));
        dst[(fillFrames + i) * kChannels]     = in[i * kChannels]     * w;
        dst[(fillFrames + i) * kChannels + 1] = in[i * kChannels + 1] * w;
    }
    if (newFrames > fadeUp) {
        std::memcpy(dst + (fillFrames + fadeUp) * kChannels,
                    in  + fadeUp * kChannels,
                    (newFrames - fadeUp) * kChannels * sizeof(float));
    }

    plc.tailL = in[(newFrames - 1) * kChannels];
    plc.tailR = in[(newFrames - 1) * kChannels + 1];
    plc.lastFrames = newFrames;
    plc.pendingMissed = 0;
    return out;
}

} // namespace AetherSDR
