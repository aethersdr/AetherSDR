#include "ReceiveControlTarget.h"
#include "ReceiveControlGuard.h"
#include "models/PanadapterModel.h"
#include "models/TransmitModel.h"

#include <QHash>
#include <QSet>

namespace AetherSDR::control {
namespace {
using Authority = SliceFrequencyControl::Authority;

bool known(Authority authority)
{
    return authority == Authority::Radio || authority == Authority::Engine;
}

bool validRange(const std::optional<ReceivePanRangeControl>& range)
{
    return range && known(range->authority) && range->minimumHz > 0
        && range->maximumHz >= range->minimumHz
        && range->maximumHz <= SliceModel::kMaximumReportedFrequencyHz;
}

const ReceiveFilterMode* filterMode(const RadioCapabilities& caps, const QString& mode)
{
    if (!caps.receiveFilterControl || !known(caps.receiveFilterControl->authority)) {
        return nullptr;
    }
    for (const ReceiveFilterMode& range : caps.receiveFilterControl->modes) {
        if (range.mode == mode && range.minimumLowHz <= range.maximumLowHz
            && range.minimumHighHz <= range.maximumHighHz && range.minimumWidthHz > 0
            && range.maximumWidthHz >= range.minimumWidthHz) {
            return &range;
        }
    }
    return nullptr;
}

bool dspSupports(const RadioCapabilities& caps, ReceiveDspControl::Feature feature,
                 const std::optional<QString>& mode)
{
    return caps.receiveDspControl && known(caps.receiveDspControl->authority) && mode
        && caps.receiveDspControl->supports(feature, *mode);
}

class ModelReceiveControlTarget final : public ReceiveControlTarget {
public:
    ModelReceiveControlTarget(RadioModel* radio, RadioConnectionTarget* connection)
        : m_radio(radio), m_guard(radio, connection)
    {
        connect(radio, &RadioModel::backendRebuilt, this, [this] { m_pendingModes.clear(); });
        connect(radio, &RadioModel::connectionStateChanged, this, [this](bool connected) {
            if (!connected) {
                m_pendingModes.clear();
            }
        });
    }

    bool available(ReceiveOperation op) const override
    {
        if (thread() != QThread::currentThread() || !m_guard.ready()) {
            return false;
        }
        if (op == ReceiveOperation::CwPitch) {
            return !checkCwPitch();
        }
        if (op == ReceiveOperation::PanCenter || op == ReceiveOperation::PanBandwidth
            || op == ReceiveOperation::PanRfGain) {
            for (PanadapterModel* pan : m_radio->panadapters()) {
                if (pan && !checkPan(pan->panId(), op)) {
                    return true;
                }
            }
        } else {
            for (SliceModel* slice : m_radio->slices()) {
                if (slice && !checkSlice(slice->sliceId(), op)) {
                    return true;
                }
            }
        }
        return false;
    }

    std::optional<ProtocolError> setMode(int id, const QString& mode) override
    {
        if (const auto error = checkSlice(id, ReceiveOperation::Mode)) {
            return error;
        }
        const RadioCapabilities caps = m_radio->backendCapabilities();
        if (!caps.receiveModeControl->modes.contains(mode)) {
            return refusal("request.out_of_range", "mode is not supported by this receive path");
        }
        SliceModel* slice = m_radio->slice(id);
        if (!m_watchedSlices.contains(slice)) {
            m_watchedSlices.insert(slice);
            connect(slice, &SliceModel::receiveModeReported, this, [this, slice] {
                // An old-mode report may already be queued ahead of the intent's
                // effect. It is not a rejection/acknowledgement; releasing here
                // would admit an old-mode filter behind the pending mode write.
                const auto pending = m_pendingModes.constFind(slice);
                if (pending != m_pendingModes.constEnd()
                    && slice->receiveObservation().mode == pending.value()) {
                    m_pendingModes.remove(slice);
                }
            });
            connect(slice, &QObject::destroyed, this, [this, slice] {
                m_pendingModes.remove(slice);
                m_watchedSlices.remove(slice);
            });
        }
        // A delayed mode write must not let a filter validated against the old
        // mode follow it. This is a filter-admission interlock, not a TX lease
        // or a claim that the mode response confirms hardware completion.
        m_pendingModes.insert(slice, mode);
        m_radio->backend()->setSliceMode(id, mode);
        return {};
    }

