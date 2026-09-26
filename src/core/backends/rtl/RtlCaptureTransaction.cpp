#include "core/backends/rtl/RtlCaptureTransaction.h"
#include "core/backends/rtl/RtlViewport.h"

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

Policy::Error validateConfigured(const std::vector<T::Receiver>& receivers,
                                 Policy::ReceiverLimits limits)
{
    if (!limits.slotCount || limits.slotCount > 8 || !limits.capacity
        || limits.capacity > limits.slotCount || receivers.empty()) {
        return Policy::Error::InvalidCapacity;
    }
    if (receivers.size() > limits.capacity) { return Policy::Error::CapacityExceeded; }
    std::array<bool, 8> used{};
    for (const T::Receiver& receiver : receivers) {
        const int id = receiver.passband.stableId;
        if (id < 0 || static_cast<std::size_t>(id) >= limits.slotCount) {
            return Policy::Error::InvalidIdentity;
        }
        if (used[id]) { return Policy::Error::DuplicateId; }
        used[id] = true;
        const auto occupied = Policy::occupiedInterval(receiver.passband);
        if (!occupied.interval) { return occupied.error; }
        if ((receiver.mode == T::Mode::Fm || receiver.mode == T::Mode::Fmn)
            && (receiver.passband.filterLowHz < -21600
                || receiver.passband.filterHighHz > 21600
                || receiver.passband.filterLowHz >= 0
                || receiver.passband.filterHighHz <= 0)) {
            return Policy::Error::InvalidInterval;
        }
        if (receiver.passband.carrierHz < kDomains.front().minimumHz
            || receiver.passband.carrierHz > kDomains.front().maximumHz) {
            return Policy::Error::InvalidNumber;
        }
    }
    return Policy::Error::None;
}

struct Membership {
    Policy::Error error = Policy::Error::None;
    std::vector<int> ids;
};

Membership receivingIn(const Policy::CaptureDescriptor& capture,
                       const std::vector<T::Receiver>& configured,
                       Policy::ReceiverLimits limits)
{
    const std::vector<Policy::SliceDescriptor> descriptors = passbands(configured);
    const Policy::CenterDomain fixed{capture.centerHz, capture.centerHz, capture.centerHz, 1};
    const Policy::RestoreResult fitted = Policy::restoreFixedCapture(capture, descriptors,
        std::span(&fixed, 1), limits);
    if (fitted.error != Policy::Error::None) { return {fitted.error, {}}; }
    for (const Policy::RestoreRejection& rejected : fitted.rejected) {
        if (rejected.reason != Policy::Error::OutsideCapture) { return {rejected.reason, {}}; }
    }
    Membership result;
    for (const Policy::SliceDescriptor& descriptor : fitted.accepted) {
        result.ids.push_back(descriptor.stableId);
    }
    return result;
}

