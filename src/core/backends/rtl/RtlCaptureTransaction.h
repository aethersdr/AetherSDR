#pragma once

#include "core/SharedCapturePolicy.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace AetherSDR::rtl {

// M1a control-side owner. All methods except execute() run on the backend's
// thread. One immutable work item may execute elsewhere, with one replaceable
// desired set waiting here. No USB, QObject or settings ownership in this class.
class RtlCaptureTransaction final {
public:
    enum class Mode { Am, Sam, Fm, Fmn, Wfm, Usb, Lsb, Cw, Cwr };
    struct Hardware {
        std::uint32_t centerHz = 95'200'000;
        std::uint32_t sampleRateHz = 2'400'000;
        int directSampling = 0;
        int offsetTuning = 0;
        int ppm = 0;
        int gainTenths = 240;
        bool operator==(const Hardware&) const = default;
    };
    struct Receiver {
        SharedCapturePolicy::SliceDescriptor passband;
        Mode mode = Mode::Wfm;
        int audioGain = 100;
        int audioPan = 50;
        bool audioMute = false;
        bool operator==(const Receiver&) const = default;
    };
    struct Desired {
        Hardware hardware;
        std::vector<Receiver> receivers;
        bool automaticDirectSampling = true;
    };
    struct Token {
        std::uint64_t session = 0;
        std::uint64_t revision = 0;
        bool operator==(const Token&) const = default;
    };
    struct State {
        Token token;
        Hardware hardware;
        SharedCapturePolicy::CaptureDescriptor capture;
        std::vector<Receiver> receivers;
        bool automaticDirectSampling = true;
    };
    struct Work {
        Token token; // completion identity; rollback target retains its old token
        State target;
        std::optional<State> before;
        bool hardwareChanged = false;
        bool compensation = false;
        std::uint64_t operation = 0; // distinguishes compensation from forward completion
    };
    enum class Control { DirectSampling, SampleRate, Ppm, OffsetTuning, Center, Gain };
    class DeviceOperations {
    public:
        virtual ~DeviceOperations() = default;
        virtual bool set(Control control, std::int64_t value) = 0;
        virtual std::optional<Hardware> read() = 0;
    };
    enum class ResultCode { Applied, Restored, Invalid };
    struct Result {
        Token token;
        ResultCode code = ResultCode::Invalid;
        std::optional<State> actual;
        std::uint64_t operation = 0;
    };
    enum class Completion { Ignored, Published, Failed, Compensating, Invalidated };
    struct Submission {
        Token token;
        SharedCapturePolicy::Error error = SharedCapturePolicy::Error::None;
        explicit operator bool() const { return token.revision != 0; }
    };

    explicit RtlCaptureTransaction(SharedCapturePolicy::ReceiverLimits limits);
    std::uint64_t beginSession();
    void endSession();
    Submission submit(const Desired& desired);
    std::optional<Work> takeWork();
    Completion complete(const Result& result);
    const std::optional<State>& confirmed() const { return m_confirmed; }
    std::size_t pendingCount() const { return m_pending ? 1 : 0; }
    bool busy() const { return m_active.has_value() || m_pending.has_value(); }
    Token requested() const { return {m_session, m_revision}; }

    // Run ONLY with USB acquisition quiesced. Success is provisional until
    // complete() validates the session/revision on the backend thread. A failed
    // apply restores and verifies the ENTIRE previous hardware configuration.
    static Result execute(const Work& work, DeviceOperations& device);

private:
    SharedCapturePolicy::ReceiverLimits m_limits;
    std::uint64_t m_session = 0;
    std::uint64_t m_revision = 0;
    std::optional<State> m_confirmed;
    std::optional<State> m_pending;
    std::optional<Work> m_active;
    bool m_open = false;
    bool m_dispatched = false;
    std::uint64_t m_operation = 0;
};

} // namespace AetherSDR::rtl