    std::optional<ProtocolError> setFilter(int id, int low, int high) override
    {
        if (const auto error = checkSlice(id, ReceiveOperation::Filter)) {
            return error;
        }
        SliceModel* slice = m_radio->slice(id);
        const RadioCapabilities caps = m_radio->backendCapabilities();
        const ReceiveFilterMode* range = filterMode(caps, *slice->receiveObservation().mode);
        const qint64 width = qint64(high) - low;
        if (!range || low < range->minimumLowHz || low > range->maximumLowHz
            || high < range->minimumHighHz || high > range->maximumHighHz
            || width < range->minimumWidthHz || width > range->maximumWidthHz) {
            return refusal("request.out_of_range", "passband is outside this mode's receive limits");
        }
        slice->noteReceiveFilterIntent();
        m_radio->backend()->requestSliceFilter(id, {low, high, SliceFilterRequest::Origin::Operator});
        return {};
    }

    std::optional<ProtocolError> setAudioGain(int id, int gain) override
    {
        if (const auto error = checkSlice(id, ReceiveOperation::AudioGain)) {
            return error;
        }
        if (gain < 0 || gain > 100) {
            return refusal("request.out_of_range", "receive gain must be 0..100");
        }
        m_radio->backend()->setSliceAudioGain(id, gain);
        return {};
    }

    std::optional<ProtocolError> setAudioMute(int id, bool muted) override
    {
        if (const auto error = checkSlice(id, ReceiveOperation::AudioMute)) {
            return error;
        }
        m_radio->backend()->setSliceAudioMute(id, muted);
        return {};
    }

    std::optional<ProtocolError> setAgcThreshold(int id, int threshold) override
    {
        if (const auto error = checkSlice(id, ReceiveOperation::AgcThreshold)) {
            return error;
        }
        if (threshold < 0 || threshold > 100) {
            return refusal("request.out_of_range", "AGC threshold is outside 0..100");
        }
        // The companion mode is the slice's current AGC mode, which the backend
        // echoed; only the threshold changes.
        SliceAgcRequest request;
        request.field = SliceAgcRequest::Field::Threshold;
        request.mode = m_radio->slice(id)->agcMode();
        request.threshold = threshold;
        m_radio->backend()->requestSliceAgc(id, request);
        return {};
    }

    std::optional<ProtocolError> setNoiseBlanker(int id, bool enabled) override
    {
        if (const auto error = checkSlice(id, ReceiveOperation::NoiseBlanker)) {
            return error;
        }
        const SliceModel* slice = m_radio->slice(id);
        SliceDspRequest request;
        request.feature = SliceDspRequest::Feature::Nb;
        request.field = SliceDspRequest::Field::Enabled;
        request.enabled = enabled;
        // The blanker's kind, level and fill ride along as the backend reported
        // them, so only on/off changes.
        request.kind = slice->nbKind();
        request.level = slice->nbLevel();
        request.fill = slice->nbFill();
        return dispatchDsp(id, request);
    }

    std::optional<ProtocolError> setNoiseReduction(int id, bool enabled) override
    {
        if (const auto error = checkSlice(id, ReceiveOperation::NoiseReduction)) {
            return error;
        }
        SliceDspRequest request;
        request.feature = SliceDspRequest::Feature::Nr;
        request.field = SliceDspRequest::Field::Enabled;
        request.enabled = enabled;
        request.level = m_radio->slice(id)->nrLevel();
        return dispatchDsp(id, request);
    }

    std::optional<ProtocolError> setRit(int id, bool enabled, int offsetHz) override
    {
        if (const auto error = checkSlice(id, ReceiveOperation::Rit)) {
            return error;
        }
        const int maximum = m_radio->backendCapabilities().receiveDspControl->maximumRitOffsetHz;
        // Turning RIT off is always allowed; the backend clamps the offset it keeps.
        if (enabled && (offsetHz < -maximum || offsetHz > maximum)) {
            return refusal("request.out_of_range", "RIT offset is outside the declared range");
        }
        // The desktop's route too (SliceModel::setRit): one intent, and a
        // backend that does not take it is a refusal, not a silent accept.
        const SliceControlRequest request{SliceControlRequest::Field::Rit,
                                          IncrementalTuning{enabled, offsetHz}};
        if (!request.valid()
            || m_radio->backend()->requestSliceControl(id, request) != ReceiveDispatch::Dispatched) {
            return refusal("capability.unavailable", "backend did not accept the RIT intent");
        }
        return {};
    }

