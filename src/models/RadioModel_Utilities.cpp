#include "RadioModel.h"
#include "core/AppSettings.h"
#include "core/BandStackSettings.h"
#include "core/MemoryFieldValues.h"

#include <algorithm>
#include <cmath>

#include <QPointer>
#include <QList>
#include <QString>
#include <QSysInfo>
#include <QThread>

namespace AetherSDR {

void RadioModel::recallBandStack(SliceModel* target, BandStackEntry entry,
                                 const std::function<void(SliceModel*, double)>& tune)
{
    if (QThread::currentThread() != thread() || !tune || !target
        || !std::isfinite(entry.frequencyMhz) || entry.frequencyMhz <= 0.0) { return; }
    // Client-side digital modes need not appear in firmware's mode list.
    const bool modeToken = !entry.mode.isEmpty() && entry.mode.size() <= 32
        && std::all_of(entry.mode.cbegin(), entry.mode.cend(), [](QChar c) {
            return (c >= QLatin1Char('A') && c <= QLatin1Char('Z'))
                || (c >= QLatin1Char('0') && c <= QLatin1Char('9'))
                || c == QLatin1Char('_') || c == QLatin1Char('-');
        });
    if (!modeToken || (!MemoryFields::isStorableMode(entry.mode)
        && !target->modeList().contains(entry.mode))) { return; }
    const QPointer<RadioModel> owner(this);
    const QPointer<SliceModel> original(target);
    const QString panId = target->panId();
    const QPointer<PanadapterModel> pan(panadapter(panId));
    const bool hadPan = !pan.isNull();
    const quint64 panOwnership = pan ? pan->ownershipRevision() : 0;
    const bool external = target->externalReceiveReplacementActive();
    const RadioControlSession session = controlSession();
    const quint64 generation = target->receiveSessionGeneration();
    const quint64 revision = ++m_bandStackRecallRevision;
    const auto current = [owner, original, pan, hadPan, panId, panOwnership, external,
                          session, generation, revision] {
        return owner && original && owner->controlSessionCurrent(session)
            && !owner->m_stagingReceiveModels && owner->m_backend
            && owner->m_bandStackRecallRevision == revision
            && original->receiveSessionGeneration() == generation
            && original->externalReceiveReplacementActive() == external
            && owner->slice(original->sliceId()) == original
            && !owner->isSlotForeign(original->sliceId()) && original->panId() == panId
            && (!hadPan || (pan && owner->panadapter(panId) == pan
                && pan->ownershipRevision() == panOwnership));
    };
    // Every setter can synchronously replace the session or start a new recall.
    // The value-copy above survives bookmark-list reloads from those callbacks.
    const auto apply = [&current](const auto& action) {
        if (!current()) { return false; }
        action();
        return current();
    };
    if (!apply([&] {
        if (original->mode() != entry.mode) { original->setMode(entry.mode); }
    })) { return; }
    if (!apply([&] { tune(original, entry.frequencyMhz); })) { return; }
    if (!apply([&] {
        if (entry.filterLow != 0 || entry.filterHigh != 0) {
            original->setFilterWidth(entry.filterLow, entry.filterHigh);
        }
    })) { return; }
    // Antenna requests leave the displayed ports to backend observations.
    // A bookmark contains physical port identifiers, never presentation aliases.
    if (!apply([&] {
        if (!entry.rxAntenna.isEmpty() && entry.rxAntenna != original->rxAntenna()
            && original->rxAntennaList().contains(entry.rxAntenna)) {
            const ReceiveDispatch result = m_backend->requestSliceRxAntenna(original->sliceId(), entry.rxAntenna);
            if (current()) { reportReceiveDispatch(result, QStringLiteral("bookmark RX antenna"), false); }
        }
    })) { return; }
    if (!apply([&] {
        if (!entry.txAntenna.isEmpty() && entry.txAntenna != original->txAntenna()
            && original->txAntennaList().contains(entry.txAntenna)) {
            const ReceiveDispatch result = m_backend->requestSliceControl(original->sliceId(),
                {SliceControlRequest::Field::TxAntenna, entry.txAntenna});
            if (current()) { reportReceiveDispatch(result, QStringLiteral("bookmark TX antenna"), false); }
        }
    })) { return; }
    const auto recallAgc = [&] {
        if (!apply([&] {
            const SliceAgcRequest request{SliceAgcRequest::Field::Mode, entry.agcMode,
                original->agcThreshold(), original->agcOffLevel(), SliceAgcRequest::Origin::Recall};
            if (request.valid() && entry.agcMode != original->agcMode()) {
                if (original->externalReceiveReplacementActive()) {
                    m_backend->requestSliceAgc(original->sliceId(), request);
                } else {
                    original->recallAgcMode(entry.agcMode);
                }
            }
        })) { return false; }
        return apply([&] {
            const SliceAgcRequest request{SliceAgcRequest::Field::Threshold, original->agcMode(),
                entry.agcThreshold, original->agcOffLevel(), SliceAgcRequest::Origin::Recall};
            const bool manual = !backendCapabilities().autoAgcThresholdControl
                || original->agcThresholdMode() == AgcThresholdMode::Manual;
            if (request.valid() && manual && !original->automaticAgcThresholdActiveOrPending()
                && entry.agcThreshold != original->agcThreshold()) {
                if (original->externalReceiveReplacementActive()) {
                    m_backend->requestSliceAgc(original->sliceId(), request);
                } else {
                    original->recallAgcThreshold(entry.agcThreshold);
                }
            }
        });
    };
    // Preserve the existing ordering: the radio AGC behind Kiwi precedes audio;
    // ordinary receive AGC follows it, after the mode's own AGC defaults.
    if (external && !recallAgc()) { return; }
    if (!apply([&] {
        if (entry.audioGain >= 0 && entry.audioGain <= 100
            && static_cast<int>(original->audioGain()) != entry.audioGain) {
            original->setAudioGain(static_cast<float>(entry.audioGain));
        }
    })) { return; }
    if (!external && !recallAgc()) { return; }
    if (!apply([&] { if (entry.nbOn != original->nbOn()) { original->setNb(entry.nbOn); } })) { return; }
    if (!apply([&] {
        if (entry.nbLevel >= 0 && entry.nbLevel <= 100 && entry.nbLevel != original->nbLevel()) {
            original->setNbLevel(entry.nbLevel);
        }
    })) { return; }
    if (radioSideNoiseReductionAvailable()) {
        if (!apply([&] { if (entry.nrOn != original->nrOn()) { original->setNr(entry.nrOn); } })) { return; }
        if (!apply([&] {
            if (entry.nrLevel >= 0 && entry.nrLevel <= 100 && entry.nrLevel != original->nrLevel()) {
                original->setNrLevel(entry.nrLevel);
            }
        })) { return; }
    }
    if (!pan) { return; }
    if (!apply([&] {
        if (entry.wnbOn != pan->wnbActive()) {
            requestPanDisplay(pan, {PanDisplayField::Wnb, entry.wnbOn}, {}, false);
        }
    })) { return; }
    apply([&] {
        if (entry.wnbLevel >= 0 && entry.wnbLevel <= 100 && entry.wnbLevel != pan->wnbLevel()) {
            requestPanDisplay(pan, {PanDisplayField::WnbLevel, entry.wnbLevel}, {}, false);
        }
    });
}

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

ReceiveDispatch RadioModel::requestSlicePreset(SliceModel* target, const SlicePresetRequest& request,
                                               std::optional<RadioControlSession> expectedSession)
{
    if (QThread::currentThread() != thread()) { return ReceiveDispatch::Unsupported; }
    const RadioControlSession session = controlSession();
    const QPointer<RadioModel> alive(this);
    const QPointer<SliceModel> original(target);
    if (!m_backend || !controlSessionCurrent(session) || !request.valid() || m_stagingReceiveModels
        || !original || slice(original->sliceId()) != original
        || isSlotForeign(original->sliceId())) { return ReceiveDispatch::Unsupported; }
    if (expectedSession && *expectedSession != session) { return ReceiveDispatch::Unsupported; }
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