std::vector<Policy::SliceDescriptor> receivingPassbands(const T::State& state)
{
    std::vector<Policy::SliceDescriptor> result;
    result.reserve(state.receivingIds.size());
    for (int id : state.receivingIds) {
        const auto receiver = std::ranges::find_if(state.receivers, [id](const T::Receiver& value) {
            return value.passband.stableId == id;
        });
        if (receiver != state.receivers.end()) { result.push_back(receiver->passband); }
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

std::vector<Policy::CenterDomain> dcClearDomains(const T::State& state,
                                                const T::Receiver& receiver)
{
    const Policy::Interval excluded = dcExclusion(receiver);
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
    if (excluded.highHz > low && excluded.lowHz < high) {
        append(low, std::min(excluded.lowHz, high));
        low = std::max(low, excluded.highHz);
    }
    append(low, high);
    return domains;
}

bool dcClearFor(const T::Receiver& receiver, double centerHz)
{
    const Policy::Interval interval = dcExclusion(receiver);
    return centerHz <= interval.lowHz || centerHz >= interval.highHz;
}

bool centersRequestedView(const Policy::CaptureDescriptor& capture,
                          const T::Desired::CenteredView& request)
{
    const auto view = RtlViewport::fit(capture, RtlViewport::kRtlSpectrumBins,
                                       request.centerHz, request.spanHz);
    const double halfBinHz = capture.achievedSampleRateHz
        / (2.0 * RtlViewport::kRtlSpectrumBins);
    return view && std::abs(view->centerHz - request.centerHz) <= halfBinHz + 1.0;
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
    if (validateConfigured(state.receivers, {8, 8}) != Policy::Error::None) { return false; }
    Policy::CaptureDescriptor actual = state.capture;
    actual.centerHz = hardware.centerHz;
    actual.achievedSampleRateHz = hardware.sampleRateHz;
    // Transitional legacy DDC margin, not a measured multi-RX bandwidth claim.
    actual.usableLeftHz = actual.usableRightHz = hardware.sampleRateHz * 0.45;
    const Membership fitted = receivingIn(actual, state.receivers, {8, 8});
    return fitted.error == Policy::Error::None && fitted.ids == state.receivingIds
        && Policy::validateReadback(state.capture, actual, receivingPassbands(state),
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
    const Policy::Error configured = validateConfigured(desired.receivers, m_limits);
    if (configured != Policy::Error::None) { return {{}, configured}; }
    if (desired.centeredView
        && (!desired.followReceiverId || !std::isfinite(desired.centeredView->centerHz)
            || !std::isfinite(desired.centeredView->spanHz)
            || desired.centeredView->centerHz < kDomains.front().minimumHz
            || desired.centeredView->centerHz > kDomains.front().maximumHz
            || desired.centeredView->spanHz <= 0)) {
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
    target.dcSuppression = desired.dcSuppression;
    target.capture = {m_session, target.token.revision, double(desired.hardware.centerHz),
        double(desired.hardware.sampleRateHz), desired.hardware.sampleRateHz * 0.45,
        desired.hardware.sampleRateHz * 0.45};
    const Receiver* followed = nullptr;
    if (desired.followReceiverId) {
        const auto found = std::ranges::find_if(target.receivers, [id = *desired.followReceiverId](
            const Receiver& receiver) { return receiver.passband.stableId == id; });
        if (found == target.receivers.end()) { return {{}, Policy::Error::InvalidIdentity}; }
        followed = &*found;
    } else if (!m_confirmed) {
        // Startup must establish one real receiver rather than a valid but
        // empty DSP capture when a saved center and first slice disagree.
        followed = &target.receivers.front();
    } else if (desired.avoidDc) {
        for (int id : m_confirmed->receivingIds) {
            const auto found = std::ranges::find_if(target.receivers, [id](const Receiver& receiver) {
                return receiver.passband.stableId == id;
            });
            if (found != target.receivers.end() && narrowFm(*found)) {
                followed = &*found;
                break;
            }
        }
        if (!followed) { return {{}, Policy::Error::NoLegalCenter}; }
    }
    const std::vector<Policy::SliceDescriptor> required = followed
        ? std::vector<Policy::SliceDescriptor>{followed->passband}
        : std::vector<Policy::SliceDescriptor>{};
    // Operator-requested free RF browsing intentionally parks receivers that
    // leave capture. The earlier all-active/refuse RFC policy needs maintainer
    // review before this behavior is merged.
    const auto selection = Policy::selectCenter(target.capture, required, kDomains, m_limits);
    if (!selection.capture) { return {{}, selection.error}; }
    target.capture = *selection.capture;
    if (desired.centeredView && !centersRequestedView(target.capture, *desired.centeredView)) {
        const auto nearest = RtlViewport::captureCenterFor(target.capture,
            RtlViewport::kRtlSpectrumBins, desired.centeredView->centerHz,
            desired.centeredView->spanHz);
        if (!nearest) { return {{}, Policy::Error::NoLegalCenter}; }
        auto preferred = target.capture;
        preferred.centerHz = *nearest;
        const auto centered = Policy::selectCenter(preferred, required, kDomains, m_limits);
        if (!centered.capture || !centersRequestedView(*centered.capture, *desired.centeredView)) {
            return {{}, Policy::Error::NoLegalCenter};
        }
        target.capture = *centered.capture;
    }
    const bool recentered = target.capture.centerHz != desired.hardware.centerHz;
    if (followed && narrowFm(*followed)
        && (!m_confirmed || recentered || desired.avoidDc)
        && (!m_confirmed || recentered || !dcClearFor(*followed, target.capture.centerHz))) {
        // An explicit slice tune may move capture away from converter DC;
        // when it must retune, prefer room for a centered viewport as well.
        // Display-only free pan cannot silently displace the operator's view.
        const auto domains = dcClearDomains(target, *followed);
        if (domains.empty() && (!desired.centeredView || desired.avoidDc)) {
            return {{}, Policy::Error::NoLegalCenter};
        }
        if (!domains.empty()) {
            auto preferred = target.capture;
            preferred.centerHz = followed->passband.carrierHz
                + followed->passband.translationHz + target.hardware.sampleRateHz / 4.0;
            const auto displaced = Policy::selectCenter(preferred, required, domains, m_limits);
            if (displaced.capture && (!desired.centeredView
                || centersRequestedView(*displaced.capture, *desired.centeredView))) {
                target.capture = *displaced.capture;
            } else if (desired.centeredView) {
                // A nearly full-width view may admit a small DC-clear offset,
                // even when the quarter-rate preference cannot center it.
                const auto nearest = Policy::selectCenter(target.capture, required, domains, m_limits);
                if (nearest.capture && centersRequestedView(*nearest.capture, *desired.centeredView)) {
                    target.capture = *nearest.capture;
                } else if (desired.avoidDc) {
                    return {{}, Policy::Error::NoLegalCenter};
                }
            } else {
                return {{}, displaced.error};
            }
        }
    }
    const Membership fitted = receivingIn(target.capture, target.receivers, m_limits);
    if (fitted.error != Policy::Error::None) { return {{}, fitted.error}; }
    target.receivingIds = fitted.ids;
    if (followed && std::ranges::find(target.receivingIds, followed->passband.stableId)
            == target.receivingIds.end()) {
        return {{}, Policy::Error::OutsideCapture};
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
    for (int id : state.receivingIds) {
        const auto receiver = std::ranges::find_if(state.receivers, [id](const Receiver& value) {
            return value.passband.stableId == id;
        });
        if (receiver == state.receivers.end()) { return false; }
        if (narrowFm(*receiver) && (!Policy::occupiedInterval(receiver->passband).interval
            || !dcClearFor(*receiver, state.capture.centerHz))) { return false; }
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
                || result.actual->receivingIds != work.target.receivingIds
                || result.actual->receivers != work.target.receivers
                || result.actual->dcSuppression != work.target.dcSuppression
                || result.actual->automaticDirectSampling != work.target.automaticDirectSampling
                || !matches(work.target, result.actual->hardware)))
        || (result.code == ResultCode::Restored
            && (!work.before || !result.actual || result.actual->token != work.before->token
                || result.actual->capture != work.before->capture
                || result.actual->receivingIds != work.before->receivingIds
                || result.actual->receivers != work.before->receivers
                || result.actual->dcSuppression != work.before->dcSuppression
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