    std::optional<ProtocolError> setPanRfGain(const QString& id, int gainDb) override
    {
        if (const auto error = checkPan(id, ReceiveOperation::PanRfGain)) {
            return error;
        }
        const ReceivePanRfGainControl range = *m_radio->backendCapabilities().receivePanRfGainControl;
        if (gainDb < range.minimumDb || gainDb > range.maximumDb
            || (gainDb - range.minimumDb) % range.stepDb != 0) {
            return refusal("request.out_of_range", "RF gain is outside the declared range or step");
        }
        m_radio->backend()->setPanRfGain(*m_radio->receiveControlPanId(id), gainDb);
        return {};
    }

    std::optional<ProtocolError> setPanCenter(const QString& id, qint64 hz) override
    {
        return setPan(id, hz, ReceiveOperation::PanCenter);
    }
    std::optional<ProtocolError> setPanBandwidth(const QString& id, qint64 hz) override
    {
        return setPan(id, hz, ReceiveOperation::PanBandwidth);
    }

    std::optional<ProtocolError> setCwPitch(int hz) override
    {
        const RadioCapabilities caps = m_radio ? m_radio->backendCapabilities() : RadioCapabilities{};
        if (const auto error = checkCwPitch(caps)) {
            return error;
        }
        const ReceiveCwPitchControl pitch = *caps.receiveCwPitchControl;
        if (hz < pitch.minimumHz || hz > pitch.maximumHz || (hz - pitch.minimumHz) % pitch.stepHz != 0) {
            return refusal("request.out_of_range", "CW pitch is outside the declared range or step");
        }
        m_radio->backend()->setCwPitch(hz);
        return {};
    }

private:
    std::optional<ProtocolError> dispatchDsp(int id, const SliceDspRequest& request)
    {
        if (!request.valid()
            || m_radio->backend()->requestSliceDsp(id, request) != ReceiveDispatch::Dispatched) {
            return refusal("capability.unavailable", "backend did not accept the DSP intent");
        }
        return {};
    }
    static ProtocolError refusal(const char* code, const char* message)
    {
        return ReceiveControlGuard::refusal(code, message);
    }
    std::optional<ProtocolError> checkSlice(int id, ReceiveOperation op) const
    {
        if (thread() != QThread::currentThread()) {
            return refusal("request.conflict", "receive target owning thread required");
        }
        if (const auto error = m_guard.checkSlice(id)) {
            return error;
        }
        const SliceModel* slice = m_radio->slice(id);
        const RadioCapabilities caps = m_radio->backendCapabilities();
        if (const auto error = m_guard.checkTransmit(caps)) {
            return error;
        }
        if (slice->externalReceiveReplacementActive() || slice->isDiversityChild()) {
            return refusal("capability.unavailable", "slice receive path is not independently controlled");
        }
        const auto& observed = slice->receiveObservation();
        bool supported = false;
        switch (op) {
        case ReceiveOperation::Mode:
            if (m_pendingModes.contains(slice)) {
                return refusal("request.conflict", "await mode readback before another mode intent");
            }
            supported = caps.receiveModeControl && known(caps.receiveModeControl->authority)
                && observed.mode && caps.receiveModeControl->modes.contains(*observed.mode);
            break;
        case ReceiveOperation::Filter:
            if (m_pendingModes.contains(slice)) {
                return refusal("request.conflict", "await mode readback before selecting a filter");
            }
            supported = observed.mode && observed.filterLowHz && observed.filterHighHz
                && *observed.filterLowHz < *observed.filterHighHz
                && filterMode(caps, *observed.mode);
            break;
        case ReceiveOperation::AudioGain:
        case ReceiveOperation::AudioMute:
            supported = caps.receiveAudioControl && known(caps.receiveAudioControl->authority)
                && (op == ReceiveOperation::AudioGain ? observed.gain.has_value() : observed.muted.has_value());
            break;
        case ReceiveOperation::AgcThreshold:
        case ReceiveOperation::NoiseBlanker:
        case ReceiveOperation::NoiseReduction:
        case ReceiveOperation::Rit: {
            // Which DSP features apply depends on the mode, so a pending mode
            // write holds these back like it holds back a filter.
            if (m_pendingModes.contains(slice)) {
                return refusal("request.conflict", "await mode readback before a mode-dependent intent");
            }
            using Feature = ReceiveDspControl::Feature;
            if (op == ReceiveOperation::AgcThreshold) {
                supported = dspSupports(caps, Feature::Agc, observed.mode) && observed.agcThreshold;
            } else if (op == ReceiveOperation::NoiseBlanker) {
                supported = dspSupports(caps, Feature::Nb, observed.mode) && observed.noiseBlanker;
            } else if (op == ReceiveOperation::NoiseReduction) {
                supported = dspSupports(caps, Feature::Nr, observed.mode) && observed.noiseReduction;
            } else {
                supported = dspSupports(caps, Feature::Rit, observed.mode)
                    && caps.receiveDspControl->maximumRitOffsetHz > 0
                    && observed.ritEnabled && observed.ritOffsetHz;
            }
            break;
        }
        default: break;
        }
        return supported ? std::nullopt : std::optional<ProtocolError>(
            refusal("capability.unavailable", "receive operation or observation unavailable"));
    }

