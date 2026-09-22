#pragma once

#include "PcmFrame.h"
#include "Resampler.h"

#include <array>
#include <memory>
#include <optional>

namespace AetherSDR {

// An accepted input can produce an empty block while the continuous 48 -> 24
// converter stages input. Consumers must still honor its discontinuity and end
// anchor. Positions count sample frames, never interleaved floats or bytes.
struct DecoderPcmBlock {
    static constexpr int kSampleRateHz = 24000;
    QVector<float> samples;
    PcmEpochLease source;
    quint64 segmentFirstInputSample = 0;
    quint64 inputEndSample = 0;
    quint64 firstOutputSample = 0;
    int inputSampleRateHz = 24000;
    int groupDelayInputFrames = 0;
    bool discontinuity = false;
    bool current() const { return source.current(); }
};

// One execution context and one selected consumer per instance. Does not start
// producers or own threads. All resets discard filter/staging history without
// erasing replay admission or permitting another still-live producer to take a
// selected route. No finite-stream tail flush or zero padding is performed.
class DecoderPcmAdapter final {
public:
    enum class RouteLane { NativeSlice, Dax, RxDemod };
    static constexpr std::size_t kMaxRoutes = 32;
    static constexpr int kInputBatchFrames = 256;

    // NativeSlice checks frame purpose and sliceId. DAX frames are Auxiliary;
    // their external channel number must be checked by the caller before accept.
    // RxDemod is the radio's single shared receive stream (Speaker purpose): it
    // carries every audible slice already mixed, so it isolates nothing and its
    // key is always 0.
    // There is no selected route at construction. A repeated selection is a no-op.
    bool selectRoute(RouteLane lane, int key);
    void clearRoute();
    // Requires a subsequent explicit selection and a new source for this route.
    // A queued frame from the retired source cannot resurrect the route.
    void retireRoute(RouteLane lane, int key);
    void reset();
    std::optional<DecoderPcmBlock> accept(const PcmFrame& frame);

private:
    struct Route {
        RouteLane lane;
        int key;
        bool operator==(const Route&) const = default;
    };
    struct Pin {
        std::optional<Route> route;
        PcmEpochLease source;
        bool retired = false;
    };
    std::optional<Route> m_route;
    std::array<Pin, kMaxRoutes> m_pins;
    PcmFrameGate m_gate;
    PcmEpochLease m_segmentSource;
    quint64 m_segmentFirstInputSample = 0;
    quint64 m_inputEndSample = 0;
    quint64 m_outputSamples = 0;
    std::array<float, kInputBatchFrames> m_staged{};
    int m_stagedFrames = 0;
    std::unique_ptr<Resampler> m_resampler;
    QByteArray m_converted;
};

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::DecoderPcmBlock)
