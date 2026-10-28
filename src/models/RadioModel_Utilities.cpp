#include "RadioModel.h"
#include "core/AppSettings.h"

#include <QPointer>
#include <QList>
#include <QString>
#include <QSysInfo>
#include <QThread>

namespace AetherSDR {

ReceiveDispatch RadioModel::requestSpot(const SpotRequest& request, PanadapterModel* pan,
                                       std::optional<RadioControlSession> expectedSession,
                                       std::optional<quint64> expectedEpoch)
{
    if (QThread::currentThread() != thread()) { return ReceiveDispatch::Unsupported; }
    const RadioControlSession session = controlSession();
    if ((expectedSession && *expectedSession != session)
        || (expectedEpoch && *expectedEpoch != m_spotRequestEpoch)) { return ReceiveDispatch::Unsupported; }
    if (!request.valid()) { return ReceiveDispatch::Unsupported; }
    if (request.action == SpotAction::Clear) { ++m_spotRequestEpoch; m_spotRequests.clear(); }
    const RadioCapabilities caps = backendCapabilities();
    bool available = request.valid() && m_backend && controlSessionCurrent(session)
        && caps.spotControl && caps.spotControl->supports(request.action);
    if (request.action == SpotAction::Remove || request.action == SpotAction::Trigger) {
        const auto entry = m_spotModel.spots().constFind(request.index);
        available = available && entry != m_spotModel.spots().cend()
            && entry->source != QLatin1String("Memory")
            && entry->source.compare(QLatin1String("TCI"), Qt::CaseInsensitive) != 0;
    }
    if (request.action == SpotAction::Trigger) {
        // Firmware owns radio-spot lock/SWR admission. TCI's gate is separate.
        available = available && pan && panadapter(pan->panId()) == pan;
    }
    const QPointer<RadioModel> alive(this);
    const QString panId = available && request.action == SpotAction::Trigger ? pan->panId() : QString();
    const ReceiveDispatch result = available
        ? m_backend->requestSpot(request, panId) : ReceiveDispatch::Unsupported;
    if (alive && session == controlSession()) {
        reportReceiveDispatch(result, QStringLiteral("radio spot"));
    }
    return result;
}

void RadioModel::queueSpot(const SpotRequest& request)
{
    if (QThread::currentThread() != thread() || request.action != SpotAction::Add || !request.valid()
        || !controlSessionCurrent(controlSession())) { return; }
    m_spotRequests.append(request, controlSession(), m_spotRequestEpoch);
}

void RadioModel::clearQueuedSpots()
{
    if (QThread::currentThread() == thread()) { m_spotRequests.clear(); }
}

void RadioModel::flushQueuedSpots(int limit)
{
    if (QThread::currentThread() != thread() || m_flushingSpotRequests) { return; }
    if (!controlSessionCurrent(controlSession())) { m_spotRequests.clear(); return; }
    const QList<QueuedSpotRequest> batch = m_spotRequests.takeBatch(limit, controlSession(), m_spotRequestEpoch);
    const QPointer<RadioModel> alive(this);
    m_flushingSpotRequests = true;
    for (const QueuedSpotRequest& entry : batch) {
        requestSpot(entry.request, nullptr, entry.session, entry.epoch);
        if (!alive) { return; }
    }
    m_flushingSpotRequests = false;
}

ReceiveDispatch RadioModel::requestPanControl(PanadapterModel* pan, const PanControlRequest& request)
{
    if (QThread::currentThread() != thread()) { return ReceiveDispatch::Unsupported; }
    const RadioControlSession session = controlSession();
    const RadioCapabilities caps = backendCapabilities();
    const bool declared = request.field == PanControlField::NoiseFloorRange
        ? panNoiseFloorRangeUnavailableReason().isEmpty()
        : caps.panControl && caps.panControl->supports(request.field)
            && (request.field == PanControlField::ReceiveAntenna || caps.panZoomModes);
    const bool available = m_backend && controlSessionCurrent(session) && request.valid()
        && pan && panadapter(pan->panId()) == pan && declared;
    const QPointer<RadioModel> alive(this);
    const ReceiveDispatch result = available
        ? m_backend->requestPanControl(pan->panId(), request) : ReceiveDispatch::Unsupported;
    if (alive && session == controlSession()) {
        reportReceiveDispatch(result, QStringLiteral("pan control"));
    }
    return result;
}

ReceiveDispatch RadioModel::requestSlicePreset(SliceModel* target, const SlicePresetRequest& request)
{
    if (QThread::currentThread() != thread()) { return ReceiveDispatch::Unsupported; }
    const RadioControlSession session = controlSession();
    const QPointer<RadioModel> alive(this);
    const QPointer<SliceModel> original(target);
    if (!m_backend || !controlSessionCurrent(session) || !request.valid()
        || !original || slice(original->sliceId()) != original) { return ReceiveDispatch::Unsupported; }
    SlicePresetRequest effective = request;
    const std::optional<SlicePresetControl> control = backendCapabilities().slicePresetControl;
    if ((request.direction && (!control || !control->supports(SlicePresetField::RepeaterOffset)))
        || (request.toneMode && (!control || !control->supports(SlicePresetField::ToneMode)))
        || (request.toneHz && (!control || !control->supports(SlicePresetField::ToneValue)))) {
        reportReceiveDispatch(ReceiveDispatch::Unsupported, QStringLiteral("slice preset"));
        return ReceiveDispatch::Unsupported;
    }
    if (effective.stepHz && (!control || !control->supports(SlicePresetField::Step))
        && applyClientOwnedSliceStep(original->sliceId(), *effective.stepHz)) {
        effective.stepHz.reset();
    }
    if (!alive || !original || !controlSessionCurrent(session)
        || slice(original->sliceId()) != original) { return ReceiveDispatch::Unsupported; }
    const bool supported = control
        && (!effective.stepHz || control->supports(SlicePresetField::Step))
        && (!effective.direction || control->supports(SlicePresetField::RepeaterOffset))
        && (!effective.toneMode || control->supports(SlicePresetField::ToneMode))
        && (!effective.toneHz || control->supports(SlicePresetField::ToneValue));
    const ReceiveDispatch result = effective.empty() ? ReceiveDispatch::LocalOnly
        : supported ? m_backend->requestSlicePreset(original->sliceId(), effective) : ReceiveDispatch::Unsupported;
    if (alive && session == controlSession()) {
        reportReceiveDispatch(result, QStringLiteral("slice preset"));
    }
    return result;
}

std::optional<RadioStationOverride> RadioModel::requestTemporaryStation(const QString& name)
{
    if (QThread::currentThread() != thread()) { return {}; }
    const RadioSetupRequest request{RadioSetupField::ClientStation, name};
    const RadioControlSession session = controlSession();
    const RadioCapabilities caps = backendCapabilities();
    if (!request.valid() || !m_backend || !controlSessionCurrent(session)
        || !caps.radioSetupControl || !caps.radioSetupControl->supports(request.field)) { return {}; }
    const QPointer<RadioModel> alive(this);
    const quint64 revision = ++m_stationIntentRevision;
    const RadioControlFence current = [alive, session, revision] {
        return alive && alive->controlSessionCurrent(session) && alive->m_stationIntentRevision == revision;
    };
    const ReceiveDispatch result = m_backend->requestRadioSetup(request, {}, current);
    if (result != ReceiveDispatch::Dispatched || !current()) { return {}; }
    m_stationOverrideOwner = revision;
    return RadioStationOverride{this, session, revision};
}

bool RadioModel::restoreTemporaryStation(const RadioStationOverride& owner)
{
    if (QThread::currentThread() != thread() || owner.model != this
        || !m_backend || !controlSessionCurrent(owner.session)
        || !owner.revision || m_stationOverrideOwner != owner.revision) { return false; }
    QString name = AppSettings::instance().value("StationName", "").toString();
    if (name.isEmpty()) { name = QSysInfo::machineHostName(); }
    const RadioSetupRequest request{RadioSetupField::ClientStation, name};
    if (!request.valid()) { return false; }
    const QPointer<RadioModel> alive(this);
    const quint64 revision = ++m_stationIntentRevision;
    const RadioControlFence current = [alive, owner, revision] {
        return alive && alive->controlSessionCurrent(owner.session)
            && alive->m_stationOverrideOwner == owner.revision && alive->m_stationIntentRevision == revision;
    };
    const ReceiveDispatch result = m_backend->requestRadioSetup(request, {}, current);
    if (result != ReceiveDispatch::Dispatched || !current()) { return false; }
    m_stationOverrideOwner = 0;
    return true;
}

} // namespace AetherSDR