    // The pitch must be declared and reported by this session's backend; the
    // model's own value is not an observation.
    std::optional<ProtocolError> checkCwPitch() const
    {
        return checkCwPitch(m_radio ? m_radio->backendCapabilities() : RadioCapabilities{});
    }
    std::optional<ProtocolError> checkCwPitch(const RadioCapabilities& caps) const
    {
        if (thread() != QThread::currentThread()) {
            return refusal("request.conflict", "receive target owning thread required");
        }
        if (const auto error = m_guard.checkTransmit(caps)) {
            return error;
        }
        const auto& pitch = caps.receiveCwPitchControl;
        if (!pitch || pitch->minimumHz <= 0 || pitch->maximumHz < pitch->minimumHz || pitch->stepHz <= 0
            || !m_radio->transmitModel().reportedCwPitch()) {
            return refusal("capability.unavailable", "CW pitch control or observation unavailable");
        }
        return {};
    }

    std::optional<ProtocolError> checkPan(const QString& id, ReceiveOperation op) const
    {
        if (thread() != QThread::currentThread() || !m_guard.ready()) {
            return refusal("request.conflict", "radio connection is not ready");
        }
        if (!m_radio->receiveControlPanId(id)) {
            return refusal("resource.not_found", "owned panadapter unavailable");
        }
        const RadioCapabilities caps = m_radio->backendCapabilities();
        if (const auto error = m_guard.checkTransmit(caps)) {
            return error;
        }
        const PanadapterModel* pan = m_radio->panadapter(id);
        if (op == ReceiveOperation::PanRfGain) {
            const auto& gain = caps.receivePanRfGainControl;
            if (!gain || !known(gain->authority) || gain->stepDb <= 0
                || gain->maximumDb < gain->minimumDb || !pan->reportedRfGain()) {
                return refusal("capability.unavailable", "RF gain control or observation unavailable");
            }
            return {};
        }
        const auto& range = op == ReceiveOperation::PanCenter
            ? caps.receivePanCenterControl : caps.receivePanBandwidthControl;
        if (!validRange(range) || !pan->reportedCenterHz() || !pan->reportedBandwidthHz()) {
            return refusal("capability.unavailable", "pan operation or geometry observation unavailable");
        }
        return {};
    }

    std::optional<ProtocolError> setPan(const QString& id, qint64 hz, ReceiveOperation op)
    {
        if (const auto error = checkPan(id, op)) {
            return error;
        }
        const RadioCapabilities caps = m_radio->backendCapabilities();
        const auto& range = op == ReceiveOperation::PanCenter
            ? caps.receivePanCenterControl : caps.receivePanBandwidthControl;
        const PanadapterModel* pan = m_radio->panadapter(id);
        const qint64 center = op == ReceiveOperation::PanCenter ? hz : *pan->reportedCenterHz();
        const qint64 bandwidth = op == ReceiveOperation::PanBandwidth ? hz : *pan->reportedBandwidthHz();
        if (hz < range->minimumHz || hz > range->maximumHz || bandwidth <= 0
            || center <= 0 || center < (bandwidth + 1) / 2) {
            return refusal("request.out_of_range", "pan geometry is outside supported receive limits");
        }
        const QString backendId = *m_radio->receiveControlPanId(id);
        if (op == ReceiveOperation::PanCenter) {
            m_radio->backend()->setPanCenter(backendId, double(hz), IRadioBackend::PanCenterIntent::Range);
        } else {
            m_radio->backend()->setPanBandwidth(backendId, double(hz));
        }
        return {};
    }

    QPointer<RadioModel> m_radio;
    ReceiveControlGuard m_guard;
    QSet<SliceModel*> m_watchedSlices;
    QHash<const SliceModel*, QString> m_pendingModes;
};
} // namespace

std::unique_ptr<ReceiveControlTarget> makeModelReceiveControlTarget(
    RadioModel* radio, RadioConnectionTarget* connection)
{
    if (!ReceiveControlGuard::canBind(radio, connection)) {
        return {};
    }
    return std::make_unique<ModelReceiveControlTarget>(radio, connection);
}
} // namespace AetherSDR::control
