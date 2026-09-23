#include "core/backends/rtl/RtlCaptureTransaction.h"

#include <array>
#include <algorithm>
#include <cmath>
#include <limits>

namespace AetherSDR::rtl {
namespace {
using T = RtlCaptureTransaction;
namespace Policy = SharedCapturePolicy;
constexpr std::array<Policy::CenterDomain, 1> kDomains{{{24'000, 1'766'000'000, 0, 1}}};

bool validHardware(const T::Hardware& hardware)
{
    const std::uint32_t rate = hardware.sampleRateHz;
    return hardware.centerHz >= 24'000 && hardware.centerHz <= 1'766'000'000
        && ((rate >= 225'001 && rate <= 300'000) || (rate >= 900'001 && rate <= 3'000'000))
        && hardware.directSampling >= 0 && hardware.directSampling <= 2
        && (hardware.offsetTuning == 0 || hardware.offsetTuning == 1)
        && !(hardware.directSampling != 0 && hardware.offsetTuning != 0)
        && hardware.ppm >= -1000 && hardware.ppm <= 1000
        && hardware.gainTenths >= -1000 && hardware.gainTenths <= 1000;
}

std::vector<Policy::SliceDescriptor> passbands(const std::vector<T::Receiver>& receivers)
{
    std::vector<Policy::SliceDescriptor> result;
    result.reserve(receivers.size());
    for (const T::Receiver& receiver : receivers) {
        result.push_back(receiver.passband);
    }
    return result;
}

bool narrowFm(const T::Receiver& receiver)
{
    return receiver.mode == T::Mode::Fm || receiver.mode == T::Mode::Fmn;
}

Policy::Interval dcExclusion(const T::Receiver& receiver)
{
    const auto occupied = Policy::occupiedInterval(receiver.passband);
    const double carrier = receiver.passband.carrierHz + receiver.passband.translationHz;
    return {std::min(occupied.interval->lowHz, carrier - T::kDcSeparationHz),
            std::max(occupied.interval->highHz, carrier + T::kDcSeparationHz)};
}

std::vector<Policy::CenterDomain> dcClearDomains(const T::State& state)
{
    std::vector<Policy::Interval> excluded;
    for (const T::Receiver& receiver : state.receivers) {
        if (narrowFm(receiver)) { excluded.push_back(dcExclusion(receiver)); }
    }
    std::ranges::sort(excluded, {}, &Policy::Interval::lowHz);
    double low = 24'000;
    double high = 1'766'000'000;
    // Placement alone must not cross the automatic front-end mode boundary.
    // A tune that already requires crossing it is handled by ordinary selection.
    if (state.automaticDirectSampling) {
        if (state.capture.centerHz < 24'000'000) { high = 23'999'999; }
        else { low = 24'000'000; }
    }
    std::vector<Policy::CenterDomain> domains;
    const auto append = [&domains](double first, double last) {
        first = std::ceil(first); last = std::floor(last);
        if (first <= last) { domains.push_back({first, last, 0, 1}); }
    };
    for (const Policy::Interval& interval : excluded) {
        if (interval.highHz <= low || interval.lowHz >= high) { continue; }
        append(low, std::min(interval.lowHz, high));
        low = std::max(low, interval.highHz);
        if (low > high) { break; }
    }
    append(low, high);
    return domains;
}

bool apply(const T::Hardware& hardware, T::DeviceOperations& device, bool compensate)
{
    // Rate/PPM/offset can themselves retune. Establish front-end mode first and
    // set center LAST among geometry controls. On rollback attempt every field,
    // even after one fails, but never declare validity on a partial restore.
    const std::array<std::pair<T::Control, std::int64_t>, 6> controls{{
        {T::Control::DirectSampling, hardware.directSampling},
        {T::Control::SampleRate, hardware.sampleRateHz},
        {T::Control::Ppm, hardware.ppm},
        {T::Control::OffsetTuning, hardware.offsetTuning},
        {T::Control::Center, hardware.centerHz},
        {T::Control::Gain, hardware.gainTenths}
    }};
    bool success = true;
    for (const auto& [control, value] : controls) {
        if (!device.set(control, value)) {
            success = false;
            if (!compensate) { break; }
        }
    }
    return success;
}

bool matches(const T::State& state, const T::Hardware& hardware)
{
    if (!validHardware(hardware) || hardware != state.hardware) { return false; }
    Policy::CaptureDescriptor actual = state.capture;
    actual.centerHz = hardware.centerHz;
    actual.achievedSampleRateHz = hardware.sampleRateHz;
    // Transitional legacy DDC margin, not a measured multi-RX bandwidth claim.
    actual.usableLeftHz = actual.usableRightHz = hardware.sampleRateHz * 0.45;
    return Policy::validateReadback(state.capture, actual, passbands(state.receivers),
                                    kDomains, {8, 8}) == Policy::Error::None;
}
} // namespace

RtlCaptureTransaction::RtlCaptureTransaction(Policy::ReceiverLimits limits)
    : m_limits(limits)
{
}

std::uint64_t RtlCaptureTransaction::beginSession()
{
    endSession();
    m_open = true;
    return m_session;
}

void RtlCaptureTransaction::endSession()
{
    ++m_session;
    m_open = false;
    m_revision = 0;
    m_pending.reset();
    m_active.reset();
    m_confirmed.reset();
    m_dispatched = false;
}

RtlCaptureTransaction::Submission RtlCaptureTransaction::submit(const Desired& desired)
{
    if (!m_open || m_revision == std::numeric_limits<std::uint64_t>::max()
        || !validHardware(desired.hardware) || desired.receivers.empty()
        || desired.receivers.size() > 8 || m_limits.slotCount > 8) {
        return {{}, Policy::Error::InvalidNumber};
    }
    for (const Receiver& receiver : desired.receivers) {
        if (receiver.mode < Mode::Am || receiver.mode > Mode::Cwr
            || receiver.audioGain < 0 || receiver.audioGain > 100
            || receiver.audioPan < 0 || receiver.audioPan > 100
            || receiver.squelchLevel < 0 || receiver.squelchLevel > 100
            || (receiver.squelchEnabled && receiver.mode != Mode::Fm && receiver.mode != Mode::Fmn)) {
            return {{}, Policy::Error::InvalidNumber};
        }
    }
    State target;
    target.token = {m_session, m_revision + 1};
    target.hardware = desired.hardware;
    target.receivers = desired.receivers;
    target.automaticDirectSampling = desired.automaticDirectSampling;
    target.capture = {m_session, target.token.revision, double(desired.hardware.centerHz),
        double(desired.hardware.sampleRateHz), desired.hardware.sampleRateHz * 0.45,
        desired.hardware.sampleRateHz * 0.45};
    const auto selection = Policy::selectCenter(target.capture, passbands(target.receivers),
                                                 kDomains, m_limits);
    if (!selection.capture) { return {{}, selection.error}; }
    target.capture = *selection.capture;
    const bool recentered = target.capture.centerHz != desired.hardware.centerHz;
    const bool hasFm = std::ranges::any_of(target.receivers, narrowFm);
    if (hasFm && (!m_confirmed || recentered || desired.avoidDc)) {
        // Preserve an already-clear established capture for an explicit repeat
        // or a mode entry. A required retune instead leaves useful view room on
        // both sides of the wanted carrier, rather than pinning it to an edge.
        if (!m_confirmed || recentered || !dcClear(target)) {
            const auto domains = dcClearDomains(target);
            if (domains.empty()) { return {{}, Policy::Error::NoLegalCenter}; }
            const auto receiver = std::ranges::find_if(target.receivers, narrowFm);
            auto preferred = target.capture;
            preferred.centerHz = receiver->passband.carrierHz
                + receiver->passband.translationHz + target.hardware.sampleRateHz / 4.0;
            const auto displaced = Policy::selectCenter(preferred, passbands(target.receivers),
                                                        domains, m_limits);
            if (!displaced.capture) { return {{}, displaced.error}; }
            target.capture = *displaced.capture;
        }
    }
    target.hardware.centerHz = static_cast<std::uint32_t>(target.capture.centerHz);
    if (desired.automaticDirectSampling) {
        target.hardware.directSampling = target.hardware.centerHz < 24'000'000 ? 2 : 0;
    }
    if (!validHardware(target.hardware)) { return {{}, Policy::Error::InvalidNumber}; }
    if (m_confirmed && target.hardware == m_confirmed->hardware) {
        target.capture.generation = m_confirmed->capture.generation;
    }
    m_revision = target.token.revision;
    m_pending = std::move(target);
    return {{m_session, m_revision}, Policy::Error::None};
}

bool RtlCaptureTransaction::dcClear(const State& state)
{
    for (const Receiver& receiver : state.receivers) {
        if (!narrowFm(receiver)) { continue; }
        if (!Policy::occupiedInterval(receiver.passband).interval) { return false; }
        const Policy::Interval interval = dcExclusion(receiver);
        if (state.capture.centerHz > interval.lowHz && state.capture.centerHz < interval.highHz) {
            return false;
        }
    }
    return true;
}

std::optional<RtlCaptureTransaction::Work> RtlCaptureTransaction::takeWork()
{
    if (m_active) {
        if (m_dispatched) { return {}; }
        m_dispatched = true;
        return m_active;
    }
    if (!m_pending) { return {}; }
    Work work;
    work.token = m_pending->token;
    work.target = std::move(*m_pending);
    m_pending.reset();
    work.before = m_confirmed;
    work.operation = ++m_operation;
    work.hardwareChanged = !m_confirmed || work.target.hardware != m_confirmed->hardware;
    m_active = work;
    m_dispatched = true;
    return work;
}

RtlCaptureTransaction::Completion RtlCaptureTransaction::complete(const Result& result)
{
    if (!m_active || !m_dispatched || result.token != m_active->token
        || result.operation != m_active->operation
        || result.token.session != m_session) {
        return Completion::Ignored;
    }
    const Work work = std::move(*m_active);
    m_active.reset();
    m_dispatched = false;
    // Treat malformed successful worker readback just like lost validity. Do
    // not trust a completion merely because it carries the expected token.
    if ((result.code != ResultCode::Applied && result.code != ResultCode::Restored)
        || (result.code == ResultCode::Applied
            && (!result.actual || result.actual->token != work.target.token
                || result.actual->capture != work.target.capture
                || result.actual->receivers != work.target.receivers
                || result.actual->automaticDirectSampling != work.target.automaticDirectSampling
                || !matches(work.target, result.actual->hardware)))
        || (result.code == ResultCode::Restored
            && (!work.before || !result.actual || result.actual->token != work.before->token
                || result.actual->capture != work.before->capture
                || result.actual->receivers != work.before->receivers
                || result.actual->automaticDirectSampling != work.before->automaticDirectSampling
                || !matches(*work.before, result.actual->hardware)))) {
        m_confirmed.reset();
        m_pending.reset();
        m_open = false;
        return Completion::Invalidated;
    }
    if (work.compensation) {
        if (result.code != ResultCode::Applied) {
            m_confirmed.reset(); m_pending.reset();
            m_open = false;
            return Completion::Invalidated;
        }
        return Completion::Ignored;
    }
    if (result.code == ResultCode::Restored) {
        return Completion::Failed;
    }
    if (result.token.revision != m_revision) {
        if (!work.before) {
            m_confirmed.reset(); m_pending.reset();
            m_open = false;
            return Completion::Invalidated;
        }
        // The device or receiver bank may have committed just before a newer
        // request arrived. Restore the last published bank before advancing.
        m_active = Work{work.token, *work.before, result.actual,
            work.target.hardware != work.before->hardware, true, ++m_operation};
        return Completion::Compensating;
    }
    m_confirmed = result.actual;
    return Completion::Published;
}

RtlCaptureTransaction::Result RtlCaptureTransaction::execute(const Work& work,
                                                            DeviceOperations& device)
{
    if (!work.hardwareChanged) {
        return {work.token, ResultCode::Applied, work.target, work.operation};
    }
    if (apply(work.target.hardware, device, work.compensation)) {
        const auto actual = device.read();
        if (actual && matches(work.target, *actual)) {
            return {work.token, ResultCode::Applied, work.target, work.operation};
        }
    }
    if (!work.compensation && work.before && apply(work.before->hardware, device, true)) {
        const auto restored = device.read();
        if (restored && matches(*work.before, *restored)) {
            return {work.token, ResultCode::Restored, work.before, work.operation};
        }
    }
    return {work.token, ResultCode::Invalid, {}, work.operation};
}
} // namespace AetherSDR::rtl
