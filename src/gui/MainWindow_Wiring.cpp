// MainWindow_Wiring.cpp — per-object signal wiring for MainWindow.
//
// Part of the #3351 monolith decomposition (Phase 1d). Holds the wiring
// methods that connect each dynamically-created radio-facing object to the
// UI — the code that runs when a panadapter, slice, VFO, or DSP widget
// comes into existence:
//
//   • wirePanadapter(): per-pan signal wiring (~70 connects per pan)
//   • wireVfoWidget(): per-VFO wiring
//   • wireAetherDspWidget(): DSP-panel wiring
//   • onSliceAdded() / onSliceRemoved(): slice lifecycle
//   • panFollowVfo() + revealFrequencyIfNeeded(): pan-follows-tuning policy
//
// This family is the most RadioSession-adjacent code in the split: when the
// session aggregate lands (#3351 follow-up / #3445), these methods take a
// session pointer instead of reaching for the singular m_radioModel.
//
// Pure code motion from MainWindow.cpp — same class, no header changes.

#include "MainWindow.h"

#include "AetherDspWidget.h"
#include "VoiceModeGate.h"   // isCwMode() — one CW-mode list, not thirteen
#include "BandRecallSliceSelectionPolicy.h"
#include "models/BandPlanManager.h"
#include "DisplayStatusGate.h"       // #4261 adaptive-throttle echo gate
#include "Ax25HfPacketDecodeDialog.h"
#include "AppletPanel.h"
#include "DbmRangeTransition.h"
#include "MainWindowHelpers.h"
#include "OwnedSingleShotTimer.h"
#include "PanRecenterPolicy.h"
#include "PanadapterApplet.h"
#include "PanadapterMessageOverlay.h"
#include "PanadapterStack.h"
#include "RadioSetupDialog.h"
#include "GpsLocationDialog.h"
#include "RxApplet.h"
#include "AmpApplet.h"
#include "AcomApplet.h"
#include "SpeApplet.h"
#include "VkampApplet.h"
#include "HealthApplet.h"
#include "ImageFileDialog.h"
#include "MeterApplet.h"
#include "ProfileSwitcherApplet.h"
#include "SMeterWidget.h"
#include "TunerApplet.h"
#include "TxApplet.h"
#include "core/PeripheralSettings.h"
#include "core/KiwiSdrManager.h"
#include "core/KiwiSdrProtocol.h"
#include "SliceLabel.h"
#include "SpectrumOverlayMenu.h"
#include "SpectrumWidget.h"
#include "core/AdaptiveFilterEngine.h"
#include "VfoWidget.h"
#include "core/BandStackSettings.h"
#include "core/AppSettings.h"
#include "core/SpotCommandPolicy.h"
#include "core/WaterfallRate.h"
#include "core/SpotModeResolver.h"
#include "core/LogManager.h"
#ifdef HAVE_RADE
#include "core/RADEEngine.h"
#endif
#include "models/ProfileLoadCommand.h"
#include "models/RadioModel.h"
#include "models/SliceLinkPolicy.h"
#include "models/SliceModel.h"
#include "models/Nr2SettingsModel.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QTimer>

#include <algorithm>
#include <memory>
#include <cmath>

namespace AetherSDR {

namespace {

// Pan-Follows-VFO edge margins, as a fraction of the visible span. Because
// frequency now maps linearly across contentWidth() (#3482), these are a
// constant fraction of the panadapter *width* in pixels at every zoom — trigger
// is the gap between the VFO flag's outer edge and the pan edge at which the pan
// starts to scroll; settle is where the flag lands after it does. settle >
// trigger by ~1.5% supplies the hysteresis that stops a held arrow-key tune from
// re-firing every step. Tightened from 0.05/0.06 so the signal can tune much
// closer to the edge before the pan shifts (#3482 follow-up); the flag flips
// inward at the content edge so it never clips the tape at this margin.
constexpr double kIncrementalTriggerEdgeMarginFrac = 0.02;
constexpr double kIncrementalSettleEdgeMarginFrac = 0.035;
constexpr double kRevealComfortEdgeMarginFrac = 0.18;
constexpr double kSpectrumClickEdgeMarginFrac = 0.05;

constexpr double kMemoryRevealTargetToleranceMhz = 0.000001;
constexpr int kPanDimensionDecoderFallbackMs = 650;
constexpr int kKiwiGestureViewUpdateMs = 100;
constexpr qint64 kProfileLoadDimensionSettleMs = kPanDimensionDecoderFallbackMs + 100;
constexpr qint64 kCenterLockTuneEchoHoldMs = 550;
constexpr auto kCenterLockSettingsKey = "CenterLock";
constexpr int kCenterLockSettingsVersion = 1;

int currentAutoSqlMarginDb()
{
    return std::clamp(
        AppSettings::instance().value("AutoSqlMarginDb", "10").toInt(), 5, 20);
}

int kiwiSdrSquelchMarginDbForSliceLevel(const SliceModel* slice, int level)
{
    if (slice && slice->externalReceiveAutoSquelchOn()) {
        return currentAutoSqlMarginDb();
    }
    return KiwiSdrProtocol::squelchSliderLevelToMarginDb(level);
}

bool memoryRevealTargetMatches(double actualMhz, double targetMhz)
{
    return targetMhz <= 0.0
        || std::abs(actualMhz - targetMhz) <= kMemoryRevealTargetToleranceMhz;
}

bool dbmRangeLooksPlausible(float minDbm, float maxDbm)
{
    constexpr float kMinAllowedDbm = -180.0f;
    constexpr float kMaxAllowedDbm = 80.0f;
    constexpr float kMinRangeDb = 10.0f;
    constexpr float kMaxRangeDb = 180.0f;

    if (!std::isfinite(minDbm) || !std::isfinite(maxDbm)) {
        return false;
    }

    const float rangeDb = maxDbm - minDbm;
    return minDbm >= kMinAllowedDbm
        && maxDbm <= kMaxAllowedDbm
        && rangeDb >= kMinRangeDb
        && rangeDb <= kMaxRangeDb;
}

SliceModel* kiwiAssignedSliceForPan(const RadioModel& radioModel,
                                    const KiwiSdrManager* manager,
                                    const QString& panId)
{
    if (!manager || panId.isEmpty()) {
        return nullptr;
    }

    QVector<SliceModel*> candidates;
    for (SliceModel* slice : radioModel.slices()) {
        if (!slice || slice->panId() != panId
            || !radioModel.sliceMayBelongToUs(slice->sliceId())
            || manager->assignedProfileForSlice(slice->sliceId()).isEmpty()) {
            continue;
        }
        if (slice->isDiversityParent()) {
            return slice;
        }
        candidates.append(slice);
    }
    if (candidates.isEmpty()) {
        return nullptr;
    }
    for (SliceModel* slice : candidates) {
        if (!slice->diversity()) {
            return slice;
        }
    }
    return candidates.first();
}

// Pan-follow tuning internals — moved with their only callers from
// MainWindow.cpp's anonymous namespace (#3351 Phase 1d).
constexpr int kPanFollowAnimationDurationMs = 110;

double quantizeIncrementalFollowDelta(double overshootMhz, double stepMhz)
{
    if (overshootMhz <= 0.0)
        return 0.0;
    if (stepMhz <= 0.0)
        return overshootMhz;
    return std::ceil((overshootMhz - 1e-12) / stepMhz) * stepMhz;
}

} // namespace

void MainWindow::showGpsLocationDialog()
{
    showOrRaisePersistent(m_gpsLocationDialog, &m_radioModel);
}

void MainWindow::noteBandRecallForPan(const QString& panId)
{
    if (panId.isEmpty()) {
        return;
    }

    // Flex band persistence destroys and rebuilds this pan's slices. Snapshot
    // the locked receiver and our pre-recall topology before sending the band
    // command. Restoration is deferred until the full grace window elapses so
    // slice status arrival order can never choose the receiver.
    m_kiwiRebind.noteBandRecall(panId);
    // Open the radio-driven-selection window on the same edge. It is armed
    // here and rolled back by panBandDispatchFailed, so unlike the markers
    // around it, it never opens for a band write that never reached the wire.
    m_bandRecallSelection.arm(panId, QDateTime::currentMSecsSinceEpoch());
    // Stamp this recall so the grace timer below only clears the Kiwi marker if
    // it is still the latest recall for this pan. Without it, an overlapping
    // recall (band button + memory spot / tune, now all routed here) would have
    // the first timer clear the marker mid-rebuild of the second — the Kiwi
    // half would lose the generation-safety the Center Lock half already has.
    const quint64 bandRecallGeneration = ++m_bandRecallGeneration;
    m_bandRecallGenerationByPan.insert(panId, bandRecallGeneration);
    const int lockedSliceId = centerLockSliceForPan(panId);
    SliceModel* lockedSlice = m_radioModel.slice(lockedSliceId);
    if (lockedSlice
        && (lockedSlice->panId() != panId
            || !m_radioModel.sliceMayBelongToUs(lockedSliceId))) {
        lockedSlice = nullptr;
    }

    int ownedSliceCount = 0;
    for (SliceModel* slice : m_radioModel.slices()) {
        if (slice && slice->panId() == panId
            && m_radioModel.sliceMayBelongToUs(slice->sliceId())) {
            ++ownedSliceCount;
        }
    }

    const quint64 centerLockGeneration = m_centerLockRebind.noteBandRecall(
        panId,
        lockedSlice ? lockedSlice->sliceId() : -1,
        lockedSlice ? lockedSlice->letter() : QString(),
        ownedSliceCount);
    QTimer::singleShot(kBandRecallRecreateGraceMs, this,
                       [this, panId, centerLockGeneration, bandRecallGeneration]() {
        // Only the latest recall for this pan clears the Kiwi band-recall
        // marker; a superseding recall now owns the window, so an older timer
        // must not clear it out from under the in-flight rebuild.
        if (m_bandRecallGenerationByPan.value(panId) == bandRecallGeneration) {
            m_kiwiRebind.clearBandRecall(panId);
            m_bandRecallGenerationByPan.remove(panId);
        }
        if (centerLockGeneration == 0) {
            return;
        }

        QVector<CenterLockRebindTracker::SliceIdentity> sliceIdentities;
        for (SliceModel* slice : m_radioModel.slices()) {
            if (!slice) {
                continue;
            }
            sliceIdentities.append({
                slice->panId(),
                slice->sliceId(),
                slice->letter(),
                m_radioModel.sliceMayBelongToUs(slice->sliceId()),
            });
        }

        const CenterLockRebindTracker::Resolution resolution =
            m_centerLockRebind.resolveAfterGrace(
                panId, centerLockGeneration, sliceIdentities);
        if (resolution.kind
            == CenterLockRebindTracker::Resolution::Kind::NoAction) {
            return;
        }

        if (resolution.kind
            == CenterLockRebindTracker::Resolution::Kind::Restore) {
            SliceModel* replacement = m_radioModel.slice(resolution.sliceId);
            if (replacement && replacement->panId() == panId
                && m_radioModel.sliceMayBelongToUs(replacement->sliceId())) {
                // persist=true self-persists and cancels any pending recall (a
                // no-op here — resolveAfterGrace already consumed it).
                setCenterLockForPan(panId, replacement->sliceId(), true, true);
                if (centerLockActiveForSlice(replacement)) {
                    return;
                }
            }
            // Identified a slice but couldn't re-lock it: fall through and
            // report it as missing (not ambiguous — the tracker WAS sure).
        }

        clearCenterLockForPan(panId, true);
        showCenterLockReleaseNotification(
            panId,
            resolution.kind
                    == CenterLockRebindTracker::Resolution::Kind::ReleaseAmbiguous
                ? tr("The band change restored a different slice layout, so "
                     "the locked slice could not be identified safely.")
                : tr("The band change did not restore the locked slice."));
    });
}

void MainWindow::selectSliceFromRadioState(
    SliceModel* slice,
    RadioSliceSelectionSource source)
{
    if (!slice) {
        return;
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const bool bandRecallInFlight =
        m_bandRecallSelection.isActive(slice->panId(), nowMs);
    const RadioSliceSelectionDecision decision =
        radioSliceSelectionDecision(bandRecallInFlight, source);

    if (bandRecallInFlight) {
        // A transient activation IS the radio still rebuilding this pan: hold
        // the window open past it (bounded), so a rebuild slower than the base
        // grace period can't land its last old-band activation just outside.
        m_bandRecallSelection.refresh(slice->panId(), nowMs);
        qCDebug(lcProtocol).noquote()
            << "MainWindow: band recall suppressing slice reveal"
            << "pan=" << slice->panId()
            << "slice=" << slice->sliceId()
            << "source="
            << (source == RadioSliceSelectionSource::ActiveStatus
                    ? "active-status" : "topology-fallback");
    }

    const bool wasUpdatingFromModel = m_updatingFromModel;
    m_updatingFromModel =
        wasUpdatingFromModel || decision.suppressActiveCommand;
    setActiveSliceInternal(slice->sliceId(), decision.revealOffscreen);
    m_updatingFromModel = wasUpdatingFromModel;
}

void MainWindow::showCenterLockReleaseNotification(const QString& panId,
                                                    const QString& detail)
{
    if (!m_panStack) {
        return;
    }
    SpectrumWidget* sw = m_panStack->spectrum(panId);
    if (!sw) {
        return;
    }

    PanadapterOverlayMessage message;
    message.id = QStringLiteral("center-lock.band-recall-release");
    message.title = tr("Center Lock released");
    message.detail = detail;
    message.timeoutMs = 5000;
    message.dismissible = true;
    message.tone = PanadapterOverlayMessageTone::Warning;
    sw->upsertOverlayMessage(std::move(message));
}

int MainWindow::centerLockSliceForPan(const QString& panId) const
{
    return m_centerLockSliceByPan.value(panId, -1);
}

bool MainWindow::centerLockActiveForSlice(const SliceModel* slice) const
{
    return slice
        && !slice->panId().isEmpty()
        && centerLockSliceForPan(slice->panId()) == slice->sliceId();
}

QString MainWindow::centerLockRadioKey() const
{
    // Persistence identity (#3854 review): radio serial ALONE is shared by
    // every client instance on this machine (a real MultiFlex configuration),
    // so one instance's saved lock intent would engage in another instance —
    // slice letters are per-client — and either instance's unlock would
    // clobber the other's saved intent (the save rewrites the whole blob).
    // Scope the key by this client's station identity (StationName setting,
    // hostname fallback — the same identity we register with `client
    // station`). The blob treats keys as opaque, so no format migration.
    const QString serial = m_radioModel.serial().trimmed();
    if (serial.isEmpty()) {
        return QString();
    }
    return serial + QLatin1Char('/') + m_radioModel.ourStationName().trimmed();
}

void MainWindow::loadCenterLockSettings()
{
    m_centerLockSliceLetterByRadioPanIndex.clear();

    const QString raw = AppSettings::instance()
        .value(kCenterLockSettingsKey, QString()).toString().trimmed();
    if (raw.isEmpty()) {
        return;
    }

    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(raw.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        qCWarning(lcGui).noquote()
            << "MainWindow: ignoring invalid CenterLock settings:"
            << error.errorString();
        return;
    }

    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("version")).toInt() != kCenterLockSettingsVersion) {
        qCWarning(lcGui) << "MainWindow: ignoring unsupported CenterLock settings version";
        return;
    }

    const QJsonObject radios = root.value(QStringLiteral("radios")).toObject();
    for (auto radioIt = radios.constBegin(); radioIt != radios.constEnd(); ++radioIt) {
        const QString serial = radioIt.key().trimmed();
        if (serial.isEmpty() || !radioIt.value().isObject()) {
            continue;
        }

        QHash<int, QString> targets;
        const QJsonObject panSlots = radioIt.value().toObject();
        for (auto slotIt = panSlots.constBegin(); slotIt != panSlots.constEnd(); ++slotIt) {
            bool indexOk = false;
            const int panIndex = slotIt.key().toInt(&indexOk);
            const QString sliceLetter = slotIt.value().toString().trimmed().toUpper();
            if (indexOk && panIndex >= 0 && !sliceLetter.isEmpty()) {
                targets.insert(panIndex, sliceLetter);
            }
        }
        if (!targets.isEmpty()) {
            m_centerLockSliceLetterByRadioPanIndex.insert(serial, targets);
        }
    }
}

void MainWindow::saveCenterLockSettings() const
{
    QJsonObject radios;
    QStringList serials = m_centerLockSliceLetterByRadioPanIndex.keys();
    std::sort(serials.begin(), serials.end());
    for (const QString& serial : serials) {
        const QHash<int, QString>& targets =
            m_centerLockSliceLetterByRadioPanIndex.value(serial);
        QList<int> panIndices = targets.keys();
        std::sort(panIndices.begin(), panIndices.end());

        QJsonObject panSlots;
        for (int panIndex : panIndices) {
            const QString sliceLetter = targets.value(panIndex).trimmed().toUpper();
            if (!sliceLetter.isEmpty()) {
                panSlots.insert(QString::number(panIndex), sliceLetter);
            }
        }
        if (!panSlots.isEmpty()) {
            radios.insert(serial, panSlots);
        }
    }

    auto& settings = AppSettings::instance();
    if (radios.isEmpty()) {
        settings.remove(kCenterLockSettingsKey);
    } else {
        QJsonObject root;
        root.insert(QStringLiteral("version"), kCenterLockSettingsVersion);
        root.insert(QStringLiteral("radios"), radios);
        settings.setValue(
            kCenterLockSettingsKey,
            QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
    }
    settings.save();
}

void MainWindow::persistCenterLockForSlice(const SliceModel* slice)
{
    if (!slice || slice->panId().isEmpty() || !m_panStack) {
        return;
    }

    const QString radioKey = centerLockRadioKey();
    SpectrumWidget* sw = m_panStack->spectrum(slice->panId());
    const QString sliceLetter = slice->letter().trimmed().toUpper();
    if (radioKey.isEmpty() || !sw || sliceLetter.isEmpty()) {
        return;
    }

    m_centerLockSliceLetterByRadioPanIndex[radioKey].insert(sw->panIndex(), sliceLetter);
    saveCenterLockSettings();
}

void MainWindow::restoreCenterLockForPan(const QString& panId)
{
    if (panId.isEmpty() || !m_panStack) {
        return;
    }

    // A band recall owns restoration until its bounded rebuild window ends.
    // Falling through to the ordinary persisted-letter path on each add would
    // let slice arrival order transfer Center Lock to the wrong receiver.
    if (m_centerLockRebind.hasPending(panId)) {
        return;
    }

    const QString radioKey = centerLockRadioKey();
    SpectrumWidget* sw = m_panStack->spectrum(panId);
    if (radioKey.isEmpty() || !sw) {
        return;
    }

    const QString targetLetter = m_centerLockSliceLetterByRadioPanIndex
        .value(radioKey).value(sw->panIndex()).trimmed();
    if (targetLetter.isEmpty()) {
        clearCenterLockForPan(panId);
        return;
    }

    for (SliceModel* slice : m_radioModel.slices()) {
        if (!slice || slice->panId() != panId
            || !m_radioModel.sliceMayBelongToUs(slice->sliceId())
            || slice->letter().compare(targetLetter, Qt::CaseInsensitive) != 0) {
            continue;
        }
        setCenterLockForPan(panId, slice->sliceId(), true, false);
        return;
    }

    clearCenterLockForPan(panId);
}

void MainWindow::setCenterLockForSlice(SliceModel* slice, bool on)
{
    if (!slice || slice->panId().isEmpty()) {
        return;
    }

    setCenterLockForPan(slice->panId(), slice->sliceId(), on);
}

void MainWindow::setCenterLockForPan(const QString& panId, int sliceId, bool on,
                                     bool persist)
{
    if (panId.isEmpty()) {
        return;
    }

    bool canceledPendingRecall = false;
    if (persist) {
        // A persisted setter call is explicit operator intent. It supersedes
        // any automatic restoration still waiting on a band recall.
        canceledPendingRecall = m_centerLockRebind.cancel(panId);
    }

    if (!on) {
        // The live binding is deliberately absent between the recalled slice's
        // removal and its replacement. An explicit off during that window must
        // still clear the saved intent, or a later add/restart can re-lock it.
        if (canceledPendingRecall || sliceId < 0
            || centerLockSliceForPan(panId) == sliceId) {
            clearCenterLockForPan(panId, persist);
        }
        return;
    }

    SliceModel* slice = m_radioModel.slice(sliceId);
    if (!slice || slice->panId() != panId) {
        return;
    }

    m_centerLockSliceByPan.insert(panId, sliceId);
    if (persist) {
        persistCenterLockForSlice(slice);
    }

    syncCenterLockUi(panId);
    recenterCenterLockForPan(panId);
}

void MainWindow::clearCenterLockForPan(const QString& panId, bool clearPersistedIntent)
{
    if (panId.isEmpty()) {
        return;
    }

    if (clearPersistedIntent) {
        m_centerLockRebind.cancel(panId);
    }

    if (clearPersistedIntent && m_panStack) {
        const QString radioKey = centerLockRadioKey();
        if (SpectrumWidget* sw = m_panStack->spectrum(panId);
            !radioKey.isEmpty() && sw) {
            auto radioIt = m_centerLockSliceLetterByRadioPanIndex.find(radioKey);
            if (radioIt != m_centerLockSliceLetterByRadioPanIndex.end()) {
                radioIt->remove(sw->panIndex());
                if (radioIt->isEmpty()) {
                    m_centerLockSliceLetterByRadioPanIndex.erase(radioIt);
                }
                saveCenterLockSettings();
            }
        }
    }

    m_centerLockSliceByPan.remove(panId);
    syncCenterLockUi(panId);
}

void MainWindow::clearCenterLockForSlice(int sliceId, bool clearPersistedIntent)
{
    QStringList pansToClear;
    for (auto it = m_centerLockSliceByPan.cbegin();
         it != m_centerLockSliceByPan.cend(); ++it) {
        if (it.value() == sliceId) {
            pansToClear.append(it.key());
        }
    }
    for (const QString& panId : pansToClear) {
        clearCenterLockForPan(panId, clearPersistedIntent);
    }
}

void MainWindow::syncCenterLockUi(const QString& panId)
{
    if (panId.isEmpty() || !m_panStack) {
        return;
    }

    if (SpectrumWidget* sw = m_panStack->spectrum(panId)) {
        sw->setCenterLockSliceId(centerLockSliceForPan(panId));
    }
}

bool MainWindow::snapCenterLockForSlice(SliceModel* slice, double mhz, bool sendCommand)
{
    if (!centerLockActiveForSlice(slice) || m_sliceDragInProgress) {
        return false;
    }

    const QString panId = slice->panId();
    const bool kiwiDisplayActive = kiwiSdrPanDisplaysKiwi(panId);

    if (m_wfmSliceId >= 0) {
        SliceModel* wfmSlice = m_radioModel.slice(m_wfmSliceId);
        if (wfmSlice && wfmSlice->panId() == panId) {
            return false;
        }
    }

    PanadapterModel* pan = m_radioModel.panadapter(panId);
    if (!pan) {
        return false;
    }

    SpectrumWidget* sw = m_panStack ? m_panStack->spectrum(panId) : nullptr;
    // Pan-BACKGROUND drag standdown (#3854 review): m_sliceDragInProgress only
    // covers slice/VFO drags. During a background drag the drag path owns the
    // center; snapping here would send a counter-command per radio echo and
    // ping-pong the pan against the gesture. The release echo re-asserts the
    // lock via the next recenter.
    if (sw && sw->panDragActive()) {
        return false;
    }
    // Effective (pending-else-model) geometry: during the profile-load hold
    // the model deliberately lags a deferred request, and comparing against it
    // would re-issue the same write on every echo (#4142).
    const double effPanBandwidthMhz = m_radioModel.effectivePanBandwidthMhz(panId);
    const double bandwidthMhz = kiwiDisplayActive && sw && sw->bandwidthMhz() > 0.0
        ? sw->bandwidthMhz()
        : (effPanBandwidthMhz > 0.0
            ? effPanBandwidthMhz
            : (sw ? sw->bandwidthMhz() : 0.0));
    const double targetCenterMhz =
        bandwidthMhz > 0.0 ? std::max(mhz, bandwidthMhz / 2.0) : mhz;
    bool changed = false;

    const bool modelNeedsCenter =
        !qFuzzyCompare(m_radioModel.effectivePanCenterMhz(panId), targetCenterMhz);
    bool centerDeferred = false;

    if (!kiwiDisplayActive && modelNeedsCenter) {
        if (sendCommand) {
            // #4142 — defer, never drop. requestPanCenter() advances the local
            // model only when the command actually reaches the wire, and queues
            // it for replay when a profile load is holding radio-state writes.
            centerDeferred = !m_radioModel.requestPanCenter(panId, targetCenterMhz);
        } else {
            // Local-only snap: the caller explicitly wants no radio write, so
            // there is no wire command for the model to diverge from.
            pan->setCenterBandwidth(targetCenterMhz, -1.0);  // aetherd RFC 2.3
        }
        changed = !centerDeferred;
    }

    // Never move the visible span while the radio stays put. A view that claims
    // a center the radio never took is precisely what projects honest tiles into
    // a lying frame and bakes black rows into waterfall history (#4142).
    if (!centerDeferred && sw && bandwidthMhz > 0.0
        && (!qFuzzyCompare(sw->centerMhz(), targetCenterMhz)
            || !qFuzzyCompare(sw->bandwidthMhz(), bandwidthMhz))) {
        sw->setFrequencyRangeImmediate(targetCenterMhz, bandwidthMhz);
        changed = true;
    }

    return changed;
}

// Re-push the pan's authoritative geometry into its spectrum view.
//
// SpectrumWidget suppresses inbound geometry while a local gesture owns the
// view (drag, VFO drag, a settling zoom). That is correct — an echo arriving
// mid-drag is stale. It was also SAFE only because Flex re-echoes pan status
// continuously, so the next status replaced whatever was dropped within
// milliseconds.
//
// A backend that emits geometry only when it CHANGES has no next status. The
// HL2 publishes its pan centre from the RX NCO, once, on tune: if that lands
// during a gesture hold it is gone for good, and the view stays parked at the
// old centre while the slice, the pan model and every waterfall row have moved
// on — measured at a permanent 6.3 kHz offset after a single drag-tune.
//
// So the widget reports that it suppressed something, and we re-push the model
// value HERE rather than let it replay the value it saw. The model is the
// authority for both families: for Flex it only advances once a command
// actually reached the wire (#4142), and for a DSP-owning backend it mirrors
// hardware state. Reading it now cannot resurrect a stale echo.
void MainWindow::resyncPanGeometryToView(const QString& panId)
{
    if (m_shuttingDown || !m_panStack)
        return;
    // Not while the pan displays a KiwiSDR: there the WIDGET is the authority
    // and the model is frozen at kiwi-assignment geometry by design
    // (PanRecenterPolicy) — re-pushing it would snap the operator's
    // widget-local zoom back to the assignment span, the #4424 bug through a
    // new door. Same reasoning as retryDeferredKiwiLeaveReconcile: the leave-
    // kiwi reconcile is what re-marries model and view afterwards.
    if (kiwiSdrPanDisplaysKiwi(panId))
        return;
    auto* pan = m_radioModel.panadapter(panId);
    auto* sw = m_panStack->spectrum(panId);
    if (!pan || !sw)
        return;
    // Effective (pending-else-model) geometry: a deferred write in flight —
    // e.g. the leave-kiwi reconcile parked behind a profile-load hold (#4142)
    // — supersedes the model value; re-pushing the superseded span here would
    // undo it. With nothing pending these read the model unchanged.
    const double centerMhz = m_radioModel.effectivePanCenterMhz(panId);
    const double bandwidthMhz = m_radioModel.effectivePanBandwidthMhz(panId);
    if (centerMhz <= 0.0 || bandwidthMhz <= 0.0)
        return;
    if (qFuzzyCompare(sw->centerMhz(), centerMhz)
        && qFuzzyCompare(sw->bandwidthMhz(), bandwidthMhz)) {
        return;   // already in agreement — nothing was actually lost
    }
    qCDebug(lcProtocol).noquote()
        << "MainWindow: re-syncing suppressed pan geometry to view"
        << QStringLiteral("pan=%1").arg(panId)
        << QStringLiteral("view=%1").arg(sw->centerMhz(), 0, 'f', 6)
        << QStringLiteral("model=%1").arg(centerMhz, 0, 'f', 6);
    sw->setFrequencyRangeImmediate(centerMhz, bandwidthMhz);
}

void MainWindow::snapCenterLocksForTuningSlice(SliceModel* slice, double mhz,
                                                bool sendCommand)
{
    if (!slice) {
        return;
    }

    const QList<int> lockedSliceIds = m_centerLockSliceByPan.values();
    for (int lockedSliceId : lockedSliceIds) {
        SliceModel* lockedSlice = m_radioModel.slice(lockedSliceId);
        if (!lockedSlice) {
            continue;
        }
        if (lockedSlice == slice || isSameDiversityReceivePair(lockedSlice, slice)) {
            snapCenterLockForSlice(lockedSlice, mhz, sendCommand);
        }
    }
}

void MainWindow::holdCenterLockTuneTarget(SliceModel* slice, double mhz)
{
    if (!slice || mhz <= 0.0) {
        return;
    }

    bool lockApplies = false;
    for (int lockedSliceId : m_centerLockSliceByPan.values()) {
        SliceModel* lockedSlice = m_radioModel.slice(lockedSliceId);
        if (lockedSlice
            && (lockedSlice == slice || isSameDiversityReceivePair(lockedSlice, slice))) {
            lockApplies = true;
            break;
        }
    }
    if (!lockApplies) {
        return;
    }

    const int sliceId = slice->sliceId();
    const qint64 untilMs = QDateTime::currentMSecsSinceEpoch()
        + kCenterLockTuneEchoHoldMs;
    m_centerLockTuneHoldBySlice.insert(sliceId, {mhz, untilMs});

    QTimer::singleShot(kCenterLockTuneEchoHoldMs + 50, this,
                       [this, sliceId, mhz]() {
        auto hold = m_centerLockTuneHoldBySlice.find(sliceId);
        if (hold == m_centerLockTuneHoldBySlice.end()
            || !qFuzzyCompare(hold->targetMhz, mhz)
            || QDateTime::currentMSecsSinceEpoch() < hold->untilMs) {
            return;
        }
        m_centerLockTuneHoldBySlice.erase(hold);
        if (SliceModel* slice = m_radioModel.slice(sliceId)) {
            pushSliceFrequencyToOverlays(slice, slice->frequency());
        }
    });
}

double MainWindow::centerLockDisplayFrequency(const SliceModel* slice, double mhz) const
{
    if (!slice) {
        return mhz;
    }

    bool lockApplies = false;
    for (int lockedSliceId : m_centerLockSliceByPan.values()) {
        const SliceModel* lockedSlice = m_radioModel.slice(lockedSliceId);
        if (lockedSlice
            && (lockedSlice == slice || isSameDiversityReceivePair(lockedSlice, slice))) {
            lockApplies = true;
            break;
        }
    }
    if (!lockApplies) {
        return mhz;
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    qint64 newestUntilMs = 0;
    double displayMhz = mhz;
    for (auto hold = m_centerLockTuneHoldBySlice.cbegin();
         hold != m_centerLockTuneHoldBySlice.cend(); ++hold) {
        if (hold->untilMs <= nowMs) {
            continue;
        }
        const SliceModel* heldSlice = m_radioModel.slice(hold.key());
        if (!heldSlice
            || (heldSlice != slice && !isSameDiversityReceivePair(heldSlice, slice))) {
            continue;
        }
        if (hold->untilMs > newestUntilMs) {
            newestUntilMs = hold->untilMs;
            displayMhz = hold->targetMhz;
        }
    }
    return displayMhz;
}

void MainWindow::recenterCenterLockForPan(const QString& panId)
{
    if (m_sliceDragInProgress) {
        return;
    }

    const int sliceId = centerLockSliceForPan(panId);
    if (sliceId < 0) {
        return;
    }

    SliceModel* slice = m_radioModel.slice(sliceId);
    if (!slice || slice->panId() != panId) {
        clearCenterLockForPan(panId);
        return;
    }

    const double displayMhz = centerLockDisplayFrequency(slice, slice->frequency());
    snapCenterLockForSlice(slice, displayMhz, true);
}

void MainWindow::recenterCenterLocks()
{
    const QStringList panIds = m_centerLockSliceByPan.keys();
    for (const QString& panId : panIds) {
        recenterCenterLockForPan(panId);
    }
}

// ── Slice Link — cross-panadapter bidirectional VFO link ────────────────────
// Independent pairs of owned slices kept on frequency: each member's
// frequencyChanged is classified by the pure policy
// (models/SliceLinkPolicy.h) and, when it is a genuine move, replayed onto
// that pair's peer through applyTuneRequest, inheriting the lock/SWR guards,
// reveal/pan-follow, and Kiwi virtual tracking.

MainWindow::SliceLinkState* MainWindow::sliceLinkFor(int sliceId)
{
    for (SliceLinkState& link : m_sliceLinks) {
        if (link.aId == sliceId || link.bId == sliceId) {
            return &link;
        }
    }
    return nullptr;
}

const MainWindow::SliceLinkState* MainWindow::sliceLinkFor(int sliceId) const
{
    for (const SliceLinkState& link : m_sliceLinks) {
        if (link.aId == sliceId || link.bId == sliceId) {
            return &link;
        }
    }
    return nullptr;
}

int MainWindow::sliceLinkPeerOf(int sliceId) const
{
    const SliceLinkState* link = sliceLinkFor(sliceId);
    if (!link) {
        return -1;
    }
    return (sliceId == link->aId) ? link->bId : link->aId;
}

void MainWindow::engageSliceLink(int aId, int bId)
{
    SliceModel* a = m_radioModel.slice(aId);
    SliceModel* b = m_radioModel.slice(bId);
    const bool eligible = a && b
        && SliceLinkPolicy::canLink(aId, bId,
                                    m_radioModel.sliceMayBelongToUs(aId),
                                    m_radioModel.sliceMayBelongToUs(bId),
                                    a->diversity(), b->diversity());
    if (!eligible) {
        qDebug() << "MainWindow: slice link refused" << aId << bId;
        return;
    }
    if (sliceLinkPeerOf(aId) == bId) {
        return;
    }
    if (sliceLinkFor(aId) || sliceLinkFor(bId)) {
        // A slice belongs to at most one pair. Re-linking must be explicit:
        // the operator unlinks the old pair first, so nothing is overwritten.
        qDebug() << "MainWindow: slice link refused; member already linked"
                 << aId << bId;
        return;
    }

    SliceLinkState link;
    link.aId = aId;
    link.bId = bId;
    link.a = a;
    link.b = b;
    link.settleGeneration = ++m_sliceLinkGeneration;
    for (SliceModel* s : {a, b}) {
        link.connections
            << connect(s, &SliceModel::frequencyChanged, this,
                       [this, s](double mhz) {
                           onSliceLinkFrequencyChanged(s, mhz);
                       })
            << connect(s, &SliceModel::frequencyCommandIssued, this,
                       [this, s](double mhz) {
                           onSliceLinkFrequencyCommandIssued(s, mhz);
                       })
            << connect(s, &SliceModel::lockedChanged, this,
                       [this, s](bool locked) {
                           onSliceLinkLockedChanged(s, locked);
                       });
    }
    m_sliceLinks.append(std::move(link));
    // Engaging is not a tune request: no initial converge — the first genuine
    // move on either member syncs the pair (radio state stays authoritative).
    qDebug() << "MainWindow: slice link engaged" << a->letter() << b->letter();
    refreshSliceLinkUi();
}

void MainWindow::dissolveSliceLink(int aId, int bId, const char* reason)
{
    for (int i = 0; i < m_sliceLinks.size(); ++i) {
        const SliceLinkState& link = m_sliceLinks.at(i);
        const bool matches = (link.aId == aId && link.bId == bId)
            || (link.aId == bId && link.bId == aId);
        if (!matches) {
            continue;
        }
        for (const QMetaObject::Connection& c : link.connections) {
            QObject::disconnect(c);
        }
        m_sliceLinks.removeAt(i);
        ++m_sliceLinkGeneration;
        qDebug() << "MainWindow: slice link dissolved" << aId << bId
                 << "—" << reason;
        refreshSliceLinkUi();
        return;
    }
}

void MainWindow::dissolveAllSliceLinks(const char* reason)
{
    if (m_sliceLinks.isEmpty()) {
        return;
    }
    for (const SliceLinkState& link : std::as_const(m_sliceLinks)) {
        for (const QMetaObject::Connection& c : link.connections) {
            QObject::disconnect(c);
        }
    }
    m_sliceLinks.clear();
    ++m_sliceLinkGeneration;
    qDebug() << "MainWindow: all slice links dissolved —" << reason;
    refreshSliceLinkUi();
}

void MainWindow::onSliceLinkFrequencyChanged(SliceModel* s, double mhz)
{
    if (m_applyingSliceLink) {
        // The synchronous optimistic emit inside our own applyTuneRequest call.
        return;
    }
    if (!s) {
        return;
    }
    const int id = s->sliceId();
    SliceLinkState* link = sliceLinkFor(id);
    if (!link) {
        return;
    }
    const int aId = link->aId;
    const int bId = link->bId;
    if (!link->a || !link->b) {
        dissolveSliceLink(aId, bId, "member destroyed");
        return;
    }
    // Continuous Multi-Flex guard: never keep driving a pair we no longer own.
    if (!m_radioModel.sliceMayBelongToUs(aId)
        || !m_radioModel.sliceMayBelongToUs(bId)) {
        dissolveSliceLink(aId, bId, "member ownership lost");
        return;
    }
    SliceModel* peer = (id == aId) ? link->b.data() : link->a.data();
    const bool changedIsA = (id == aId);
    SliceLinkPolicy::PendingWrites& changedRing =
        changedIsA ? link->pendingA : link->pendingB;
    SliceLinkPolicy::PendingWrites& peerRing =
        changedIsA ? link->pendingB : link->pendingA;

    SliceLinkPolicy::Input in;
    in.changedHz = llround(mhz * 1e6);
    in.nowMs = QDateTime::currentMSecsSinceEpoch();
    in.peerLocked = peer->isLocked();
    in.swrSweepRunning = m_swrSweep.running;

    const SliceLinkPolicy::Decision d = SliceLinkPolicy::classify(in, changedRing);
    if (d.consumeThrough >= 0) {
        changedRing.consumeThrough(d.consumeThrough);
    }
    if (d.becomeOrigin) {
        link->originId = id;
    }

    switch (d.action) {
    case SliceLinkPolicy::Action::Propagate: {
        if (link->suspendedByLock) {
            link->suspendedByLock = false;
            refreshSliceLinkUi();
        }
        const qint64 peerHz = llround(peer->frequency() * 1e6);
        if (peerHz == in.changedHz) {
            // Already converged (e.g. a settle re-assert) — no write, no echo.
            return;
        }
        peerRing.record(in.changedHz, in.nowMs);
        const TuneIntent intent =
            SliceLinkPolicy::isIncrementalDelta(peerHz, in.changedHz)
                ? TuneIntent::IncrementalTune
                : TuneIntent::AbsoluteJump;
        m_applyingSliceLink = true;
        applyTuneRequest(peer, mhz, intent, "slice-link");
        m_applyingSliceLink = false;
        scheduleSliceLinkSettleCheck(id);
        break;
    }
    case SliceLinkPolicy::Action::SuspendLocked:
        if (!link->suspendedByLock) {
            link->suspendedByLock = true;
            refreshSliceLinkUi();
        }
        break;
    case SliceLinkPolicy::Action::IgnoreEcho:
        // The residual misclassification hole (a genuine move landing on a
        // still-pending value) is closed by the settle check re-asserting
        // the last genuine mover once the pair goes quiet.
        scheduleSliceLinkSettleCheck(id);
        break;
    case SliceLinkPolicy::Action::IgnoreSweep:
        break;
    }
}

void MainWindow::onSliceLinkFrequencyCommandIssued(SliceModel* s, double mhz)
{
    // frequencyChanged is emitted before this command-provenance signal, so a
    // genuine local move has already propagated to the peer. Arm the origin's
    // ring now to absorb its later radio confirmation (including stale values
    // from a fast spin) without replaying them backwards onto the peer.
    if (m_applyingSliceLink || !s) {
        return;
    }
    const int id = s->sliceId();
    SliceLinkState* link = sliceLinkFor(id);
    if (!link) {
        return;
    }
    SliceLinkPolicy::PendingWrites* ring = nullptr;
    if (id == link->aId) {
        ring = &link->pendingA;
    } else if (id == link->bId) {
        ring = &link->pendingB;
    }
    if (ring) {
        ring->record(llround(mhz * 1e6), QDateTime::currentMSecsSinceEpoch());
    }
}

void MainWindow::onSliceLinkLockedChanged(SliceModel* s, bool locked)
{
    // lockedChanged is emitted unconditionally on status refreshes, so this
    // must be idempotent: act only on an unlock while suspended.
    if (locked || !s) {
        return;
    }
    SliceLinkState* link = sliceLinkFor(s->sliceId());
    if (!link || !link->suspendedByLock) {
        return;
    }
    const int aId = link->aId;
    const int bId = link->bId;
    if (!link->a || !link->b) {
        dissolveSliceLink(aId, bId, "member destroyed");
        return;
    }
    SliceModel* origin = (link->originId == aId) ? link->a.data()
                                                 : link->b.data();
    SliceModel* peer = (origin == link->a.data()) ? link->b.data()
                                                  : link->a.data();
    if (!origin || !peer || peer->isLocked()) {
        return;
    }
    link->suspendedByLock = false;
    refreshSliceLinkUi();
    // Re-converge via the settle check rather than tuning the peer in the
    // same breath as the unlock: the radio applies its own unlock with some
    // latency, and a tune sent immediately after `slice unlock` is refused
    // radio-side (observed on a FLEX-8600) — the refusal echo would then
    // propagate the peer's stale frequency backwards over the origin.
    // kSettleMs later the unlock has landed and the normal diverged-pair
    // re-assert converges the peer to the origin.
    scheduleSliceLinkSettleCheck(s->sliceId());
}

void MainWindow::scheduleSliceLinkSettleCheck(int sliceId)
{
    SliceLinkState* link = sliceLinkFor(sliceId);
    if (!link) {
        return;
    }
    // Trailing convergence net: kSettleMs after the most recent link event
    // (each event bumps the generation, so only the last scheduled check
    // acts), re-assert the last genuine mover if the pair is still diverged.
    const int aId = link->aId;
    const int bId = link->bId;
    const quint64 generation = ++m_sliceLinkGeneration;
    link->settleGeneration = generation;
    QTimer::singleShot(static_cast<int>(SliceLinkPolicy::kSettleMs), this,
                       [this, aId, bId, generation]() {
        SliceLinkState* current = sliceLinkFor(aId);
        if (!current || current->bId != bId
            || current->settleGeneration != generation
            || current->suspendedByLock) {
            return;
        }
        SliceModel* origin = (current->originId == current->aId)
                                 ? current->a.data()
                                 : current->b.data();
        SliceModel* peer = (current->originId == current->aId)
                               ? current->b.data()
                               : current->a.data();
        if (!origin || !peer || current->originId < 0) {
            return;
        }
        if (llround(origin->frequency() * 1e6)
            == llround(peer->frequency() * 1e6)) {
            return;
        }
        onSliceLinkFrequencyChanged(origin, origin->frequency());
    });
}

void MainWindow::refreshSliceLinkUi()
{
    // Push every pair + the eligible-peer roster to every pan's spectrum:
    // links are cross-pan, so a pan's context menu needs peers its own
    // overlays cannot see.
    QVector<SpectrumWidget::SliceLinkCandidate> candidates;
    QVector<SpectrumWidget::SliceLinkPair> pairs;
    const QList<SliceModel*> slices = m_radioModel.slices();
    for (SliceModel* s : slices) {
        if (!s || !m_radioModel.sliceMayBelongToUs(s->sliceId())
            || s->diversity()) {
            continue;
        }
        candidates.append({s->sliceId(),
                           SliceLabel::unicodeForm(s->sliceId(), s->letter())});
    }
    for (const SliceLinkState& link : std::as_const(m_sliceLinks)) {
        pairs.append({link.aId, link.bId, link.suspendedByLock});
    }
    if (!m_panStack) {
        return;
    }
    const auto applets = m_panStack->allApplets();
    for (PanadapterApplet* applet : applets) {
        if (applet && applet->spectrumWidget()) {
            SpectrumWidget* sw = applet->spectrumWidget();
            sw->setSliceLinkCandidates(candidates);
            sw->setSliceLinkPairs(pairs);
        }
    }
}

QJsonObject MainWindow::automationSetSliceLink(int aId, int bId, bool on)
{
    if (!on) {
        if (sliceLinkPeerOf(aId) == bId) {
            dissolveSliceLink(aId, bId, "bridge request");
        }
        return QJsonObject{{QStringLiteral("ok"), true},
                           {QStringLiteral("linked"), false}};
    }
    engageSliceLink(aId, bId);
    if (sliceLinkPeerOf(aId) != bId) {
        return QJsonObject{
            {QStringLiteral("ok"), false},
            {QStringLiteral("error"),
             QStringLiteral("refused: slices %1 and %2 are not linkable "
                            "(unknown, foreign, diversity, identical, or "
                            "already linked to another slice)")
                 .arg(aId)
                 .arg(bId)}};
    }
    return QJsonObject{{QStringLiteral("ok"), true},
                       {QStringLiteral("linked"), true},
                       {QStringLiteral("a"), aId},
                       {QStringLiteral("b"), bId}};
}

void MainWindow::syncActiveSliceSquelchLineToSpectrums()
{
    SliceModel* s = activeSlice();
    if (!s) {
        auto clearSpectrum = [](SpectrumWidget* sw) {
            if (!sw) {
                return;
            }
            sw->clearKiwiSdrSquelchLine();
            sw->setSquelchLine(false, 0);
        };
        if (!m_panStack) {
            clearSpectrum(spectrum());
            return;
        }
        for (PanadapterApplet* applet : m_panStack->allApplets()) {
            if (!applet) {
                continue;
            }
            clearSpectrum(m_panStack->spectrum(applet->panId()));
        }
        return;
    }

    const bool kiwiActive = m_kiwiSdrManager
        && !m_kiwiSdrManager->assignedProfileForSlice(s->sliceId()).isEmpty();
    const bool on = kiwiActive ? s->receiveSquelchOn() : s->squelchOn();
    const int level = kiwiActive ? s->receiveSquelchLevel()
                                 : s->squelchLevel();
    const QString kiwiPanId = kiwiActive ? s->panId() : QString();
    auto applyToSpectrum = [&](const QString& panId, SpectrumWidget* sw) {
        if (!sw) {
            return;
        }
        if (SliceModel* kiwiSlice =
                kiwiAssignedSliceForPan(m_radioModel, m_kiwiSdrManager, panId)) {
            sw->setKiwiSdrSquelchLine(
                kiwiSlice->receiveSquelchOn(),
                kiwiSdrSquelchMarginDbForSliceLevel(
                    kiwiSlice, kiwiSlice->receiveSquelchLevel()),
                kiwiSlice->externalReceiveAutoSquelchOn());
            sw->setSquelchLine(false, 0);
            return;
        }
        if (kiwiActive) {
            if (panId == kiwiPanId) {
                sw->setKiwiSdrSquelchLine(
                    on, kiwiSdrSquelchMarginDbForSliceLevel(s, level),
                    s->externalReceiveAutoSquelchOn());
            } else {
                sw->clearKiwiSdrSquelchLine();
                sw->setSquelchLine(false, 0);
            }
            return;
        }
        if (!kiwiSdrProfileForPan(panId).isEmpty()
            || sw->kiwiSdrWaterfallActive()) {
            sw->clearKiwiSdrSquelchLine();
            sw->setSquelchLine(false, 0);
            return;
        }
        sw->clearKiwiSdrSquelchLine();
        sw->setSquelchLine(on, level);
    };

    if (!m_panStack) {
        applyToSpectrum(s->panId(), spectrumForSlice(s));
        return;
    }

    for (PanadapterApplet* applet : m_panStack->allApplets()) {
        if (!applet) {
            continue;
        }
        applyToSpectrum(applet->panId(), m_panStack->spectrum(applet->panId()));
    }
}

bool MainWindow::autoSquelchShouldRunOnSpectrum(
    const QString& panId, const SpectrumWidget* spectrum) const
{
    if (const SliceModel* kiwiSlice =
            kiwiAssignedSliceForPan(m_radioModel, m_kiwiSdrManager, panId)) {
        return kiwiSlice->externalReceiveAutoSquelchOn();
    }

    if (!m_appletPanel || !m_appletPanel->rxApplet()
        || m_appletPanel->rxApplet()->sqlMode() != RxApplet::SqlMode::Auto) {
        return false;
    }

    const SliceModel* s = activeSlice();
    if (!s) {
        return false;
    }

    const bool activeKiwi = m_kiwiSdrManager
        && !m_kiwiSdrManager->assignedProfileForSlice(s->sliceId()).isEmpty();

    if (activeKiwi) {
        return panId == s->panId();
    }

    return kiwiSdrProfileForPan(panId).isEmpty()
        && (!spectrum || !spectrum->kiwiSdrWaterfallActive());
}

void MainWindow::syncActiveSliceAutoSquelchToSpectrums()
{
    auto applyToSpectrum = [this](const QString& panId, SpectrumWidget* sw) {
        if (!sw) {
            return;
        }
        sw->setAutoSquelchEnable(autoSquelchShouldRunOnSpectrum(panId, sw));
    };

    if (!m_panStack) {
        if (SliceModel* s = activeSlice()) {
            applyToSpectrum(s->panId(), spectrumForSlice(s));
        } else {
            applyToSpectrum(QString(), spectrum());
        }
        return;
    }

    for (PanadapterApplet* applet : m_panStack->allApplets()) {
        if (!applet) {
            continue;
        }
        applyToSpectrum(applet->panId(), m_panStack->spectrum(applet->panId()));
    }
}

void MainWindow::wireAetherDspWidget(AetherDspWidget* w)
{
    if (!w || !m_audio) return;

    connect(w, &AetherDspWidget::dspMethodUserToggled,
            this, [this](const QString&, bool) {
        m_aetherDspModePolicy.noteUserOverride();
    });

    // NR2
    connect(w, &AetherDspWidget::nr2GainMaxChanged, this, [this](float v) {
        QMetaObject::invokeMethod(m_audio, [this, v]() { m_audio->setNr2GainMax(v); });
    });
    connect(w, &AetherDspWidget::nr2GainFloorChanged, this, [this](float v) {
        QMetaObject::invokeMethod(m_audio, [this, v]() { m_audio->setNr2GainFloor(v); });
    });
    connect(w, &AetherDspWidget::nr2GainSmoothChanged, this, [this](float v) {
        QMetaObject::invokeMethod(m_audio, [this, v]() { m_audio->setNr2GainSmooth(v); });
    });
    connect(w, &AetherDspWidget::nr2QsppChanged, this, [this](float v) {
        QMetaObject::invokeMethod(m_audio, [this, v]() { m_audio->setNr2Qspp(v); });
    });
    connect(w, &AetherDspWidget::nr2GainMethodChanged, this, [this](int m) {
        QMetaObject::invokeMethod(m_audio, [this, m]() { m_audio->setNr2GainMethod(m); });
    });
    connect(w, &AetherDspWidget::nr2NpeMethodChanged, this, [this](int m) {
        QMetaObject::invokeMethod(m_audio, [this, m]() { m_audio->setNr2NpeMethod(m); });
    });
    connect(w, &AetherDspWidget::nr2AeFilterChanged, this, [this](bool on) {
        QMetaObject::invokeMethod(m_audio, [this, on]() { m_audio->setNr2AeFilter(on); });
    });
    connect(w, &AetherDspWidget::nr2UseOriginalGeometryChanged,
            this, [this](bool useOriginal) {
        QMetaObject::invokeMethod(m_audio, [this, useOriginal]() {
            m_audio->setNr2UseOriginalGeometry(useOriginal);
        });
    });
    // NR4
    connect(w, &AetherDspWidget::nr4ReductionChanged, this, [this](float v) {
        QMetaObject::invokeMethod(m_audio, [this, v]() { m_audio->setNr4ReductionAmount(v); });
    });
    connect(w, &AetherDspWidget::nr4SmoothingChanged, this, [this](float v) {
        QMetaObject::invokeMethod(m_audio, [this, v]() { m_audio->setNr4SmoothingFactor(v); });
    });
    connect(w, &AetherDspWidget::nr4WhiteningChanged, this, [this](float v) {
        QMetaObject::invokeMethod(m_audio, [this, v]() { m_audio->setNr4WhiteningFactor(v); });
    });
    connect(w, &AetherDspWidget::nr4AdaptiveNoiseChanged, this, [this](bool on) {
        QMetaObject::invokeMethod(m_audio, [this, on]() { m_audio->setNr4AdaptiveNoise(on); });
    });
    connect(w, &AetherDspWidget::nr4NoiseMethodChanged, this, [this](int m) {
        QMetaObject::invokeMethod(m_audio, [this, m]() { m_audio->setNr4NoiseEstimationMethod(m); });
    });
    connect(w, &AetherDspWidget::nr4MaskingDepthChanged, this, [this](float v) {
        QMetaObject::invokeMethod(m_audio, [this, v]() { m_audio->setNr4MaskingDepth(v); });
    });
    connect(w, &AetherDspWidget::nr4SuppressionChanged, this, [this](float v) {
        QMetaObject::invokeMethod(m_audio, [this, v]() { m_audio->setNr4SuppressionStrength(v); });
    });
    // DFNR
    // RN2
    connect(w, &AetherDspWidget::rn2DryMixChanged, this, [this](float v) {
        QMetaObject::invokeMethod(m_audio, [this, v]() { m_audio->setRn2DryMix(v); });
    });
    connect(w, &AetherDspWidget::dfnrAttenLimitChanged, this, [this](float v) {
        QMetaObject::invokeMethod(m_audio, [this, v]() { m_audio->setDfnrAttenLimit(v); });
    });
    connect(w, &AetherDspWidget::dfnrPostFilterBetaChanged, this, [this](float v) {
        QMetaObject::invokeMethod(m_audio, [this, v]() { m_audio->setDfnrPostFilterBeta(v); });
    });
    // MNR
    connect(w, &AetherDspWidget::mnrStrengthChanged, this, [this](float v) {
        QMetaObject::invokeMethod(m_audio, [this, v]() { m_audio->setMnrStrength(v); });
    });
    // NR2 enable: route through enableNr2WithWisdom so FFTW plans are
    // ready before AudioEngine constructs SpectralNR — direct enable on
    // the audio thread can leave plans incompatible with the runtime
    // FFTW version and crash the next feedAudioData. (#2275 / NR4→NR2)
    connect(w, &AetherDspWidget::nr2EnableWithWisdomRequested,
            this, &MainWindow::enableNr2WithWisdom);
}


void MainWindow::wireVfoTelemetry(VfoWidget* vfo, SliceModel* s)
{
    if (!vfo || !s) {
        return;
    }

    // Feed S-meter per-slice — only this VFO's slice level
    const int sid = s->sliceId();
    const QPointer<VfoWidget> vfoPtr(vfo);
    connect(&m_radioModel.meterModel(), &MeterModel::sLevelChanged,
            vfo, [this, vfoPtr, sid](int sliceIndex, float dbm) {
        if (sliceIndex == sid
            && (!m_kiwiSdrManager
                || m_kiwiSdrManager->assignedProfileForSlice(sid).isEmpty())) {
            deferReceivePresentation(
                ReceivePresentationSource::Flex,
                ReceivePresentationSurface::Meter,
                [this, vfoPtr, sid, dbm]() {
                    if (!vfoPtr
                        || (m_kiwiSdrManager
                            && !m_kiwiSdrManager
                                    ->assignedProfileForSlice(sid).isEmpty())) {
                        return;
                    }
                    vfoPtr->setSignalLevel(dbm);
                },
                QString::number(sid));
        }
    });
    // Feed ESC meter per-slice — signal strength after ESC processing
    connect(&m_radioModel.meterModel(), &MeterModel::escLevelChanged,
            vfo, [this, vfoPtr, sid](int sliceIndex, float dbm) {
        if (sliceIndex == sid) {
            deferReceivePresentation(
                ReceivePresentationSource::Flex,
                ReceivePresentationSurface::Meter,
                [vfoPtr, dbm]() {
                    if (vfoPtr) {
                        vfoPtr->setEscLevel(dbm);
                    }
                },
                QString::number(sid));
        }
    });
    // Feed the SmartMTR TX scales: mic level + compression (dBFS / dB) from
    // micMetersChanged, and forward power + SWR from txMetersChanged. The VFO
    // shows the operator-selected meter only on its own TX slice while
    // transmitting. No amp-operate gate (unlike the analog S-Meter below): the
    // meter reports the truth of whatever the operator selected.
    connect(&m_radioModel.meterModel(), &MeterModel::micMetersChanged,
            vfo, [vfo](float micLevel, float, float micPeak, float compPeak) {
        vfo->setMicLevel(micLevel, micPeak);
        vfo->setTxCompression(compPeak);
    });
    connect(&m_radioModel.meterModel(), &MeterModel::txMetersChanged,
            vfo, [vfo](float fwd, float swr, bool swrValid) {
        vfo->setTxPower(fwd);
        // Absent SWR keeps the widget's idle value rather than a stale ratio.
        vfo->setTxSwr(swrValid ? swr : 0.0f);
    });
    auto updateVfoTxGate = [this, vfo]() {
        const auto& tx = m_radioModel.transmitModel();
        vfo->setTransmitting(tx.isTransmitting() || tx.isTuning()
                             || m_radioModel.isRadioTransmitting());
    };
    connect(&m_radioModel.transmitModel(), &TransmitModel::moxChanged,
            vfo, [updateVfoTxGate](bool) { updateVfoTxGate(); });
    connect(&m_radioModel.transmitModel(), &TransmitModel::tuneChanged,
            vfo, [updateVfoTxGate](bool) { updateVfoTxGate(); });
    connect(&m_radioModel, &RadioModel::radioTransmittingChanged,
            vfo, [updateVfoTxGate](bool) { updateVfoTxGate(); });
    updateVfoTxGate();
    connect(&m_radioModel, &RadioModel::antListChanged,
            vfo, &VfoWidget::setAntennaList);
}


bool MainWindow::reattachSliceVisualsToPanadapter(SliceModel* s)
{
    // (No m_applyingLayout guard: that flag is never set anywhere — a dead
    // remnant of the old delete/recreate layout path. The sibling guards in
    // onSliceAdded/onSliceRemoved are equally inert; flagged for cleanup.)
    if (!s) {
        return false;
    }

    SpectrumWidget* target = nullptr;
    if (m_panStack && !s->panId().isEmpty()) {
        target = m_panStack->spectrum(s->panId());
    } else {
        target = spectrum();
    }
    if (!target) {
        return false;
    }

    const int sliceId = s->sliceId();
    VfoWidget* targetVfo = target->vfoWidget(sliceId);

    if (m_panStack) {
        for (PanadapterApplet* applet : m_panStack->allApplets()) {
            if (!applet) {
                continue;
            }
            SpectrumWidget* sw = applet->spectrumWidget();
            if (!sw || sw == target) {
                continue;
            }
            sw->removeSliceOverlay(sliceId);
            if (!sw->vfoWidget(sliceId)) {
                continue;
            }
            if (!targetVfo) {
                targetVfo = sw->takeVfoWidget(sliceId);
                if (targetVfo) {
                    target->adoptVfoWidget(sliceId, targetVfo);
                }
            } else {
                sw->removeVfoWidget(sliceId);
            }
        }
    }

    if (!targetVfo) {
        targetVfo = target->addVfoWidget(sliceId);
        if (targetVfo) {
            const QString& sub = m_radioModel.licenseSubscription();
            targetVfo->setSmartSdrPlus(sub.contains("SmartSDR+"));
            targetVfo->setHasExtendedDsp(m_radioModel.hasExtendedDspFilters());
            targetVfo->setHasRadioSideDsp(m_radioModel.hasRadioSideDsp());
            targetVfo->setHasLmsNoiseFilters(m_radioModel.hasLmsNoiseFilters());
            targetVfo->setHasManualNotch(m_radioModel.hasManualNotch());
            targetVfo->setHasHostNoiseBlanker(m_radioModel.hasHostNoiseBlanker());
            targetVfo->setRadioFilterWidths(m_radioModel.radioFilterWidthsHz());
            wireVfoWidget(targetVfo, s);
            targetVfo->setDiversityAllowed(m_radioModel.isDiversityAllowed());
            wireVfoTelemetry(targetVfo, s);
#ifdef HAVE_RADE
            // Parity with onSliceAdded: a deferred-attach slice can already
            // be in FDVU/FDVL when its pan finally lands (#4037 review).
            if (s->mode().startsWith("FDV")) {
                activateFdvDisplay(sliceId);
            }
#endif
        }
    }

    if (targetVfo && sliceId == m_activeSliceId) {
        target->setActiveVfoWidget(sliceId);
    }
    if (m_panStack && !s->panId().isEmpty()) {
        if (PanadapterApplet* applet = m_panStack->panadapter(s->panId())) {
            applet->setSliceId(sliceId, s->letter());
        }
    }

    pushSliceOverlay(s);
    target->setSliceOverlayLetter(sliceId, s->letter());
    if (s->isTxSlice()) {
        if (m_panStack) {
            for (PanadapterApplet* applet : m_panStack->allApplets()) {
                if (applet && applet->spectrumWidget()) {
                    applet->spectrumWidget()->setHasTxSlice(
                        applet->spectrumWidget() == target);
                }
            }
        } else if (m_panApplet && m_panApplet->spectrumWidget()) {
            m_panApplet->spectrumWidget()->setHasTxSlice(true);
        }
        syncTxWaterfallSliceToSpectrums();
    }
    return true;
}


void MainWindow::onSliceAdded(SliceModel* s)
{
    // During layout transition, spectrums are being destroyed/recreated — skip
    if (m_applyingLayout) return;

    qDebug() << "MainWindow: slice added" << s->sliceId();
    const bool firstSlice = (m_activeSliceId < 0);

    // A slice arriving on a pan with a live band-recall window is the radio
    // still rebuilding that pan. Extend the window (bounded) before anything
    // below consults it, so a rebuild slower than the base grace period stays
    // covered end to end instead of only for its first 1500 ms.
    m_bandRecallSelection.refresh(s->panId(), QDateTime::currentMSecsSinceEpoch());

    // First slice — wire everything up
    if (firstSlice) {
        selectSliceFromRadioState(
            s, RadioSliceSelectionSource::TopologyFallback);

        // Detect initial band from radio's frequency
        if (m_bandSettings.currentBand().isEmpty())
            m_bandSettings.setCurrentBand(BandSettings::bandForFrequency(s->frequency()));

        // Re-create audio stream if it was invalidated by a profile load.
        // Only create if PC Audio is enabled — if the user is listening
        // through the radio's hardware outputs, don't switch to PC. (#336)
        if (m_needAudioStream) {
            m_needAudioStream = false;
            if (AppSettings::instance().value("PcAudioEnabled", "True").toString() == "True")
                m_radioModel.createRxAudioStream();
        }

        // Restore client-side DSP (NR2/RN2) from last session.
        // Deferred so the VFO widget exists for button sync.
        QTimer::singleShot(500, this, [this]() {
            auto& settings = AppSettings::instance();
            if (Nr2SettingsModel::instance().config().enabled)
                enableNr2WithWisdom();
            else if (settings.value("ClientRn2Enabled", "False").toString() == "True")
                QMetaObject::invokeMethod(m_audio, [this]() { m_audio->setRn2Enabled(true); });
            else if (settings.value("ClientNr4Enabled", "False").toString() == "True")
                QMetaObject::invokeMethod(m_audio, [this]() { m_audio->setNr4Enabled(true); });
            else if (settings.value("ClientDfnrEnabled", "False").toString() == "True")
                QMetaObject::invokeMethod(m_audio, [this]() { m_audio->setDfnrEnabled(true); });
            else if (settings.value("ClientMnrEnabled", "False").toString() == "True")
                QMetaObject::invokeMethod(m_audio, [this]() { m_audio->setMnrEnabled(true); });
            // BNR not auto-restored — requires manual enable each session

            refreshCwDecodeState();
            refreshRttyDecodeState();
        });
    }

    // Restore per-slice DAX channel from last session (#1221).
    // Deferred so the radio's initial slice status has arrived first. Skip this
    // during profile recall: setDaxChannel() updates local slice state before
    // sending the radio command, and profile-created slices must keep the
    // radio/profile DAX assignment.
    //
    // #4558: only within the post-connect restore window. The keys are
    // position-based (A = index 0, ...), so a mid-session destroy/recreate —
    // which re-enters the list at the tail — resolves to the WRONG key when
    // other slices are open: with B alive, a recreated A reads
    // DaxChannel_SliceB and steals B's live channel 300 ms later. Mid-session
    // adds have current radio/TCI state; last-session keys apply only to the
    // enumeration that follows a connect. See DaxRestorePolicy.h.
    {
        const int sliceIdx = m_radioModel.slices().indexOf(s);
        const QString key = sliceIdx >= 0
            ? DaxRestorePolicy::keyForIndex(sliceIdx) : QString();
        const int savedDax = key.isEmpty()
            ? 0 : AppSettings::instance().value(key, "0").toInt();
        // Resolve the key before branching so the skip path can say whether a
        // restore was actually suppressed. Logging every mid-session add would
        // report "skipping" for the overwhelmingly common case of no key at
        // all, which is noise in exactly the captures this logging exists for.
        if (savedDax > 0 && m_daxRestore.restoreAllowed()) {
            // QPointer guards against a dangling slice: a raw SliceModel*
            // stays non-null after the slice is destroyed, so the `s &&`
            // check below is only meaningful if the capture auto-nulls.
            // Removing the slice within this 300 ms window (rapid
            // add/remove) would otherwise dereference freed memory. (#3646)
            QPointer<SliceModel> sp(s);
            QTimer::singleShot(300, this, [this, sp, savedDax, key]() {
                if (sp && !profileLoadRadioStateWritesHeld()) {
                    // The send path below (SliceModel::commandReady) has no
                    // logging of its own; without this line the restore is
                    // invisible in any capture (#4558).
                    qCDebug(lcDax) << "MainWindow: restoring last-session DAX"
                                   << key << "=" << savedDax
                                   << "to slice" << sp->sliceId();
                    sp->setDaxChannel(savedDax);
                }
            });
        } else if (savedDax > 0) {
            qCDebug(lcDax) << "MainWindow: skipping last-session DAX restore"
                           << key << "=" << savedDax
                           << "for mid-session slice add, id" << s->sliceId()
                           << "(#4558)";
        }
    }

    // Re-claim TX assignment after profile load or slice recreation (#145).
    // The radio sets tx=1 on the slice but tx_client_handle may be 0x00000000
    // if the slice was destroyed and recreated (e.g. by profile global load).
    // During profile recall, RadioModel's profile-load hold suppresses this
    // slice write so it cannot fight the radio's restored profile state.
    if (s->isTxSlice()) {
        s->setTxSlice(true);
    }

    // Keep the radio-side TX DAX flag aligned when the TX slice or mode changes.
    // SmartSDR DAX2 on Windows owns both the dax_tx stream and the radio's
    // `transmit dax` flag, so Windows is a no-op here — AetherSDR must not
    // toggle that flag on slice-mode transitions (e.g. band-stack restore)
    // because doing so parks DAX2's TX Stream in Busy. (#2315)
    auto updateDaxTxMode = [this]() {
        bool isDigital = false;
        int txSliceId = -1;
        for (auto* sl : m_radioModel.slices()) {
            if (sl->isTxSlice()) {
                txSliceId = sl->sliceId();
                const QString& m = sl->mode();
                isDigital = (m == "DIGU" || m == "DIGL" || m == "RTTY"
                          || m == "DFM"  || m == "NFM"  || m == "NT");
                break;
            }
        }
        // Digital modes need dax=1 for TX audio routing through DAX, but only
        // on platforms where AetherSDR actually owns the dax_tx feed — hosted
        // DAX (macOS / PipeWire).  Linux non-PipeWire builds have no DAX feed
        // available, so leave the radio's dax flag alone to keep digital TX
        // on the physical mic input. (#2273)
        //
        // Windows is intentionally excluded: SmartSDR DAX2 owns the radio's
        // `transmit dax` flag and the dax_tx stream.  Toggling it from slice-
        // mode changes (e.g. band-stack restore flipping the slice from DIGU
        // back to USB during a band switch) sends `transmit set dax=0`, which
        // parks DAX2's TX Stream in Busy and silently breaks digital TX until
        // the user re-arms it from the DAX2 window.  Let the user manage that
        // state via DAX2 — matches SmartSDR Console behavior. (#2315)
#if defined(Q_OS_MAC) || defined(HAVE_PIPEWIRE)
        m_audio->setDaxTxMode(isDigital);
        if (!profileLoadRadioStateWritesHeld()) {
            m_radioModel.transmitModel().setDax(isDigital);
            if (isDigital) {
                m_radioModel.ensureDaxTxStream(DaxTxRequestReason::HostedDaxBridge);
            }
        }
#else
        Q_UNUSED(isDigital);
#endif

#ifdef HAVE_RADE
        // RADE mode should only route mic→RADEEngine when the TX slice IS
        // the RADE slice.  Otherwise a RADE slice running on a non-TX slice
        // would hijack voice TX on the actual TX slice.
        if (m_radeSliceId >= 0 && m_radeEngine && m_radeEngine->isActive())
            m_audio->setRadeMode(txSliceId == m_radeSliceId);
#else
        Q_UNUSED(txSliceId);
#endif
    };
    connect(s, &SliceModel::modeChanged, this, updateDaxTxMode);
    connect(s, &SliceModel::txSliceChanged, this, updateDaxTxMode);

    // Status-bar toast on the first blocked tune of a sustained sequence.
    // Driven by SliceModel's lockedFeedbackActiveChanged (false→true edge),
    // which gives natural dedup — a stuck MIDI knob or held keyboard arrow
    // generates one toast, not one per event. The on-screen LOCKED overlay
    // on the VFO/RX freq label handles the per-event visual feedback. (#2984)
    connect(s, &SliceModel::lockedFeedbackActiveChanged, this, [this](bool active) {
        if (active)
            statusBar()->showMessage(tr("Slice is locked — unlock to tune."), 3000);
    });
    updateDaxTxMode();  // set initial state from current TX slice mode

    // Push overlay for this slice to the spectrum widget
    pushSliceOverlay(s);

    // Set the panadapter applet's slice label (e.g. "Slice B") based on
    // which pan this slice belongs to
    if (m_panStack && !s->panId().isEmpty()) {
        if (auto* applet = m_panStack->panadapter(s->panId()))
            applet->setSliceId(s->sliceId(), s->letter());
    }

    // Set initial hasTxSlice for waterfall freeze logic
    if (s->isTxSlice()) {
        if (auto* sw = spectrumForSlice(s))
            sw->setHasTxSlice(true);
    }

    // Sync show-TX-in-waterfall on first slice
    if (auto* sw = spectrumForSlice(s))
        sw->setShowTxInWaterfall(
            m_radioModel.transmitModel().showTxInWaterfall());
    syncTxWaterfallSliceToSpectrums();

    // Per-client letter (#2606) — keep spectrum overlay synced so the
    // slice marker / passband colour follows the badge in RadioIndexed
    // display mode.  Also push the current letter once now in case it
    // was already set at slice creation.
    if (auto* sw = spectrumForSlice(s))
        sw->setSliceOverlayLetter(s->sliceId(), s->letter());
    connect(s, &SliceModel::letterChanged, this,
            [this, s](const QString& letter) {
        if (auto* sw = spectrumForSlice(s))
            sw->setSliceOverlayLetter(s->sliceId(), letter);
        if (centerLockActiveForSlice(s)) {
            persistCenterLockForSlice(s);
        } else {
            restoreCenterLockForPan(s->panId());
        }
    });
    restoreCenterLockForPan(s->panId());

    // Connect slice state changes → spectrum overlay updates
    connect(s, &SliceModel::frequencyChanged, this, [this, s](double mhz) {
        // Don't snap overlay back to stale radio-confirmed freq during active
        // encoder tuning — the optimistic VFO position is already ahead (#1524)
        bool activeTuning = false;
#ifdef HAVE_SERIALPORT
        activeTuning = activeTuning || m_flexCoalesceTimer.isActive();
#endif
#ifdef HAVE_MIDI
        activeTuning = activeTuning || m_midiTuneIdleTimer.isActive();
#endif
        // HID encoder frequency tuning routes through applyFlexControlWheelAction,
        // so m_flexCoalesceTimer above already covers it.
        const bool memoryRevealPending = (m_pendingMemoryRevealSliceId == s->sliceId());
        SliceModel* active = m_radioModel.slice(m_activeSliceId);
        const bool activeDiversityPartner = isSameDiversityReceivePair(active, s);
        SliceModel* dragTarget = m_radioModel.slice(m_sliceDragTargetSliceId);
        const bool dragDiversityPartner = isSameDiversityReceivePair(dragTarget, s);
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        const bool dragEchoHoldActive =
            m_sliceDragInProgress
            || (m_sliceDragEchoHoldUntilMs > 0 && nowMs < m_sliceDragEchoHoldUntilMs);
        const bool dragTargetSlice =
            m_sliceDragTargetSliceId >= 0
            && (s->sliceId() == m_sliceDragTargetSliceId || dragDiversityPartner);
        if (dragEchoHoldActive && dragTargetSlice
            && m_sliceDragTargetMhz > 0.0 && !memoryRevealPending) {
            const int sliceId = s->sliceId();
            QTimer::singleShot(0, this, [this, sliceId]() {
                const double displayMhz = m_sliceDragTargetMhz;
                if (displayMhz <= 0.0) {
                    return;
                }
                if (!m_sliceDragInProgress
                    && QDateTime::currentMSecsSinceEpoch() >= m_sliceDragEchoHoldUntilMs) {
                    return;
                }
                SliceModel* slice = m_radioModel.slice(sliceId);
                if (!slice) {
                    return;
                }
                pushSliceFrequencyToOverlays(slice, displayMhz);
            });
            return;
        }
        if (activeTuning
            && (s->sliceId() == m_activeSliceId || activeDiversityPartner)
            && !memoryRevealPending) {
            return;
        }

        m_updatingFromModel = true;
        pushSliceFrequencyToOverlays(s, mhz);
        m_updatingFromModel = false;
        if (s->isTxSlice())
            syncTxWaterfallSliceToSpectrums();

        if (memoryRevealPending) {
            const double targetMhz = m_pendingMemoryRevealTargetMhz;
            if (memoryRevealTargetMatches(mhz, targetMhz)) {
                m_pendingMemoryRevealSliceId = -1;
                m_pendingMemoryRevealTargetMhz = 0.0;
                const double revealMhz = targetMhz > 0.0 ? targetMhz : mhz;
                const TuneCenteringResult result =
                    revealFrequencyIfNeeded(s, revealMhz,
                                            TuneIntent::CommandedTargetCenter,
                                            "memory-apply");
                logTunePolicyDecision("memory-apply", TuneIntent::CommandedTargetCenter,
                                      mhz, revealMhz, result);
            } else {
                qCDebug(lcProtocol).noquote().nospace()
                    << "MainWindow: deferring memory reveal for intermediate echo slice="
                    << s->sliceId()
                    << " echo_mhz=" << QString::number(mhz, 'f', 6)
                    << " target_mhz=" << QString::number(targetMhz, 'f', 6);
            }
        }

        if (s->isTxSlice() || s->sliceId() == m_swrSweep.sliceId) {
            clearSwrSweepForBandChange(s->sliceId(), s->panId(),
                                       BandSettings::bandForFrequency(mhz));
        }

        // Feed frequency to Antenna Genius for band→antenna recall
        if (s->sliceId() == m_activeSliceId)
            m_antennaGenius.setRadioFrequency(mhz);

#ifdef HAVE_HIDAPI
        if (s->sliceId() == m_activeSliceId)
            updateTMate2Display();
#endif
    });

    // Feed current frequency immediately (AG may connect later and reprocess).
    if (s->sliceId() == m_activeSliceId && s->frequency() > 0.0)
        m_antennaGenius.setRadioFrequency(s->frequency());

    connect(s, &SliceModel::filterChanged, this, [this, s](int lo, int hi) {
        auto* sw = spectrumForSlice(s);
        if (!sw) return;
        // Skip overlay update while user is dragging a filter edge — the radio's
        // status echo would overwrite the drag position, causing snap-to-zero (#764)
        if (sw->isDraggingFilter()) return;
        sw->setSliceOverlay(s->sliceId(), s->frequency(),
            lo, hi, s->isTxSlice(), s->sliceId() == m_activeSliceId,
            s->mode(), s->rttyMark(), s->rttyShift(),
            s->ritOn(), s->ritFreq(), s->xitOn(), s->xitFreq(),
            s->diversity(), s->isDiversityParent(),
            s->isDiversityChild(), s->diversityIndex());
        if (s->isTxSlice())
            syncTxWaterfallSliceToSpectrums();
    });
    // Squelch threshold line on the spectrum overlay. Flex follows the active
    // slice only; Kiwi replacement audio owns an independent line on its own
    // pan even when a Flex slice is active elsewhere.
    connect(s, &SliceModel::squelchChanged, this, [this, s](bool on, int level) {
        Q_UNUSED(on);
        Q_UNUSED(level);
        if (s != activeSlice()) return;
        syncActiveSliceSquelchLineToSpectrums();
    });
    connect(s, &SliceModel::externalReceiveSquelchChanged,
            this, [this, s](bool on, int level) {
        const bool kiwiAssigned = m_kiwiSdrManager
            && !m_kiwiSdrManager->assignedProfileForSlice(s->sliceId()).isEmpty();
        if (!kiwiAssigned) {
            return;
        }
        updateKiwiSdrVirtualReceiverControlsForSlice(s);
        if (SpectrumWidget* sw = spectrumForSlice(s)) {
            sw->setKiwiSdrSquelchLine(
                on, kiwiSdrSquelchMarginDbForSliceLevel(s, level),
                s->externalReceiveAutoSquelchOn());
        }
        if (s == activeSlice()) {
            syncActiveSliceSquelchLineToSpectrums();
        }
    });
    if (s == activeSlice()) {
        syncActiveSliceSquelchLineToSpectrums();
    } else if (auto* sw = spectrumForSlice(s)) {
        sw->setSquelchLine(s->squelchOn(), s->squelchLevel());
    }

    connect(s, &SliceModel::txSliceChanged, this, [this, s](bool tx) {
        // Update hasTxSlice on all spectrums for waterfall freeze logic
        if (tx) {
            for (auto* pan : m_radioModel.panadapters()) {
                if (auto* sw = m_panStack ? m_panStack->spectrum(pan->panId()) : nullptr)
                    sw->setHasTxSlice(sw == spectrumForSlice(s));
            }
            if (!m_panStack && m_panApplet)
                m_panApplet->spectrumWidget()->setHasTxSlice(true);
        }
        if (auto* sw = spectrumForSlice(s))
            sw->setSliceOverlay(s->sliceId(), s->frequency(),
                s->filterLow(), s->filterHigh(), tx,
                s->sliceId() == m_activeSliceId,
                s->mode(), s->rttyMark(), s->rttyShift(),
                s->ritOn(), s->ritFreq(), s->xitOn(), s->xitFreq(),
                s->diversity(), s->isDiversityParent(),
                s->isDiversityChild(), s->diversityIndex());
        syncTxWaterfallSliceToSpectrums();
        updateSplitState();

        // TX flag just moved — keyer availability follows the TX slice (#4173).
        updateKeyerAvailability();

        // Active follows TX slice (#1351) — switch the displayed/active slice
        // when an external program (e.g. WSJT-X) moves the TX flag
        if (tx && s->sliceId() != m_activeSliceId
            && AppSettings::instance().value("ActiveFollowsTxSlice", "False").toString() == "True") {
            setActiveSlice(s->sliceId());
        }
    });

    // When the radio notifies us that this slice became active, switch to it.
    // During a band-stack rebuild this is synchronization-only: FLEX can
    // transiently activate the surviving old-band slice, and revealing it
    // would send a stale pan-center command that undoes the selected band.
    connect(s, &SliceModel::activeChanged, this, [this, s](bool active) {
        if (!active) return;
        // Drop only our own optimistic edge, which setActiveSliceInternal
        // brands as it emits. Comparing against m_activeSliceId instead would
        // also drop a radio-originated re-activation of the already-selected
        // slice — another client toggling active away and back, or a profile
        // load replaying status — and that must still resync the UI and
        // reveal the slice, as it did before this guard existed.
        if (s->sliceId() == m_optimisticActiveEdgeSliceId) return;
        selectSliceFromRadioState(
            s, RadioSliceSelectionSource::ActiveStatus);
    });

    // Update filter limits when the active slice's mode changes
    connect(s, &SliceModel::modeChanged, this, [this, s](const QString& mode) {
        if (s->sliceId() == m_activeSliceId)
            updateFilterLimitsForMode(mode);

        // Update spectrum overlay with new mode (for RTTY mark/space lines)
        pushSliceOverlay(s);
        if (s->isTxSlice())
            syncTxWaterfallSliceToSpectrums();

        // Show/hide CW decode panel and start/stop decoder.  Centralised
        // through refreshCwDecodeState() so the RX/TX toggle pair and
        // MOX state share one decision tree (#2417).
        if (s->sliceId() == m_activeSliceId) {
            refreshCwDecodeState();
            refreshRttyDecodeState();

        }
        // The radio supplies one already-mixed RX stream, so the most
        // restrictive audible slice controls the global client DSP state.
        updateAetherDspModePolicy();

        // CWX/DVK availability and their F1-F12 shortcuts follow the TX slice
        // (#4173); the ASR (Copy Assist) indicator follows the ACTIVE slice,
        // because it decodes received audio (#4825). Re-evaluate when either of
        // those two slices changes mode — narrowing this to the TX slice would
        // leave the ASR indicator stale after a CW→USB change on a non-TX
        // active slice, with no other edge to correct it.
        //
        // Through activeSlice(), not an m_activeSliceId comparison: the gate
        // itself calls activeSlice(), which falls back to the first isActive()
        // slice when the cached id's slice is not active. Asking the same
        // question the gate asks keeps the trigger and the gate from diverging
        // inside that fallback window (PR #4932 review).
        if (s->isTxSlice() || activeSlice() == s)
            updateKeyerAvailability();
#ifdef HAVE_RADE
        if (mode.startsWith("FDV"))
            activateFdvDisplay(s->sliceId());
        else if (m_fdvDisplaySliceId == s->sliceId())
            deactivateFdvDisplay();
#endif
    });

    // Update RTTY mark/space lines on spectrum when mark/shift changes;
    // also push new params to the decoder when Auto mark is selected.
    connect(s, &SliceModel::rttyMarkChanged, this, [this, s](int) {
        pushSliceOverlay(s);
        if (s->sliceId() == m_activeSliceId) refreshRttyDecodeState();
    });
    connect(s, &SliceModel::rttyShiftChanged, this, [this, s](int) {
        pushSliceOverlay(s);
        if (s->sliceId() == m_activeSliceId) refreshRttyDecodeState();
    });

    connect(s, &SliceModel::ritChanged, this, [this, s](bool, int) { pushSliceOverlay(s); });
    connect(s, &SliceModel::xitChanged, this, [this, s](bool, int) {
        pushSliceOverlay(s);
        if (s->isTxSlice())
            syncTxWaterfallSliceToSpectrums();
    });

    // Handle slice migration between panadapters
    connect(s, &SliceModel::panIdChanged, this, [this, s](const QString&) {
        // Migration clears the active lock AND the old pan-slot's persisted
        // intent — the letter now lives on another pan; leaving the intent
        // would re-lock a future slice that inherits the letter (#3854
        // review). The remove/re-add body this branch originally carried is
        // superseded by #4037's reattachSliceVisualsToPanadapter.
        clearCenterLockForSlice(s->sliceId(), /*clearPersistedIntent=*/true);
        reattachSliceVisualsToPanadapter(s);
        updateKiwiSdrVirtualTrackingForSlice(s);
        refreshKiwiSdrWaterfallAvailability();
        syncKiwiSdrPanadapterUiStates();
        syncKiwiSdrDiversityEscControls();
    });

    // Create a VfoWidget for this slice on the correct panadapter. When the
    // pan hasn't arrived yet (out-of-order reconnect), skip ONLY the widget
    // creation — reattachSliceVisualsToPanadapter() creates and wires it when
    // the pan lands. The slice-scoped wiring below must still run: it is all
    // pan-independent (every connect resolves the VFO dynamically at fire
    // time), and skipping it left the late-attached slice 90% wired — no
    // applet tab button, no split refresh on external TX-role changes, no
    // band-stack dwell (#4037 review).
    if (auto* swForVfo = spectrumForSlice(s)) {
        auto* vfo = swForVfo->addVfoWidget(s->sliceId());

        // Set SmartSDR+ flag before wireVfoWidget so rebuildFilterButtons
        // sees the correct value when setSlice() triggers the first build (#1356)
        {
            const QString& sub = m_radioModel.licenseSubscription();
            bool hasPlus = sub.contains("SmartSDR+");
            vfo->setSmartSdrPlus(hasPlus);
        }

        // Set extended DSP flag before wireVfoWidget so the mode-change lambda
        // in setSlice() gates NRL/NRS/RNN/NRF visibility correctly (#2177)
        vfo->setHasExtendedDsp(m_radioModel.hasExtendedDspFilters());
        vfo->setHasRadioSideDsp(m_radioModel.hasRadioSideDsp());
        vfo->setHasLmsNoiseFilters(m_radioModel.hasLmsNoiseFilters());
        vfo->setHasManualNotch(m_radioModel.hasManualNotch());
        vfo->setHasHostNoiseBlanker(m_radioModel.hasHostNoiseBlanker());
        vfo->setRadioFilterWidths(m_radioModel.radioFilterWidthsHz());

        wireVfoWidget(vfo, s);

        // NR2/RN2/RADE are now wired permanently in wireVfoWidget — no
        // special handling needed here for active slice timing.

        // Show DIV button on dual-SCU radios (ModelCapabilities table, Principle I)
        vfo->setDiversityAllowed(m_radioModel.isDiversityAllowed());

        wireVfoTelemetry(vfo, s);
    }

#ifdef HAVE_RADE
    // Reconnect scenario: slice may already be in FDVU/FDVL when AetherSDR
    // connects. Pan-independent — runs even when the VFO is deferred.
    if (s->mode().startsWith("FDV"))
        activateFdvDisplay(s->sliceId());
#endif

    // Refresh the split-pair visualization whenever this slice's TX role changes,
    // so an externally-initiated split (rigctld / CAT / TCI / front panel) is
    // reflected on the panadapter, not just GUI-initiated split. (#3726)
    connect(s, &SliceModel::txSliceChanged, this, [this](bool) { updateSplitState(); });

    // Slice Link bookkeeping: the eligible-peer roster follows diversity and
    // letter changes; a member entering diversity dissolves the link (the
    // radio couples diversity pairs itself — G4).
    connect(s, &SliceModel::diversityChanged, this, [this, s](bool on) {
        const int peerId = sliceLinkPeerOf(s->sliceId());
        if (on && peerId >= 0) {
            dissolveSliceLink(s->sliceId(), peerId,
                              "member entered diversity");
        }
        refreshSliceLinkUi();
    });
    connect(s, &SliceModel::letterChanged, this,
            [this](const QString&) { refreshSliceLinkUi(); });

    // Reset band-stack auto-save dwell timer on every active-slice tune
    connect(s, &SliceModel::frequencyChanged, this, [this, s]() {
        if (s->sliceId() != m_activeSliceId) return;
        if (!m_bsAutoSaveTimer) return;
        if (profileLoadRadioStateWritesHeld()) return;
        const int dwellSec = BandStackSettings::instance().autoSaveDwellSeconds();
        if (dwellSec <= 0) return;
        m_bsAutoSaveTimer->start(dwellSec * 1000);
    });

    // If split is pending, this new slice is the TX slice
    if (m_splitActive && m_splitTxSliceId < 0 && s->sliceId() != m_splitRxSliceId) {
        m_splitTxSliceId = s->sliceId();
        s->setTxSlice(true);
        s->setAudioMute(true);  // TX slice in split has no audio output
        // TX slice frequency is already set by the slice create command
        // (with mode-dependent offset), so do NOT override it here (#789).
        if (auto* sw = spectrum()) sw->setSplitPair(m_splitRxSliceId, m_splitTxSliceId);
        updateSplitState();
        // Auto-focus the TX VFO so the user can immediately tune the TX offset
        setActiveSlice(s->sliceId());
    }

    // Refresh slice tab buttons (#1278)
    if (s->isActive() && s->sliceId() != m_activeSliceId) {
        m_updatingFromModel = true;
        setActiveSliceInternal(s->sliceId(), false);
        m_updatingFromModel = false;
    }

    m_appletPanel->updateSliceButtons(m_radioModel.slices(), m_activeSliceId);
    refreshKiwiSdrSlices();
    refreshKiwiSdrWaterfallAvailability();
    syncKiwiSdrDiversityEscControls();

    // A slice can arrive already flagged as the TX slice (e.g. external split
    // created the TX slice out-of-band). Re-derive the split-pair from the model
    // so the panadapter pairs it without a GUI action. (#3726)
    updateSplitState();

    refreshSliceLinkUi();
    updateAetherDspModePolicy();
}

void MainWindow::onSliceRemoved(int id)
{
    if (m_applyingLayout) return;

    qDebug() << "MainWindow: slice removed" << id;

    // #4558: any LIVE removal ends the last-session DAX restore window — from
    // here on, slice adds are mid-session (band-stack recreates included) and
    // must keep their live radio state. The post-reconnect stale-slice prune
    // emits sliceRemoved too but is not live (slice(id) is non-null, same
    // discriminator as the Center Lock guard below) and must not end it.
    // The discriminator is one-directional and its failures are all safe —
    // DaxRestorePolicy::onSliceRemoved() has the enumeration and the argument.
    if (m_daxRestore.onSliceRemoved(/*liveRemoval=*/!m_radioModel.slice(id))) {
        qCDebug(lcDax) << "MainWindow: last-session DAX restore window closed"
                          " (live slice removal, id" << id << ")";
    }

    // Center Lock (#3854 review): a LIVE removal normally clears the persisted
    // letter intent too — Flex recycles letters lowest-free, so a dormant
    // intent would silently re-lock a later, unrelated slice. A band-stack
    // recall is the exception: band_persistence deliberately rebuilds the
    // pan's slices, and the positive per-pan recall marker keeps intent alive
    // until the bounded window can resolve the complete topology. Two guards
    // ride on m_radioModel.slice(id):
    //  - the stale-slice prune after reconnect emits sliceRemoved(oldId)
    //    whose id collides with the NEW session's slices (slice 0 is always
    //    0). Live removals emit AFTER the slice leaves the model, so a
    //    non-null slice(id) means this is the prune — the active lock (and
    //    intent) belong to the new slice; skip entirely.
    //  - disconnect teardown never emits sliceRemoved, so reconnect/restart
    //    intent survives — that is the restore feature.
    if (!m_radioModel.slice(id)) {
        bool preservePersistedIntent = false;
        for (auto it = m_centerLockSliceByPan.cbegin();
             it != m_centerLockSliceByPan.cend(); ++it) {
            if (it.value() == id && m_centerLockRebind.onSliceRemoved(it.key(), id)) {
                preservePersistedIntent = true;
                break;
            }
        }
        clearCenterLockForSlice(id, /*clearPersistedIntent=*/!preservePersistedIntent);
    }
    m_centerLockTuneHoldBySlice.remove(id);

    // Slice Link: dissolve when a member is genuinely going away, using the
    // same live-vs-prune reasoning as the Center Lock block above but keyed
    // on object identity — on the post-reconnect stale prune the id already
    // names a live new-session slice, so only dissolve when that live slice
    // is NOT the object the link holds (i.e. the prune targets our member).
    if (SliceLinkState* link = sliceLinkFor(id)) {
        const int aId = link->aId;
        const int bId = link->bId;
        SliceModel* live = m_radioModel.slice(id);
        SliceModel* member = (id == aId) ? link->a.data() : link->b.data();
        if (!live || live != member) {
            dissolveSliceLink(aId, bId, "member slice removed");
        }
    }
    refreshSliceLinkUi();

    // A band-stack recall (band_persistence) DROPS and RE-CREATES the slice
    // (same id, new band) instead of retuning it, which would tear down a
    // KiwiSDR replacement on the removal. Defer that teardown across the
    // remove->re-add so onSliceAdded can re-bind the Kiwi (retuned to the new
    // band) instead of reverting to a plain Flex slice (#4158). m_kiwiRebind is
    // the pure policy — see KiwiRebindTracker; identity is gated on band-recall
    // intent and generation, not slice id + time.
    //
    // `live` distinguishes a genuine removal (the slice has already left the
    // model, so slice(id) is null) from the reconnect stale-slice prune (a
    // sliceRemoved(oldId) whose id already belongs to a live new-session slice)
    // — same guard the center-lock logic above uses. Only a live Kiwi removal
    // defers; the prune keeps the pre-existing immediate cleanup and never
    // enters the grace path, so a stale prune can't later clear a live
    // assignment.
    const bool liveRemoval = !m_radioModel.slice(id);
    // Same reconstruction evidence as onSliceAdded, but a live removal cannot
    // name its pan — the slice has already left the model (see the comment on
    // the overlay cleanup below). Refresh every live window; see
    // BandRecallSelectionGuard::refreshAll for why that is the safe read.
    if (liveRemoval) {
        m_bandRecallSelection.refreshAll(QDateTime::currentMSecsSinceEpoch());
    }
    const QString kiwiRebindProfile = (liveRemoval && m_kiwiSdrManager)
        ? m_kiwiSdrManager->assignedProfileForSlice(id) : QString();
    const auto rebindAction =
        m_kiwiRebind.onSliceRemoved(id, liveRemoval, kiwiRebindProfile);
    if (rebindAction.kind == KiwiRebindTracker::RemoveAction::Defer) {
        // Leave the assignment and Kiwi connection intact during the window so
        // a re-bind is seamless. The pre-Kiwi mute may already have been
        // consumed by the band-recall preparation; the recreated slice then
        // snapshots its recalled band's own mute. If no recreation re-binds
        // it, the generation-safe expiry finalizes the teardown.
        const quint64 generation = rebindAction.generation;
        QTimer::singleShot(kBandRecallRecreateGraceMs, this, [this, id, generation]() {
            if (!m_kiwiRebind.onGraceExpired(id, generation)) {
                return;
            }
            m_kiwiSdrBandRecallPreparations.remove(id);
            m_kiwiSdrVirtualPreviousMute.remove(id);
            if (m_kiwiSdrManager) {
                m_kiwiSdrManager->clearSliceAssignment(id);
            }
            syncKiwiSdrPanadapterUiStates();
        });
    } else {
        m_kiwiSdrBandRecallPreparations.remove(id);
        m_kiwiSdrVirtualPreviousMute.remove(id);
        if (m_kiwiSdrManager) {
            m_kiwiSdrManager->clearSliceAssignment(id);
        }
    }
    syncKiwiSdrPanadapterUiStates();

    // Drop the adaptive-filter engine's per-slice smoothing/baseline state.
    // processFrame() only self-clears state for slices it still sees; a deleted
    // slice is never seen again, so without this a recreated slice reusing the
    // same id inherits the closed slice's baseline/fit (RFC #3878).
    if (m_adaptiveFilterEngine)
        m_adaptiveFilterEngine->resetSlice(id);

#ifdef HAVE_RADE
    // If the RADE slice was closed, deactivate RADE
    if (id == m_radeSliceId)
        deactivateRADE();
    // If the FreeDV display slice was closed, deactivate the FDV display
    if (id == m_fdvDisplaySliceId)
        deactivateFdvDisplay();
#endif

    // If the split TX slice was closed, disable split
    if (m_splitActive && id == m_splitTxSliceId) {
        // TX slice removed out-of-band (2nd client / front panel / rigctld),
        // not via the in-app SPLIT toggle. Reclaim TX onto the former RX slice
        // explicitly (mirror disableSplit). activeSlice() is wrong here:
        // onSliceAdded() auto-focuses the new TX slice, so the active slice is
        // the one just removed (already gone from the model -> nullptr, TX is
        // never reclaimed) or, if a third slice was focused, the WRONG slice,
        // which would be keyed via the radio command "slice set N tx=1".
        const int rxId = m_splitRxSliceId;   // capture before reset
        m_splitActive = false;
        m_splitRxSliceId = -1;
        m_splitTxSliceId = -1;
        if (auto* sw = spectrum()) sw->setSplitPair(-1, -1);
        if (auto* rx = m_radioModel.slice(rxId))
            rx->setTxSlice(true);
        updateSplitState();
    }

    // Remove overlay from all panadapter spectrums (slice model is already gone,
    // so we can't look up which pan it was on)
    if (m_panStack) {
        for (auto* pan : m_radioModel.panadapters()) {
            if (auto* sw = m_panStack->spectrum(pan->panId())) {
                sw->removeSliceOverlay(id);
                sw->removeVfoWidget(id);
            }
        }
    }
    // Clean slice overlay and VFO widget from all spectrums
    for (auto* a : m_panStack->allApplets()) {
        a->spectrumWidget()->removeSliceOverlay(id);
        a->spectrumWidget()->removeVfoWidget(id);
    }

    // Update pan title bars — show the first remaining slice on each pan,
    // or clear the title if the pan has no slices left.
    if (m_panStack) {
        for (auto* applet : m_panStack->allApplets()) {
            bool found = false;
            for (auto* sl : m_radioModel.slices()) {
                if (sl->panId() == applet->panId()) {
                    applet->setSliceId(sl->sliceId(), sl->letter());
                    found = true;
                    break;
                }
            }
            if (!found)
                applet->clearSliceTitle();
        }
    }

    // Reset panadapter state so display settings re-sync after profile load
    m_radioModel.resetPanState();
    m_needAudioStream = true;

    // If the removed slice was active, switch to the first remaining slice
    if (id == m_activeSliceId) {
        m_appletPanel->setSlice(nullptr);
        if (auto* sw = spectrum()) sw->overlayMenu()->setSlice(nullptr);

        const auto& slices = m_radioModel.slices();
        if (!slices.isEmpty()) {
            selectSliceFromRadioState(
                slices.first(),
                RadioSliceSelectionSource::TopologyFallback);
        } else {
            m_activeSliceId = -1;
            if (m_ax25HfPacketDecodeDialog)
                m_ax25HfPacketDecodeDialog->setAttachedSlice(nullptr);
            refreshMiniPanFollow();   // no active slice → blank the mini-pan readout/passband
            // The last slice is gone, so every indicator's slice is gone with
            // it. The re-select branch above reaches updateKeyerAvailability()
            // through setActiveSliceInternal(); this branch has no such call,
            // and without one the row keeps whatever state the departed slice
            // left it in.
            //
            // Refresh only — deliberately NOT a teardown. The gate leaves an
            // open Copy Assist panel alone on a null slice, because this branch
            // is on the band-recall path: with band_persistence the radio drops
            // and re-creates the slice under the same id (KiwiRebindTracker.h,
            // #4158), and a single-slice setup passes through here on every band
            // change (#4932 review).
            updateKeyerAvailability();
        }
    }

    // Refresh slice tab buttons (#1278)
    m_appletPanel->updateSliceButtons(m_radioModel.slices(), m_activeSliceId);
    refreshKiwiSdrSlices();
    refreshKiwiSdrWaterfallAvailability();
    syncKiwiSdrDiversityEscControls();

    // Removing one half of a split pair (e.g. external controller deletes the TX
    // slice) must re-derive the panadapter split-pair from the model. (#3726)
    updateSplitState();
    updateAetherDspModePolicy();
}

void MainWindow::beginProfileLoadRadioStateWriteHold(const QString& profileType,
                                                     const QString& profileName)
{
    if (!profileLoadMayRebuildRadioTopology(profileType)) {
        qCDebug(lcProtocol).noquote()
            << "MainWindow: profile load does not require topology write hold"
            << QStringLiteral("type=%1").arg(profileType)
            << QStringLiteral("name=%1").arg(profileName);
        return;
    }

    const qint64 untilMs = QDateTime::currentMSecsSinceEpoch() + kProfileLoadStateWriteHoldMs;
    m_profileLoadRadioStateWriteHoldUntilMs =
        std::max(m_profileLoadRadioStateWriteHoldUntilMs, untilMs);
    m_suppressStartupPanLayoutRearrange = false;
    m_layoutRestoreUntilMs = kPanLayoutRestoreWaitingForFirstPan;
    if (m_layoutRestoreTimer) {
        m_layoutRestoreTimer->stop();
    }
    if (m_bsAutoSaveTimer) {
        m_bsAutoSaveTimer->stop();
    }
    m_pendingProfileLoadPanDimensions.clear();
    m_profileLoadPendingFftYpixels.clear();
    m_profileLoadPanDimensionsSettlingUntilMs.clear();
    holdNoiseFloorAutoAdjustForProfileLoad(untilMs);
    if (m_panStack) {
        for (PanadapterApplet* applet : m_panStack->allApplets()) {
            if (applet && applet->spectrumWidget()) {
                applet->spectrumWidget()->clearDisplay();
            }
        }
    }
    setPanadapterConnectionAnimation(true, "Loading profile...");

    qCInfo(lcProtocol).noquote()
        << "MainWindow: holding profile-owned radio state writes"
        << QStringLiteral("type=%1").arg(profileType)
        << QStringLiteral("name=%1").arg(profileName);
}

bool MainWindow::profileLoadRadioStateWritesHeld() const
{
    return QDateTime::currentMSecsSinceEpoch() < m_profileLoadRadioStateWriteHoldUntilMs;
}

bool MainWindow::profileLoadPanDisplaySettling(const QString& panId) const
{
    return m_pendingProfileLoadPanDimensions.contains(panId)
        || m_profileLoadPendingFftYpixels.contains(panId)
        || m_profileLoadPanDimensionsSettlingUntilMs.contains(panId);
}

void MainWindow::holdNoiseFloorAutoAdjustForProfileLoad(qint64 untilMs)
{
    // Do not let auto noise-floor reposition the client-side dBm scale while a
    // profile is rebuilding pans. The radio owns profile dBm ranges; auto floor
    // can reacquire only after those ranges and deferred xpixels/ypixels settle.
    if (!m_panStack) {
        return;
    }

    for (PanadapterApplet* applet : m_panStack->allApplets()) {
        if (!applet || !applet->spectrumWidget()) {
            continue;
        }
        applet->spectrumWidget()->suspendNoiseFloorAutoAdjustUntil(untilMs);
    }
}

void MainWindow::reacquireNoiseFloorLocksAfterProfileLoad()
{
    if (m_shuttingDown || !m_radioModel.isConnected() || !m_panStack) {
        return;
    }

    for (PanadapterApplet* applet : m_panStack->allApplets()) {
        if (!applet || !applet->spectrumWidget()) {
            continue;
        }

        SpectrumWidget* sw = applet->spectrumWidget();
        if (auto* pan = m_radioModel.panadapter(applet->panId())) {
            if (pan->panStreamId() && m_radioModel.panStream()) {
                m_radioModel.panStream()->setDbmRange(
                    pan->panStreamId(), pan->minDbm(), pan->maxDbm());
            }
            if (!sw->noiseFloorAutoAdjustEnabled()) {
                sw->setDbmRange(pan->minDbm(), pan->maxDbm());
            }
        }
        if (sw->noiseFloorAutoAdjustEnabled()) {
            sw->resumeNoiseFloorAutoAdjust();
        } else {
            sw->reacquireNoiseFloorLock();
        }
    }
}

void MainWindow::releaseProfileLoadPanDisplayHold(const QString& panId, SpectrumWidget* sw)
{
    if (panId.isEmpty()) {
        return;
    }

    m_pendingProfileLoadPanDimensions.remove(panId);
    m_profileLoadPendingFftYpixels.remove(panId);
    m_profileLoadPanDimensionsSettlingUntilMs.remove(panId);
    if (sw) {
        sw->resumeNoiseFloorAutoAdjust();
    }
}

void MainWindow::markProfileLoadPanDimensionsReady(const QString& panId, int yPixels)
{
    auto expectedIt = m_profileLoadPendingFftYpixels.find(panId);
    if (expectedIt == m_profileLoadPendingFftYpixels.end()
        || expectedIt.value() != yPixels) {
        return;
    }

    releaseProfileLoadPanDisplayHold(
        panId,
        m_panStack ? m_panStack->spectrum(panId) : nullptr);
}

bool MainWindow::profileLoadPanDimensionsMatchExpected(const QString& panId,
                                                       SpectrumWidget* sw) const
{
    auto expectedIt = m_profileLoadPendingFftYpixels.find(panId);
    if (expectedIt == m_profileLoadPendingFftYpixels.end()) {
        return true;
    }

    return sw
        && panPixelDimensionsReady(sw)
        && panYpixelsFor(sw) == expectedIt.value();
}

void MainWindow::retryProfileLoadPanDimensions(const QString& panId, SpectrumWidget* sw)
{
    if (panId.isEmpty() || !sw) {
        return;
    }

    m_profileLoadPendingFftYpixels.remove(panId);
    m_profileLoadPanDimensionsSettlingUntilMs.remove(panId);

    if (!panPixelDimensionsReady(sw)) {
        m_pendingProfileLoadPanDimensions.insert(panId);
        return;
    }

    m_profileLoadPendingFftYpixels.insert(panId, panYpixelsFor(sw));
    m_profileLoadPanDimensionsSettlingUntilMs.insert(
        panId,
        QDateTime::currentMSecsSinceEpoch() + kProfileLoadDimensionSettleMs);
    sendPanDimensionsToRadio(panId, sw, false);
}

void MainWindow::sendPanDimensionsToRadio(const QString& panId,
                                          SpectrumWidget* sw,
                                          bool updateLocalDecoderImmediately)
{
    // Raw sender for radio FFT dimensions. Normal lifecycle/resize paths must
    // call requestPanDimensionsForRadio() instead so profile loads can defer
    // these writes; sending xpixels/ypixels while the radio is rebuilding a
    // profile can make the radio autosave a partial GUIClient slice layout.
    if (panId.isEmpty() || !sw || !panPixelDimensionsReady(sw)) {
        return;
    }

    auto* pan = m_radioModel.panadapter(panId);
    if (!pan) {
        return;
    }

    const int xpix = panXpixelsFor(sw);
    const int ypix = panYpixelsFor(sw);
    m_radioModel.sendCommand(
        QString("display pan set %1 xpixels=%2 ypixels=%3")
            .arg(panId).arg(xpix).arg(ypix));

    // Arm the DSS settle gate now, before the radio echo switches the local
    // decoder. The stream keeps decoding with the old y_pixels until the echo,
    // so rows arriving in the request→echo window can be decoded against a stale
    // scale; since retained 3D history is now preserved across a resize (rather
    // than cleared on echo), those mis-scaled rows would otherwise survive in
    // scrollback. Dropping them here costs only a few valid rows and matches the
    // pre-preservation intent that pre-echo rows must not persist. Only for a
    // genuine transition off an established scale — a width-only resize or first
    // dimension push (fftYPixels()==0) has no prior history to protect.
    if (pan->fftYPixels() > 0 && pan->fftYPixels() != ypix) {
        sw->beginFftPixelScaleSettle();
    }

    if (profileLoadRadioStateWritesHeld()) {
        m_profileLoadPendingFftYpixels.insert(panId, ypix);
        m_profileLoadPanDimensionsSettlingUntilMs.insert(
            panId,
            QDateTime::currentMSecsSinceEpoch() + kProfileLoadDimensionSettleMs);
    }

    const quint32 streamId = pan->panStreamId();
    if (streamId) {
        QPointer<SpectrumWidget> swGuard(sw);
        auto updateLocalDecoder = [this, panId, swGuard, streamId, ypix]() {
            if (m_shuttingDown || !swGuard) {
                return;
            }
            auto* currentPan = m_radioModel.panadapter(panId);
            if (!currentPan || currentPan->panStreamId() != streamId) {
                return;
            }
            if (!panPixelDimensionsReady(swGuard.data())
                || panYpixelsFor(swGuard.data()) != ypix) {
                return;
            }
            // A radio status echo updates both the model and stream decoder.
            // Do not run the delayed fallback afterward: besides being
            // redundant, it would restart the DSS scale-settle window for a
            // width-only resize whose y_pixels never changed.
            if (currentPan->fftYPixels() == ypix) {
                return;
            }
            // Telling the radio the new scale is Flex-only — a non-Flex backend
            // (HL2) owns no PanadapterStream (#4448) — but the local widget
            // rescale below must still happen.
            if (m_radioModel.panStream()) {
                m_radioModel.panStream()->setYPixels(streamId, ypix);
            }
            swGuard->prepareForFftPixelScaleChange();
        };

        if (updateLocalDecoderImmediately) {
            updateLocalDecoder();
        } else {
            // Radio status echo is the normal sync point. The delayed fallback
            // preserves resize recovery if the echo is missed, without decoding
            // queued old-height FFT frames with the new height.
            QTimer::singleShot(kPanDimensionDecoderFallbackMs,
                               this,
                               updateLocalDecoder);
        }
    }
}

void MainWindow::requestPanDimensionsForRadio(const QString& panId,
                                              SpectrumWidget* sw,
                                              bool updateLocalDecoderImmediately)
{
    if (panId.isEmpty() || !sw || !panPixelDimensionsReady(sw)) {
        return;
    }

    // Pixel dimensions are client-owned display metadata, but sending them
    // while the radio is tearing down/rebuilding a profile can dirty the
    // GUIClient restore snapshot with an intermediate topology. That was the
    // root cause of profile-loaded sessions later restarting with missing
    // slices. Keep all pan size pushes on this path so they are deferred and
    // coalesced until the profile load has been accepted and settled.
    if (profileLoadRadioStateWritesHeld()) {
        m_pendingProfileLoadPanDimensions.insert(panId);
        qCDebug(lcProtocol).noquote()
            << "MainWindow: deferring pan dimensions during profile load"
            << QStringLiteral("pan=%1").arg(panId);
        return;
    }

    sendPanDimensionsToRadio(panId, sw, updateLocalDecoderImmediately);
}

void MainWindow::flushPendingProfileLoadPanDimensions()
{
    if (m_shuttingDown || !m_radioModel.isConnected()
            || m_pendingProfileLoadPanDimensions.isEmpty() || !m_panStack) {
        return;
    }

    const QSet<QString> pending = m_pendingProfileLoadPanDimensions;
    m_pendingProfileLoadPanDimensions.clear();

    int sent = 0;
    for (PanadapterApplet* applet : m_panStack->allApplets()) {
        if (!applet || !pending.contains(applet->panId())) {
            continue;
        }

        sendPanDimensionsToRadio(applet->panId(), applet->spectrumWidget(), false);
        ++sent;
    }

    if (sent > 0) {
        qCDebug(lcProtocol).noquote()
            << "MainWindow: flushed deferred profile-load pan dimensions"
            << QStringLiteral("count=%1").arg(sent);
    }
}

void MainWindow::scheduleProfileLoadRecovery(const QString& profileType,
                                             const QString& profileName)
{
    if (!profileLoadMayRebuildRadioTopology(profileType)) {
        qCInfo(lcProtocol).noquote()
            << "MainWindow: scheduling lightweight profile-load recovery"
            << QStringLiteral("type=%1").arg(profileType)
            << QStringLiteral("name=%1").arg(profileName);
        QTimer::singleShot(350, this, [this, profileType, profileName]() {
            runProfileLoadRecoveryPass(profileType, profileName, false, false);
        });
        return;
    }

    const qint64 untilMs = QDateTime::currentMSecsSinceEpoch() + kProfileLoadStateWriteHoldMs;
    m_profileLoadRadioStateWriteHoldUntilMs =
        std::max(m_profileLoadRadioStateWriteHoldUntilMs, untilMs);
    holdNoiseFloorAutoAdjustForProfileLoad(m_profileLoadRadioStateWriteHoldUntilMs);

    qCInfo(lcProtocol).noquote()
        << "MainWindow: scheduling profile-load recovery"
        << QStringLiteral("type=%1").arg(profileType)
        << QStringLiteral("name=%1").arg(profileName);

    QTimer::singleShot(350, this, [this, profileType, profileName]() {
        runProfileLoadRecoveryPass(profileType, profileName, false, true);
    });
    QTimer::singleShot(1200, this, [this, profileType, profileName]() {
        runProfileLoadRecoveryPass(profileType, profileName, true, false);
    });
    QTimer::singleShot(2500, this, [this, profileType, profileName]() {
        runProfileLoadRecoveryPass(profileType, profileName, false, false);
    });
    QTimer::singleShot(1500, this, [this]() {
        flushPendingProfileLoadPanDimensions();
    });
    QTimer::singleShot(3500, this, [this]() {
        flushPendingProfileLoadPanDimensions();
    });
    QTimer::singleShot(kProfileLoadDeferredPanFlushDelayMs, this, [this]() {
        flushPendingProfileLoadPanDimensions();
        // Deferred pan WRITES (center/bandwidth/band) are NOT flushed from
        // here. RadioModel owns that replay — hold-relative, armed by the act
        // of deferring — because this entire recovery pass is gated on the
        // profile-load ACK, and a large topology (8 pans / 8 slices, measured
        // 5/5 on a 6700) can stall the radio into a force-disconnect before
        // the ACK ever arrives; a flush scheduled here would never run. A
        // dimensions-then-centers ordering is NOT required for correctness:
        // every VITA-49 tile carries its own FrameLowFreq/BinBandwidth, so a
        // recenter that lands before the pixel geometry settles is a
        // self-correcting transient, not a scar. (#4142)
    });
    QTimer::singleShot(kProfileLoadPostHoldRecoveryDelayMs, this, [this]() {
        m_profileLoadPendingFftYpixels.clear();
        m_profileLoadPanDimensionsSettlingUntilMs.clear();
        reacquireNoiseFloorLocksAfterProfileLoad();
#ifdef HAVE_WEBSOCKETS
        if (tciServer()) {
            tciServer()->rearmDaxForProfileLoad();
        }
#endif
    });
}

void MainWindow::runProfileLoadRecoveryPass(const QString& profileType,
                                            const QString& profileName,
                                            bool rearmDaxIq,
                                            bool resetDaxRxStreams)
{
    Q_UNUSED(profileType);
    Q_UNUSED(profileName);

    if (m_shuttingDown || !m_radioModel.isConnected()) {
        return;
    }

    // Keep this pass to client-owned sessions. A profile load can rewrite
    // radio-owned pan/slice topology; pushing display pan state here can dirty
    // the radio's GUIClient restore snapshot while it is settling.

    SliceModel* referenceSlice = m_radioModel.slice(m_activeSliceId);
    if (!referenceSlice && !m_radioModel.slices().isEmpty()) {
        referenceSlice = m_radioModel.slices().first();
    }
    if (referenceSlice && referenceSlice->frequency() > 0.0) {
        const QString bandName = BandSettings::bandForFrequency(referenceSlice->frequency());
        if (!bandName.isEmpty()) {
            m_bandSettings.setCurrentBand(bandName);
        }
        if (referenceSlice->sliceId() == m_activeSliceId) {
            m_antennaGenius.setRadioFrequency(referenceSlice->frequency());
        }
    }

    if (AppSettings::instance().value("PcAudioEnabled", "True").toString() == "True") {
        audioStartRx();
    }

#ifdef HAVE_WEBSOCKETS
    if (resetDaxRxStreams && tciServer() && !profileLoadRadioStateWritesHeld()) {
        tciServer()->rearmDaxForProfileLoad();
    }
#endif

#if defined(Q_OS_MAC) || defined(HAVE_PIPEWIRE)
    if (m_daxBridge) {
        auto* panStream = m_radioModel.panStream();
        bool txSliceIsDigital = false;

        // Post-profile-load reconcile (#3305): reseed the slice→channel shadow
        // and (re)acquire whatever the restored slices carry. The manager
        // dedupes against live streams, and streams the radio destroyed during
        // the load are re-created automatically by its removed-status recreate
        // path — no manual stream remove/create here anymore. Channels held by
        // TCI or RADE are simply co-held; nothing is stomped.
        for (auto* slice : m_radioModel.slices()) {
            if (!slice) {
                continue;
            }
            if (!m_radioModel.sliceMayBelongToUs(slice->sliceId())) {
                continue;
            }

            if (slice->isTxSlice()) {
                const QString mode = slice->mode();
                txSliceIsDigital = (mode == "DIGU" || mode == "DIGL" || mode == "RTTY"
                                  || mode == "DFM"  || mode == "NFM"  || mode == "NT");
            }

            const int channel = slice->daxChannel();
            m_daxSliceLastCh[slice->sliceId()] = channel;
            if (channel < 1 || channel > 8) {
                continue;
            }
            if (panStream) {
                panStream->acquireDaxChannel(
                    channel, PanadapterStream::DaxConsumer::Bridge);
            }
        }

        m_audio->setDaxTxMode(txSliceIsDigital);
        if (txSliceIsDigital && m_radioModel.transmitModel().daxOn()) {
            m_radioModel.ensureDaxTxStream(DaxTxRequestReason::HostedDaxBridge);
        }
    }
#endif

    if (rearmDaxIq) {
        auto& settings = AppSettings::instance();
        for (int channel = 1; channel <= 4; ++channel) {
            if (settings.value(QStringLiteral("DaxIqEnabled%1").arg(channel), "False").toString() != "True") {
                continue;
            }
            const DaxIqModel::IqStream stream = m_radioModel.daxIqModel().stream(channel);
            if (stream.exists && stream.streamId != 0) {
                if (m_radioModel.panStream()) {
                    m_radioModel.panStream()->unregisterIqStream(stream.streamId);
                }
                m_radioModel.daxIqModel().removeStream(channel);
                m_radioModel.daxIqModel().handleStreamRemoved(stream.streamId);
            }
            m_radioModel.daxIqModel().createStream(channel);
            const int rate = settings.value(QStringLiteral("DaxIqRate%1").arg(channel), "48000").toInt();
            QTimer::singleShot(600, this, [this, channel, rate]() {
                if (m_radioModel.isConnected()) {
                    m_radioModel.daxIqModel().setSampleRate(channel, rate);
                }
            });
        }
    }
}

void MainWindow::wirePanDisplayStatus(PanadapterApplet* applet,
                                      PanadapterModel* pan)
{
    if (!applet || !pan) {
        return;
    }
    SpectrumWidget* sw = applet->spectrumWidget();
    if (!sw) {
        return;
    }

    const QString panId = applet->panId();
    QVector<QMetaObject::Connection> oldConnections =
        m_panDisplayStatusConnections.take(panId);
    for (const QMetaObject::Connection& connection : oldConnections) {
        QObject::disconnect(connection);
    }

    QVector<QMetaObject::Connection> connections;
    connections.reserve(4);
    connections.append(connect(
        pan, &PanadapterModel::averageReported,
        sw, [sw](int average) {
            if (average >= 0) {
                sw->setFftAverage(average);
            }
        }));
    connections.append(connect(
        pan, &PanadapterModel::weightedAverageReported,
        sw, [sw, pan](bool weighted) {
            // Guard on "known" like the other three fields, so an unreported
            // value doesn't paint a definitive unchecked box (#4261).
            if (pan->weightedAverageKnown()) {
                sw->setFftWeightedAvg(weighted);
            }
        }));
    connections.append(connect(
        pan, &PanadapterModel::fpsReported,
        sw, [this, sw](int fps) {
            // The adaptive cap is transient client transport state: suppress only
            // the cap's own echo so the pre-cap radio value stays the restore
            // target, but let a genuine radio/profile update through even while
            // throttled (#4261 — otherwise it's lost when the throttle lifts).
            if (applyThrottledDisplayReport(m_adaptiveThrottleActive,
                                            m_adaptiveFpsCap, fps)) {
                sw->setFftFps(fps);
            }
        }));
    connections.append(connect(
        pan, &PanadapterModel::waterfallLineDurationReported,
        sw, [this, sw](int lineDurationMs) {
            if (applyThrottledDisplayReport(
                    m_adaptiveThrottleActive,
                    m_radioModel.adaptiveWfRateForCap(m_adaptiveFpsCap),
                    lineDurationMs)) {
                sw->setWfLineDuration(lineDurationMs);
            }
        }));
    m_panDisplayStatusConnections.insert(panId, connections);

    // When the ENGINE shapes the frame rate, the seeding runs the other way:
    // the radio reports no display state, so the pan model has nothing to give
    // the widget and it is the widget's current slider values that the shaper
    // needs. Done before the reads below so those still see a populated model.
    // Without it the stream is shaped to the built-in defaults until the
    // operator happens to touch a slider.
    // Same predicate, told to the widget: it selects which rate-to-cadence law
    // seeds the time axis, and the two disagree by more than 10x in the middle
    // of the slider (core/WaterfallRate.h). Set unconditionally, not inside the
    // branch below — a pan re-wired after switching from a self-shaping backend
    // to a Flex has to be told the law changed back.
    sw->setWfRateShapedLocally(m_radioModel.shapesDisplayRatesLocally());
    if (m_radioModel.shapesDisplayRatesLocally()) {
        m_radioModel.requestPanDisplayRates(panId, sw->fftFps(),
                                            sw->wfLineDuration());
    }

    // Reclaimed pans already hold their latest status and do not necessarily
    // emit a new report after reconnect. Seed the view immediately.
    if (pan->average() >= 0) {
        sw->setFftAverage(pan->average());
    }
    if (pan->weightedAverageKnown()) {
        sw->setFftWeightedAvg(pan->weightedAverage());
    }
    if (applyThrottledDisplayReport(m_adaptiveThrottleActive,
                                    m_adaptiveFpsCap, pan->fps())) {
        sw->setFftFps(pan->fps());
    }
    if (applyThrottledDisplayReport(
            m_adaptiveThrottleActive,
            m_radioModel.adaptiveWfRateForCap(m_adaptiveFpsCap),
            pan->waterfallLineDuration())) {
        sw->setWfLineDuration(pan->waterfallLineDuration());
    }
}

// Display panel → "Clone to all Pans" (sits above Reset to Defaults).
//
// Every value the Display panel can change is written onto every OTHER open
// panadapter. Three kinds of setting live in that panel and each has to travel
// its own way — a single "copy the settings blob" pass would get two of them
// wrong:
//
//   1. Client appearance (trace colours, fill, waterfall palette/blanker,
//      background, grid, 3D view). The SpectrumWidget setters both apply and
//      persist under the target's own per-pan settingsKey(), so calling them is
//      the whole job.
//   2. Client appearance whose setter does NOT persist (background image path
//      and opacity — MainWindow has always owned those writes, see the
//      backgroundImage* handlers). Persisted explicitly here.
//   3. Radio-authoritative values (FFT average / FPS / weighted average,
//      waterfall line duration, color gain, black level). Per AGENTS.md these
//      are never persisted client-side; they are cloned by sending the same
//      commands the live sliders send, so the radio echoes them back as status.
//
// Kiwi-backed pans re-use the waterfall gain/black/rate controls for a different
// quantity (dBm range), so those three are skipped for such a target exactly as
// the live per-control handlers skip them.
int MainWindow::cloneDisplaySettingsToAllPans(PanadapterApplet* source)
{
    if (!source || !m_panStack) {
        return 0;
    }
    SpectrumWidget* src = source->spectrumWidget();
    if (!src) {
        return 0;
    }

    auto& settings = AppSettings::instance();
    // Whether the SOURCE pan is showing Kiwi decides what its waterfall
    // level/rate accessors actually mean — see the waterfall block below.
    const bool srcIsKiwi = kiwiSdrPanDisplaysKiwi(source->panId());
    int cloned = 0;

    for (PanadapterApplet* target : m_panStack->allApplets()) {
        if (!target || target == source) {
            continue;
        }
        SpectrumWidget* dst = target->spectrumWidget();
        if (!dst || dst == src) {
            continue;
        }
        const QString targetPanId = target->panId();
        const bool targetIsKiwi = kiwiSdrPanDisplaysKiwi(targetPanId);

        // ── PANADAPTER ────────────────────────────────────────────────────
        dst->setFftHeatMap(src->fftHeatMap());
        dst->setShowGrid(src->showGrid());
        dst->setFftLineWidth(src->fftLineWidth());
        dst->setFftLineColor(src->fftLineColor());
        dst->setFftFillAlpha(src->fftFillAlpha());
        dst->setFftFillColor(src->fftFillColor());
        // noiseFloorPosition (and dssFloorDepth below) are stored per display
        // source, so these setters write the target's CURRENTLY DISPLAYED
        // source and read the source pan's. That is deliberate: the operator
        // is cloning the look they can see, not a Flex slot and a Kiwi slot.
        // The target's other-source value is left alone rather than
        // overwritten with a position that was never chosen for it.
        dst->setNoiseFloorPosition(src->noiseFloorPosition());
        dst->setNoiseFloorEnable(src->noiseFloorEnabled());

        // ── WATERFALL (client-side) ───────────────────────────────────────
        // The blanker is Flex/Kiwi-agnostic — it filters rows after they land,
        // whatever produced them — so it clones unconditionally. Everything
        // else on this group (auto-black trio, gain, black level, rate) is
        // cloned in the waterfall block further down, which is gated: all five
        // of those live per-control handlers bail on a Kiwi-displaying pan.
        dst->setWfBlankerEnabled(src->wfBlankerEnabled());
        dst->setWfBlankerThreshold(src->wfBlankerThreshold());
        dst->setWfBlankerMode(src->wfBlankerMode());

        // ── BACKGROUND ────────────────────────────────────────────────────
        // "none" is the persisted spelling for background-off; an empty path is
        // its in-memory form. Keep the two in step or a restart resurrects the
        // default logo on a pan the operator turned off.
        const QString bgPath = src->backgroundImagePath();
        dst->setBackgroundImage(bgPath);
        settings.setValue(dst->settingsKey("BackgroundImage"),
                          bgPath.isEmpty() ? QStringLiteral("none") : bgPath);
        dst->setBackgroundOpacity(src->backgroundOpacity());
        settings.setValue(dst->settingsKey("BackgroundOpacity"),
                          QString::number(src->backgroundOpacity()));
        dst->setBackgroundFillColor(src->backgroundFillColor());

        // ── APPEARANCE ────────────────────────────────────────────────────
        dst->setFreqGridSpacing(src->freqGridSpacing());
        dst->setFreqScaleFontPt(src->freqScaleFontPt());
        dst->setWfColorScheme(src->wfColorScheme());

        // ── 3D VIEW ───────────────────────────────────────────────────────
        dst->setSpectrumRenderMode(src->spectrumRenderMode());
        dst->setDssFloorDepth(src->dssFloorDepth());
        dst->setDssGain(src->dssGain());
        dst->setDssRowSpan(src->dssRowSpan());

        // ── Radio-authoritative values ────────────────────────────────────
        // A pan applet exists before the radio hands back its id (setPanId()
        // runs after creation), so a clone racing pan creation would put
        // `display pan set  average=0` — double space, no id — on the wire.
        // requestPanDisplayRates() already returns early on an empty id; these
        // two raw sendCommand() calls are the exposed pair.
        if (!targetPanId.isEmpty()) {
            m_radioModel.sendCommand(QString("display pan set %1 average=%2")
                                         .arg(targetPanId)
                                         .arg(src->fftAverage()));
            m_radioModel.sendCommand(QString("display pan set %1 weighted_average=%2")
                                         .arg(targetPanId)
                                         .arg(src->fftWeightedAvg() ? 1 : 0));
        }
        dst->setFftAverage(src->fftAverage());
        dst->setFftWeightedAvg(src->fftWeightedAvg());
        // Always update the widget's restore target; only ask the radio when the
        // congestion-aware throttle isn't holding the rates down (same rule as
        // the FPS slider — otherwise the clone fights the cap).
        dst->setFftFps(src->fftFps());
        if (!m_adaptiveThrottleActive) {
            m_radioModel.requestPanDisplayRates(targetPanId, src->fftFps(),
                                                /*wfMs=*/0);
        }

        // ── Waterfall level/rate controls ─────────────────────────────────
        // Skipped whenever EITHER end is displaying Kiwi. Target-side is the
        // guard the five live handlers use. Source-side matters just as much:
        // on a Kiwi-displaying pan these sliders drive the profile's dBm
        // min/max through KiwiSdrManager, so wfColorGain()/wfBlackLevel()/
        // wfLineDuration() still hold whatever stale Flex values the widget
        // last had. Cloning those would push a setting the operator never made
        // anywhere onto every Flex pan — and send `display panafall set` for it.
        if (!srcIsKiwi && !targetIsKiwi) {
            dst->setWfAutoBlack(src->wfAutoBlack());
            dst->setWfAutoBlackOffset(src->wfAutoBlackOffset());
            // The operator's stored INTENT, not the capability-masked value —
            // see SpectrumWidget::wfAutoBlackRadioSide(). Cloning the masked
            // value would quietly downgrade every other pan to SW on a radio
            // without HW.
            dst->setWfAutoBlackRadioSide(src->wfAutoBlackRadioSide());
            // No m_radioModel.setWaterfallAutoBlack()/setWaterfallAutoBlackSource()
            // here, unlike the live handlers: those two are radio-GLOBAL, and
            // the source pan's own toggle already set them to these values.
            // Re-sending them once per target would be the same write N times.
            dst->setWfColorGain(src->wfColorGain());
            dst->setWfBlackLevel(src->wfBlackLevel());
            if (auto* pan = m_radioModel.panadapter(targetPanId);
                pan && !pan->waterfallId().isEmpty()) {
                m_radioModel.sendCommand(
                    QString("display panafall set %1 color_gain=%2")
                        .arg(pan->waterfallId())
                        .arg(src->wfColorGain()));
                m_radioModel.sendCommand(
                    QString("display panafall set %1 black_level=%2")
                        .arg(pan->waterfallId())
                        .arg(src->wfBlackLevel()));
            }
            // Clamp like the slider path does (applyWaterfallLineDuration).
            // The source value came through the same clamp today, so this is
            // belt-and-braces — but the two paths should not disagree about
            // what a legal rate is.
            const int wfRate = std::clamp(src->wfLineDuration(),
                                          AetherSDR::WaterfallRate::kMin,
                                          AetherSDR::WaterfallRate::kMax);
            dst->setWfLineDuration(wfRate);
            if (!m_adaptiveThrottleActive) {
                m_radioModel.requestPanDisplayRates(targetPanId, /*fps=*/0,
                                                    wfRate);
            }
        }

        // Repaint the target's own Display panel so an operator who opens it
        // sees the cloned values rather than the pre-clone slider positions.
        // BOTH steps run for a Kiwi target, in this order.
        //
        // syncKiwiSdrPanadapterUiState() alone is not enough: on a pan that is
        // actually displaying Kiwi it falls through to the final block and
        // calls syncKiwiWaterfallSettings() only — its internal
        // syncFlexDisplaySettings() runs on the two EARLY-RETURN paths (no
        // profile, or profile present but not displaying Kiwi). So a Kiwi
        // target would get its WF Ceiling/Floor/Rate refreshed and nothing
        // else: heat map, grid, line width/colour, fill, FFT floor, colour
        // scheme, render mode, 3D and average would all still show the
        // pre-clone positions, and the panel would read as if the clone had
        // not happened.
        //
        // Running the Flex sync first is safe even though it opens with
        // setKiwiWaterfallControlMode(false): syncDisplaySettings() blocks its
        // signals, and the dBm span lives in the profile / KiwiSdrManager
        // rather than in the menu, so syncKiwiWaterfallSettings() re-reads it
        // (profile.waterfallMinDbm/MaxDbm) and the transient 0..100 clamp
        // cannot destroy anything.
        if (SpectrumOverlayMenu* dstMenu = dst->overlayMenu()) {
            dstMenu->syncDisplaySettings(
                dst->fftAverage(), dst->fftFps(),
                static_cast<int>(dst->fftFillAlpha() * 100.0f),
                dst->fftWeightedAvg(), dst->fftFillColor(), dst->wfColorGain(),
                dst->wfBlackLevel(), dst->wfAutoBlack(),
                dst->wfAutoBlackOffset(), dst->wfLineDuration(),
                dst->noiseFloorPosition(), dst->noiseFloorEnabled(),
                dst->fftHeatMap(), dst->wfColorScheme(), dst->showGrid(),
                dst->fftLineWidth(), dst->wfAutoBlackRadioSide(),
                dst->spectrumRenderMode(), dst->dssFloorDepth(),
                dst->dssGain(), dst->fftLineColor(), dst->dssRowSpan());
        }
        if (targetIsKiwi) {
            // Re-enter Kiwi control mode over the Flex sync above: restores the
            // dBm labels and the profile's span on WF Ceiling / WF Floor.
            syncKiwiSdrPanadapterUiState(targetPanId);
        }
        // Kiwi-agnostic controls: syncKiwiSdrPanadapterUiState() does not carry
        // these, so they run for every target.
        if (SpectrumOverlayMenu* dstMenu = dst->overlayMenu()) {
            dstMenu->syncExtraDisplaySettings(
                dst->wfBlankerEnabled(), dst->wfBlankerThreshold(),
                dst->backgroundOpacity(), dst->freqGridSpacing(),
                dst->backgroundFillColor(), dst->freqScaleFontPt());
        }
        ++cloned;
    }

    if (cloned > 0) {
        // The two radio-GLOBAL auto-black flags, asserted ONCE after the loop
        // rather than per target. The per-target block deliberately omits them
        // because they are not per-pan — but "the source pan's own toggle
        // already set them" only holds if the operator toggled it THIS session:
        // on a fresh start the source restores wfAutoBlack /
        // wfAutoBlackRadioSide from AppSettings without ever passing through
        // wfAutoBlackChanged / wfAutoBlackSourceChanged, so the radio-side
        // flags can still be at their defaults. Idempotent, and one write.
        // Radio-side INTENT, not the capability-masked value, for the same
        // reason the per-target clone uses wfAutoBlackRadioSide().
        if (!srcIsKiwi) {
            m_radioModel.setWaterfallAutoBlack(src->wfAutoBlack());
            m_radioModel.setWaterfallAutoBlackSource(src->wfAutoBlackRadioSide());
        }
        settings.save();
    }
    return cloned;
}

void MainWindow::wirePanadapter(PanadapterApplet* applet)
{
    auto* sw = applet->spectrumWidget();
    auto* menu = sw->overlayMenu();
    if (profileLoadRadioStateWritesHeld()) {
        // Profile recall briefly rebuilds pan topology and pixel dimensions.
        // Keep auto noise-floor from sliding the client-side dBm scale during
        // that window; the radio's profile-owned min_dbm/max_dbm status must
        // seed the display first, then auto floor can reacquire afterward.
        sw->suspendNoiseFloorAutoAdjustUntil(m_profileLoadRadioStateWriteHoldUntilMs);
    }

    auto pendingDbm = std::make_shared<DbmRangeTransition::Handshake>();
    auto encoderDbmRange =
        std::make_shared<DbmRangeTransition::Range>();
    auto encoderRecoveryPending = std::make_shared<bool>(false);
    if (auto* pan = m_radioModel.panadapter(applet->panId())) {
        *encoderDbmRange = {pan->minDbm(), pan->maxDbm()};
    }
    auto setStreamDbmRange =
        [this, applet, encoderDbmRange]
        (float minDbm, float maxDbm, bool waitForEcho = false) {
        *encoderDbmRange = {minDbm, maxDbm};
        if (auto* pan = m_radioModel.panadapter(applet->panId())) {
            if (pan->panStreamId() && m_radioModel.panStream()) {
                m_radioModel.panStream()->setDbmRange(pan->panStreamId(), minDbm, maxDbm, waitForEcho);
            }
        }
    };
    auto applyAuthoritativeDbmRange =
        [this, applet, sw, setStreamDbmRange, encoderRecoveryPending]
                                      (const DbmRangeTransition::Range& range) {
        sw->cancelPendingDbmRangeChange();
        if (auto* pan = m_radioModel.panadapter(applet->panId())) {
            if (pan->panStreamId() && m_radioModel.panStream()) {
                m_radioModel.panStream()->cancelPendingDbmRange(pan->panStreamId());
            }
        }
        setStreamDbmRange(range.minDbm, range.maxDbm);
        if (*encoderRecoveryPending) {
            *encoderRecoveryPending = false;
            sw->reacquireNoiseFloorLock();
            return;
        }
        sw->setDbmRange(range.minDbm, range.maxDbm);
        sw->prepareForFftScaleChange();
        sw->reacquireNoiseFloorLock();
    };
    auto reconcileDbmRangeFromModel = [this, applet, pendingDbm,
                                       applyAuthoritativeDbmRange]() {
        if (auto* pan = m_radioModel.panadapter(applet->panId())) {
            const DbmRangeTransition::HandshakeDecision decision =
                pendingDbm->cancelForRadioAuthority(pan->minDbm(), pan->maxDbm());
            if (decision.action == DbmRangeTransition::HandshakeAction::ReconcileRadioRange) {
                applyAuthoritativeDbmRange(decision.range);
            }
        }
    };
    auto retireDbmRangeWithoutEcho =
        [this, applet, sw, encoderRecoveryPending]() {
        // Some Flex firmware accepts a display range command without echoing
        // min_dbm/max_dbm. In that case the decoder is already using the range
        // the radio adopted, so only retire the stale-echo guard; reverting to
        // PanadapterModel here would restore the pre-command range and corrupt
        // every subsequent FFT bin until another status happened to arrive.
        sw->cancelPendingDbmRangeChange();
        if (auto* pan = m_radioModel.panadapter(applet->panId())) {
            if (pan->panStreamId() && m_radioModel.panStream()) {
                m_radioModel.panStream()->cancelPendingDbmRange(pan->panStreamId());
            }
        }
        if (*encoderRecoveryPending) {
            *encoderRecoveryPending = false;
            sw->reacquireNoiseFloorLock();
        }
    };
    auto finishDbmRangeHandshake = [pendingDbm, applyAuthoritativeDbmRange,
                                    retireDbmRangeWithoutEcho](quint64 generation) {
        const DbmRangeTransition::HandshakeDecision decision =
            pendingDbm->finish(generation);
        if (decision.action == DbmRangeTransition::HandshakeAction::ReconcileRadioRange) {
            applyAuthoritativeDbmRange(decision.range);
        } else if (decision.action
                   == DbmRangeTransition::HandshakeAction::RetireWithoutEcho) {
            retireDbmRangeWithoutEcho();
        }
    };
    auto armDbmRangeHandshake = [sw, pendingDbm, finishDbmRangeHandshake]
                                (float minDbm, float maxDbm) {
        const quint64 generation = pendingDbm->arm(
            minDbm, maxDbm, QDateTime::currentMSecsSinceEpoch());
        QTimer::singleShot(kDbmRangeHandshakeTimeoutMs, sw,
            [pendingDbm, finishDbmRangeHandshake, generation]() {
                finishDbmRangeHandshake(generation);
            });
    };
    auto sendDbmRangeCommand = [this, applet](float minDbm, float maxDbm) {
        // BACKSTOP. The callers above gate this individually so each one can do
        // the right local thing, but this is the one place every dBm range
        // leaves for the radio — and a range sent to a backend with no command
        // plane is silently dropped, which is precisely what starts the ratchet.
        // A future fourth caller gets the protection without knowing to ask.
        if (!m_radioModel.backendCapabilities().radioOwnsDbmScale) {
            return;
        }
        if (!dbmRangeLooksPlausible(minDbm, maxDbm)) {
            qCWarning(lcProtocol).noquote()
                << "MainWindow: rejecting implausible dBm range"
                << QStringLiteral("pan=%1").arg(applet->panId())
                << QStringLiteral("min=%1").arg(minDbm, 0, 'f', 2)
                << QStringLiteral("max=%1").arg(maxDbm, 0, 'f', 2);
            return;
        }

        // #3977 ownership note: foreign-owned pans are refused at the handler
        // entry (before the local decoder commit) and again centrally in
        // RadioModel::sendCommand — no per-command gate needed here.
        m_radioModel.sendCommand(
            QString("display pan set %1 min_dbm=%2 max_dbm=%3")
                .arg(applet->panId())
                .arg(static_cast<double>(minDbm), 0, 'f', 2)
                .arg(static_cast<double>(maxDbm), 0, 'f', 2));
    };

    // Guard: wirePanadapter() is called once at startup (for m_panApplet) and
    // again from panadapterAdded for the same widget.  Without these disconnects
    // every sw/menu/applet → this signal would be connected twice, causing each
    // user gesture to fire two identical radio commands (e.g. duplicate
    // "display pan set … min_dbm= max_dbm=").  Clearing prior connections here
    // replaces the scattered per-signal disconnect guards below.
    sw->disconnect(this);
    menu->disconnect(this);
    applet->disconnect(this);
    QObject::disconnect(this, &MainWindow::bandStackRestoreStarting, sw, nullptr);
    connect(this, &MainWindow::bandStackRestoreStarting,
            sw, [applet, pendingDbm, reconcileDbmRangeFromModel]
                (const QString& panId) {
        if (applet->panId() == panId && pendingDbm->active()) {
            reconcileDbmRangeFromModel();
        }
    });
    if (m_appletPanel && m_appletPanel->rxApplet()) {
        QObject::disconnect(m_appletPanel->rxApplet(), nullptr, sw, nullptr);
    }
    sw->setFpsMeterSyncStatsProvider([this]() {
        return receivePresentationOverlayStatsText();
    });

    auto updateKiwiWaterfallView = [this, applet, sw](double centerMhz,
                                                      double bandwidthMhz) {
        if (m_kiwiSdrManager && applet && sw) {
            const QString displayProfileId =
                kiwiSdrProfileForPan(applet->panId());
            for (SliceModel* slice : m_radioModel.slices()) {
                if (!slice || slice->panId() != applet->panId()
                    || !m_radioModel.sliceMayBelongToUs(slice->sliceId())) {
                    continue;
                }
                const QString profileId =
                    m_kiwiSdrManager->assignedProfileForSlice(slice->sliceId());
                if (profileId.isEmpty()) {
                    continue;
                }
                m_kiwiSdrManager->updateWaterfallView(
                    slice->sliceId(), applet->panId(), centerMhz,
                    bandwidthMhz, sw->wfLineDuration());
                const bool profileCanDriveWaterfall =
                    KiwiSdrClient::stateAllowsReceiverControl(
                        m_kiwiSdrManager->state(profileId))
                    && m_kiwiSdrManager->waterfallAvailable(profileId);
                if (profileId == displayProfileId
                    && kiwiSdrPanDisplaysKiwi(applet->panId())
                    && profileCanDriveWaterfall) {
                    sw->setKiwiSdrWaterfallAvailable(
                        m_kiwiSdrManager->waterfallAvailable(profileId));
                    sw->setKiwiSdrWaterfallProfile(profileId);
                    sw->setKiwiSdrWaterfallActive(true);
                    syncKiwiSdrAppletWaterfallState();
                }
            }
        }
    };

    auto kiwiGestureLastViewUpdate = std::make_shared<QElapsedTimer>();
    const auto updateKiwiWaterfallViewForGesture =
        [updateKiwiWaterfallView,
         kiwiGestureLastViewUpdate](double centerMhz, double bandwidthMhz) {
            const bool updateDue =
                !kiwiGestureLastViewUpdate->isValid()
                || kiwiGestureLastViewUpdate->elapsed()
                       >= kKiwiGestureViewUpdateMs;
            if (!updateDue) {
                return;
            }
            updateKiwiWaterfallView(centerMhz, bandwidthMhz);
            kiwiGestureLastViewUpdate->restart();
        };

    const auto updateKiwiWaterfallViewUnlessDeferred =
        [updateKiwiWaterfallView, updateKiwiWaterfallViewForGesture, sw,
         kiwiGestureLastViewUpdate](double centerMhz, double bandwidthMhz) {
            if (sw && sw->waterfallViewUpdateDeferred()) {
                return;
            }
            if (sw && sw->frequencyRangeGestureActive()) {
                updateKiwiWaterfallViewForGesture(centerMhz, bandwidthMhz);
                return;
            }
            updateKiwiWaterfallView(centerMhz, bandwidthMhz);
            kiwiGestureLastViewUpdate->invalidate();
        };

    connect(sw, &SpectrumWidget::frequencyRangeChanged,
            this, updateKiwiWaterfallViewUnlessDeferred);
    connect(sw, &SpectrumWidget::frequencyRangeChangeRequested,
            this, updateKiwiWaterfallViewUnlessDeferred);
    connect(sw, &SpectrumWidget::centerChangeRequested,
            this, [updateKiwiWaterfallView, updateKiwiWaterfallViewForGesture,
                   sw, kiwiGestureLastViewUpdate](double centerMhz) {
                if (!sw) {
                    return;
                }
                if (sw->frequencyRangeGestureActive()) {
                    return;
                }
                if (sw->panDragActive()) {
                    updateKiwiWaterfallViewForGesture(centerMhz,
                                                      sw->bandwidthMhz());
                    return;
                }
                updateKiwiWaterfallView(centerMhz, sw->bandwidthMhz());
                kiwiGestureLastViewUpdate->invalidate();
            });
    connect(sw, &SpectrumWidget::panDragSettled,
            this, [this, updateKiwiWaterfallView,
                   kiwiGestureLastViewUpdate](double centerMhz,
                                              double bandwidthMhz) {
                updateKiwiWaterfallView(centerMhz, bandwidthMhz);
                kiwiGestureLastViewUpdate->invalidate();
                retryAllDeferredKiwiLeaveReconcile();
            });
    connect(sw, &SpectrumWidget::frequencyRangeSettled,
            this, [this, updateKiwiWaterfallView,
                   kiwiGestureLastViewUpdate](double centerMhz,
                                              double bandwidthMhz) {
                updateKiwiWaterfallView(centerMhz, bandwidthMhz);
                kiwiGestureLastViewUpdate->invalidate();
                retryAllDeferredKiwiLeaveReconcile();
            });
    connect(sw, &SpectrumWidget::kiwiSdrDisplaySourceRequested,
            this, [this, applet](bool kiwi) {
        setKiwiSdrPanDisplaySource(applet->panId(), kiwi);
    });

    // Wire band plan manager to this spectrum widget
    sw->setBandPlanManager(m_bandPlanMgr);

    // Panadapter zoom limits. A backend that knows its real span range reports
    // it (per-pan, addressed); the FlexLib model table is the fallback for one
    // that doesn't — which is every Flex radio, so that path is unchanged.
    //
    // The fallback is not harmless when it's wrong: it falls through to 5.4 MHz
    // for any model string it doesn't recognise, so an HL2 delivering 384 kHz
    // could be zoomed fourteen times past its own data and the spectrum that was
    // never sampled rendered as black bars.
    applyPanBandwidthLimitsToWidget(
        applet->panId(), sw,
        m_radioModel.panMinBandwidthMhz(applet->panId()),
        m_radioModel.panMaxBandwidthMhz(applet->panId()));
    QObject::disconnect(&m_radioModel, &RadioModel::panBandwidthLimitsChanged,
                        sw, nullptr);
    connect(&m_radioModel, &RadioModel::panBandwidthLimitsChanged,
            sw, [this, applet, sw](const QString& panId, double minMhz, double maxMhz) {
        // Applets exist before any backend connects, so a connect-time report has
        // to re-clamp the widgets already on screen.
        if (panId != applet->panId()) {
            return;
        }
        // Don't undo the KiwiSDR widening. When this pan is displaying a Kiwi, the
        // data on screen is the Kiwi's, so syncKiwiSdrPanadapterUiState() widens
        // the ceiling to whichever of the two reaches further. Applying the local
        // backend's report raw would cap a displayed Kiwi at the HL2's 384 kHz on
        // connect — the local radio's limit clamping a zoom the Kiwi can fill.
        // (#4470)
        applyPanBandwidthLimitsToWidget(panId, sw, minMhz, maxMhz);
    });

    // Set panId on the overlay menu so +RX routes to the correct pan
    menu->setPanId(applet->panId());
    menu->setMemories(m_radioModel.memories());
    menu->setRadioModel(&m_radioModel);
    menu->setKiwiSdrManager(m_kiwiSdrManager);
    menu->setRadioCapabilities(m_radioModel.capabilities());
    menu->setDeclaredBands(m_radioModel.declaredBands());
    applyTuningRangeToOverlayMenu(menu);
    applyNotchCapabilities(sw);
    applyRadioSideDspToPanDisplay(sw);

    // Antenna list → this overlay menu (per-pan, mirrors VfoWidget pattern) (#1260)
    connect(&m_radioModel, &RadioModel::antListChanged,
            menu, &SpectrumOverlayMenu::setAntennaList,
            Qt::UniqueConnection);
    menu->setAntennaList(m_radioModel.antennaList());

    // Apply smart spot filter state to this (possibly new) panadapter
    sw->setSmartSpotFilter(m_smartSpotFilterEnabled, m_smartSpotFilterEnabledMs);
    sw->setSmartSpotFilterOpacity(
        AppSettings::instance().value("SmartSpotFilterOpacity", 80).toInt());
    sw->setSmartSpotFilterDelayS(
        AppSettings::instance().value("SmartSpotFilterDelayS", 30).toInt());
    sw->setSmartSpotFilterMatchHz(
        AppSettings::instance().value("SmartSpotFilterMatchHz", 1000).toInt());

    // Apply current prop forecast state to this (possibly new) panadapter
    if (m_propForecast) {
        sw->setPropForecastVisible(m_propForecast->isEnabled());
        if (m_propForecast->isEnabled()) {
            PropForecast fc = m_propForecast->lastForecast();
            sw->setPropForecast(fc.kIndex, fc.aIndex, fc.sfi);
        }
    }

    // Click on K/A/SFI overlay → open prop dashboard
    connect(sw, &SpectrumWidget::propForecastClicked,
            this, &MainWindow::showPropDashboard);

    if (auto* pan = m_radioModel.panadapter(applet->panId())) {
        connect(pan, &PanadapterModel::wideChanged,
                sw, &SpectrumWidget::setWideActive,
                Qt::UniqueConnection);
        sw->setWideActive(pan->wideActive());
        connect(pan, &PanadapterModel::wnbStateChanged,
                sw, &SpectrumWidget::syncWnbState,
                Qt::UniqueConnection);
        connect(pan, &PanadapterModel::wnbStateChanged,
                menu, &SpectrumOverlayMenu::syncWnbState,
                Qt::UniqueConnection);
        sw->syncWnbState(pan->wnbActive(), pan->wnbLevel(), pan->wnbUpdating());
        menu->syncWnbState(pan->wnbActive(), pan->wnbLevel(), pan->wnbUpdating());

        // Route confirmed level changes (min_dbm / max_dbm) from the radio to
        // this pan's spectrum widget.  Using the per-pan PanadapterModel signal
        // (guarded by change detection) rather than the RadioModel-level
        // panadapterLevelChanged signal ensures that:
        //   a) stale echo-backs (same value) don't overwrite the user's in-flight
        //      local change while waiting for the radio to confirm the command, and
        //   b) in multi-pan setups, a level update on pan B doesn't incorrectly
        //      update pan A's dBm scale.
        QObject::disconnect(pan, &PanadapterModel::levelChanged, sw, nullptr);
        connect(pan, &PanadapterModel::levelChanged,
                sw, [sw, pendingDbm, setStreamDbmRange,
                     applyAuthoritativeDbmRange,
                     encoderRecoveryPending](float minDbm, float maxDbm) {
            const DbmRangeTransition::HandshakeDecision decision =
                pendingDbm->observeRadioRange(
                    minDbm, maxDbm, QDateTime::currentMSecsSinceEpoch(),
                    kDbmRangeHandshakeTimeoutMs);
            if (decision.action
                == DbmRangeTransition::HandshakeAction::HoldRequestedRange) {
                setStreamDbmRange(
                    decision.range.minDbm, decision.range.maxDbm, true);
                return;
            }
            if (decision.action
                == DbmRangeTransition::HandshakeAction::ReconcileRadioRange) {
                applyAuthoritativeDbmRange(decision.range);
                return;
            }
            if (*encoderRecoveryPending) {
                setStreamDbmRange(minDbm, maxDbm);
                *encoderRecoveryPending = false;
                sw->reacquireNoiseFloorLock();
                return;
            }
            if (sw->isDraggingDbmScale()) {
                return;
            }
            sw->setDbmRange(minDbm, maxDbm);
        });

        // Prime the spectrum widget with the pan's current dBm range immediately
        // so the noise-floor auto-adjust starts from the correct position.
        // Without this, the widget defaults to refLevel=-50 / dynamicRange=100
        // while the radio model already knows the saved range (e.g. -130 to -40).
        // On reconnect the auto-adjust would animate from the wrong baseline and
        // fire dbmRangeChangeRequested with bogus values — which then locks out the
        // correct radio-reported range via the pendingDbm guard. (#3034)
        sw->setDbmRange(pan->minDbm(), pan->maxDbm());

        // Also set here, not only from applyCapabilitiesToUi: a pane added
        // AFTER connect (Add Panadapter, a layout change) never sees that
        // signal, and would arm its auto-floor against a radio that cannot
        // echo a range — one runaway pane is enough to churn the session.
        sw->setRadioOwnsDbmScale(!m_radioModel.isConnected()
                                 || m_radioModel.backendCapabilities().radioOwnsDbmScale);

        wirePanDisplayStatus(applet, pan);
    }
    syncTxWaterfallSliceToSpectrums();

    // ── Debounced resize → re-push xpixels/ypixels to the radio (#1511) ───
    // When the layout changes (e.g. adding a second pan, splitting, resizing),
    // the FFT pane dimensions change but the radio keeps encoding FFT bins to the
    // old yPixels scale.  This causes a ~5 dB noise-floor offset until restart.
    // A 300ms debounce avoids flooding the radio during animated resizes.
    {
        const OwnedSingleShotTimer resizeTimer = ensureOwnedSingleShotTimer(
            sw, QStringLiteral("panDimensionDebounceTimer"), 300);
        if (resizeTimer.newlyCreated) {
            connect(sw, &SpectrumWidget::dimensionsChanged,
                    resizeTimer.timer,
                    [timer = resizeTimer.timer](int, int) { timer->start(); });
            connect(resizeTimer.timer, &QTimer::timeout,
                    this, [this, applet, sw]() {
                auto* pan = m_radioModel.panadapter(applet->panId());
                if (!pan || !sw) {
                    return;
                }
                if (!panPixelDimensionsReady(sw)) {
                    return;
                }
                requestPanDimensionsForRadio(pan->panId(), sw);
            });
        }
    }

    // ── Tuning step size → this pan's spectrum widget ─────────────────────
    // The global connection only covers AppSettings + radio command.
    // Each pan must also receive stepSizeChanged so scroll-to-tune
    // uses the correct step regardless of which pan is active.
    connect(m_appletPanel->rxApplet(), &RxApplet::stepSizeChanged,
            sw, &SpectrumWidget::setStepSize);

    // ── Pan activation: clicking on this pan makes it active ─────────────
    connect(applet, &PanadapterApplet::activated,
            m_panStack, &PanadapterStack::setActivePan,
            Qt::UniqueConnection);

    // ── Close pan: X button on title bar closes this pan ────────────────
    connect(applet, &PanadapterApplet::closeRequested,
            this, [this](const QString& panId) {
        // Don't close the last pan
        if (m_panStack->count() <= 1) return;
        clearCenterLockForPan(panId, true);
        // Single source of truth for teardown: removePanadapter sends the
        // FlexLib-correct "display pan remove" + "display panafall remove"
        // pair, so a panafall-created pan also frees its waterfall. (#3843)
        m_radioModel.removePanadapter(panId);
    });

    // ── User drag actions from spectrum → radio (per-pan) ──────────────────
    connect(sw, &SpectrumWidget::frequencyRangeChangeRequested,
            this, [this, applet](double center, double bandwidth) {
        applyPanRangeRequest(applet->panId(), center, bandwidth, "explicit-pan-range");
    });
    connect(sw, &SpectrumWidget::bandwidthChangeRequested,
            this, [this, applet](double bw) {
        if (kiwiSdrPanDisplaysKiwi(applet->panId())) {
            return;
        }
        // #4142: a bandwidth drag is user intent — during the profile-load
        // hold a bare sendCommand() is silently destroyed; defer it instead.
        m_radioModel.requestPanBandwidth(applet->panId(), bw);
    });
    connect(sw, &SpectrumWidget::centerChangeRequested,
            this, [this, applet](double center) {
        if (kiwiSdrPanDisplaysKiwi(applet->panId())) {
            return;
        }
        // Clamp against EFFECTIVE bandwidth (a deferred zoom's pending value,
        // else the model): during the hold the model deliberately lags the
        // user's deferred request. Dispatch re-clamps against live geometry
        // regardless — this caller clamp is UX-only.
        const double effBw =
            m_radioModel.effectivePanBandwidthMhz(applet->panId());
        if (effBw > 0.0) {
            center = std::max(center, effBw / 2.0);
        }
        // The radio may acknowledge this command without echoing a display status
        // to the setting client, so requestPanCenter() keeps the canonical model
        // aligned with the wire command (TCI dds: follows this center) — and
        // during a profile load it defers rather than letting sendCmd drop it
        // silently (#4142).
        //
        // DRAG: this is the operator moving the window itself, which on a
        // backend whose scope is slaved to the VFO is a retune and nothing
        // else. Every other centre-only caller in this file is a follow,
        // reveal or recentre and takes the default (Range) — see
        // RadioModel::requestPanCenter().
        m_radioModel.requestPanCenter(applet->panId(), center, -1.0,
                                      IRadioBackend::PanCenterIntent::Drag);
    });
    // Band/Segment Zoom toggle off the pan's radio-authoritative model state
    // (togglePanZoomModeForPan) — shared with the keyboard/MIDI shortcuts
    // (MainWindow_Shortcuts.cpp) and the RC28/FlexControl paths
    // (MainWindow_Controllers.cpp), and per-pan by construction. This right-click
    // menu targets THIS applet's pan, not the active slice's. (#4057)
    connect(sw, &SpectrumWidget::bandZoomRequested,
            this, [this, applet]() {
        togglePanZoomModeForPan(applet->panId(), /*segmentZoom=*/false);
    });
    connect(sw, &SpectrumWidget::segmentZoomRequested,
            this, [this, applet]() {
        togglePanZoomModeForPan(applet->panId(), /*segmentZoom=*/true);
    });
    connect(sw, &SpectrumWidget::filterChangeRequested,
            this, [this](int lo, int hi) {
        if (auto* s = activeSlice()) s->setFilterWidth(lo, hi);
    });

    connect(sw, &SpectrumWidget::dbmRangeChangeRequested,
            this, [this, applet, sw, armDbmRangeHandshake,
                   setStreamDbmRange, sendDbmRangeCommand]
                  (float minDbm, float maxDbm) {
        if (sw->kiwiSdrWaterfallActive()) {
            sw->setDbmRange(minDbm, maxDbm);
            sw->prepareForFftScaleChange();
            sw->reacquireNoiseFloorLock();
            return;
        }

        const bool profileLoadHeld = profileLoadRadioStateWritesHeld();
        const bool autoFloorChange = sw->pendingAutoNoiseFloorDbmRange();

        // A backend whose dBm scale is FIXED never echoes a range back, and the
        // auto-floor loop is built on that echo: it moves the reference level,
        // requests the range, and waits for confirmation before moving again.
        // With nothing to confirm it reads the unchanged floor as "not there
        // yet" and steps again — a measured 24 dB/s ratchet that walks off the
        // bottom of the scale (-202 … -1882 dBm) and only becomes visible when
        // dbmRangeLooksPlausible() starts rejecting it at -180. Re-seed the
        // widget from the pan's real range instead, which is the same recovery
        // the profile-load path above uses, and drop the request.
        //
        // Deliberately NOT setNoiseFloorEnable(false): that is the operator's
        // own toggle, and forcing it would both fight the overlay menu and lose
        // the setting for the next radio. The auto-floor stays enabled and
        // simply has nothing to chase on a fixed scale.
        if (autoFloorChange && !m_radioModel.backendCapabilities().radioOwnsDbmScale) {
            if (auto* pan = m_radioModel.panadapter(applet->panId())) {
                sw->setDbmRange(pan->minDbm(), pan->maxDbm());
            }
            return;
        }
        if (profileLoadPanDisplaySettling(applet->panId()) && autoFloorChange) {
            if (auto* pan = m_radioModel.panadapter(applet->panId())) {
                if (pan->panStreamId() && m_radioModel.panStream()) {
                    setStreamDbmRange(pan->minDbm(), pan->maxDbm());
                }
                sw->setDbmRange(pan->minDbm(), pan->maxDbm());
            }
            sw->reacquireNoiseFloorLock();
            return;
        }

        const bool localOnly = profileLoadHeld || autoFloorChange;
        if (localOnly) {
            // Local-only dBm changes must not update PanadapterStream. The
            // stream decodes radio FFT pixel values using the radio's actual
            // min_dbm/max_dbm; changing that decoder without sending the same
            // range to the radio creates a feedback loop where the estimated
            // noise floor and display scale chase each other indefinitely.
            sw->setDbmRange(minDbm, maxDbm);
            return;
        }

        // #3977: refuse the WHOLE operation — local decoder commit included —
        // when this session no longer owns the pan. Gating only the outbound
        // command (RadioModel::sendCommand backstop) would leave the local
        // FFT decoder committed to a range the radio never adopted, with no
        // levelChanged echo to ever resync it.
        if (auto* pan = m_radioModel.panadapter(applet->panId());
            pan && !pan->ownedByClient(m_radioModel.ourClientHandle())) {
            return;
        }

        armDbmRangeHandshake(minDbm, maxDbm);
        setStreamDbmRange(minDbm, maxDbm, true);
        sendDbmRangeCommand(minDbm, maxDbm);
    });
    connect(sw, &SpectrumWidget::radioDbmHeadroomRecoveryRequested,
            this, [this, applet, sw, encoderDbmRange,
                   encoderRecoveryPending, armDbmRangeHandshake,
                   setStreamDbmRange, sendDbmRangeCommand](float headroomDb) {
        if (sw->kiwiSdrWaterfallActive()
            || profileLoadRadioStateWritesHeld()) {
            return;
        }
        // Headroom recovery asks the RADIO to move its scale — the clue is in
        // the name of the signal that gets us here. A backend whose scale is
        // fixed has nothing to ask and nothing that will answer, so the request
        // is dropped, the handshake never completes, and the auto-floor retries
        // it forever: this is the second source of the 24 dB/s dBm ratchet, and
        // it survives gating the dbmRangeChangeRequested path alone (measured
        // on an IC-9700 — the sendCommand lines stop, the rejections do not).
        if (!m_radioModel.backendCapabilities().radioOwnsDbmScale) {
            return;
        }
        if (auto* pan = m_radioModel.panadapter(applet->panId());
            pan && !pan->ownedByClient(m_radioModel.ourClientHandle())) {
            return;
        }

        const DbmRangeTransition::Range recoveryRange =
            DbmRangeTransition::clippedFloorRecoveryRange(
                encoderDbmRange->minDbm,
                encoderDbmRange->maxDbm,
                headroomDb);
        *encoderRecoveryPending = true;
        sw->cancelPendingDbmRangeChange();
        armDbmRangeHandshake(
            recoveryRange.minDbm, recoveryRange.maxDbm);
        setStreamDbmRange(
            recoveryRange.minDbm, recoveryRange.maxDbm, true);
        sendDbmRangeCommand(
            recoveryRange.minDbm, recoveryRange.maxDbm);
    });
    connect(sw, &SpectrumWidget::dbmRangeDragFinished,
            this, [this, applet, sw, armDbmRangeHandshake,
                   setStreamDbmRange, sendDbmRangeCommand](float minDbm, float maxDbm) {
        if (sw->kiwiSdrWaterfallActive()) {
            sw->setDbmRange(minDbm, maxDbm);
            sw->prepareForFftScaleChange();
            sw->reacquireNoiseFloorLock();
            return;
        }

        // #3977: same whole-operation refusal as dbmRangeChangeRequested.
        if (auto* pan = m_radioModel.panadapter(applet->panId());
            pan && !pan->ownedByClient(m_radioModel.ourClientHandle())) {
            return;
        }

        // A hand drag is a deliberate operator action, so it still moves the
        // LOCAL scale on a fixed-scale backend — but there is no radio-side
        // range to command and no echo to wait for, so skip the handshake and
        // the command. Arming the handshake here would leave m_pendingDbmRange
        // set until it times out, which is what blocks the next legitimate
        // range update (SpectrumWidget::setDbmRange's early return).
        if (!m_radioModel.backendCapabilities().radioOwnsDbmScale) {
            sw->setDbmRange(minDbm, maxDbm);
            setStreamDbmRange(minDbm, maxDbm, true);
            return;
        }

        armDbmRangeHandshake(minDbm, maxDbm);
        setStreamDbmRange(minDbm, maxDbm, true);
        sendDbmRangeCommand(minDbm, maxDbm);
    });

    // ── TNF signals ──────────────────────────────────────────────────────
    // QPointer guards against dangling sw when a pan is removed (#381)
    QPointer<SpectrumWidget> swGuard(sw);
    auto* tnf = &m_radioModel.tnfModel();
    auto rebuildTnfMarkers = [this, swGuard]() {
        if (!swGuard) return;
        auto* t = &m_radioModel.tnfModel();
        QVector<SpectrumWidget::TnfMarker> markers;
        for (const auto& e : t->tnfs())
            markers.append({e.id, e.freqMhz, e.widthHz, e.depthDb, e.permanent});
        swGuard->setTnfMarkers(markers);
    };
    QObject::disconnect(tnf, &TnfModel::tnfChanged, sw, nullptr);
    QObject::disconnect(tnf, &TnfModel::tnfRemoved, sw, nullptr);
    connect(tnf, &TnfModel::tnfChanged,  sw, rebuildTnfMarkers);
    connect(tnf, &TnfModel::tnfRemoved,  sw, rebuildTnfMarkers);
    connect(tnf, &TnfModel::globalEnabledChanged,
            sw, &SpectrumWidget::setTnfGlobalEnabled,
            Qt::UniqueConnection);
    QObject::disconnect(m_tnfIndicatorConnection);
    m_tnfIndicatorConnection = connect(
        tnf, &TnfModel::globalEnabledChanged,
        this, [this](bool on) {
            m_tnfIndicator->setStyleSheet(on
                ? "QLabel { color: #00b4d8; font-weight: bold; font-size: 24px; }"
                : "QLabel { color: #404858; font-weight: bold; font-size: 24px; }");
        });

    // FDX indicator style update
    QObject::disconnect(m_fdxIndicatorConnection);
    m_fdxIndicatorConnection = connect(
        &m_radioModel, &RadioModel::infoChanged, this, [this]() {
            const bool fdx = m_radioModel.fullDuplexEnabled();
            m_fdxIndicator->setStyleSheet(fdx
                ? "QLabel { color: #00b4d8; font-weight: bold; font-size: 24px; }"
                : "QLabel { color: #404858; font-weight: bold; font-size: 24px; }");
        });
    connect(sw, &SpectrumWidget::tnfCreateRequested,   tnf, &TnfModel::createTnf,
            Qt::UniqueConnection);
    connect(sw, &SpectrumWidget::tnfMoveRequested,     tnf, &TnfModel::setTnfFreq,
            Qt::UniqueConnection);
    connect(sw, &SpectrumWidget::tnfRemoveRequested,   tnf, &TnfModel::requestRemoveTnf,
            Qt::UniqueConnection);
    connect(sw, &SpectrumWidget::tnfWidthRequested,    tnf, &TnfModel::setTnfWidth,
            Qt::UniqueConnection);
    connect(sw, &SpectrumWidget::tnfDepthRequested,    tnf, &TnfModel::setTnfDepth,
            Qt::UniqueConnection);
    connect(sw, &SpectrumWidget::tnfPermanentRequested, tnf, &TnfModel::setTnfPermanent,
            Qt::UniqueConnection);

    // ── Spot markers ─────────────────────────────────────────────────────
    auto* spots = &m_radioModel.spotModel();
    auto rebuildSpots = [this, swGuard]() {
        if (!swGuard) return;  // widget destroyed (layout change)
        auto* s = &m_radioModel.spotModel();
        const bool showMemories =
            AppSettings::instance().value("IsMemorySpotsEnabled", "False").toString() == "True";
        QVector<SpectrumWidget::SpotMarker> markers;
        for (const auto& spot : s->spots()) {
            if (spot.source == "Memory" && !showMemories)
                continue;
            qint64 tsMs = 0;
            if (spot.source != "Memory") {
                tsMs = (spot.timestamp.isValid() && spot.timestamp.toMSecsSinceEpoch() > 0)
                    ? spot.timestamp.toMSecsSinceEpoch()
                    : spot.addedMs;
            }
            QColor dxccCol;
            if (m_dxccProvider.isEnabled() && spot.source != "Memory")
                dxccCol = m_dxccProvider.colorForSpot(spot.callsign, spot.rxFreqMhz, spot.mode);
            markers.append({spot.index, spot.callsign, spot.rxFreqMhz, spot.color, spot.mode,
                            dxccCol, spot.source, spot.spotterCallsign, spot.comment, tsMs,
                            spot.backgroundColor});
        }
        swGuard->setSpotMarkers(markers);
    };
    // Coalesce spot-model change bursts into a single rebuild per repaint
    // cadence (#2481). Each rebuildSpots() iterates every spot, runs a DXCC
    // colour lookup per spot, and diffs the whole marker vector. Firing it
    // once per incoming spot — and once per panadapter — turns two
    // simultaneous high-rate cluster feeds (e.g. two LogHX3 Spot Machines via
    // SpotCollector) into an O(spots x spots/sec x pans) main-thread storm
    // that stutters the waterfall. A short single-shot timer collapses a burst
    // of N spot signals into one rebuild. Parented to sw so it dies with the
    // pan; QPointer in rebuildSpots already guards against a dangling widget.
    const OwnedSingleShotTimer spotRebuildTimer = ensureOwnedSingleShotTimer(
        sw, QStringLiteral("spotRebuildTimer"), 50);  // ~20 Hz, below FFT repaint cadence
    if (spotRebuildTimer.newlyCreated) {
        connect(spotRebuildTimer.timer, &QTimer::timeout, sw, rebuildSpots);
        QPointer<QTimer> rebuildTimerGuard(spotRebuildTimer.timer);
        auto scheduleRebuildSpots = [rebuildTimerGuard]() {
            if (rebuildTimerGuard) {
                startSingleShotTimerIfIdle(rebuildTimerGuard.data());
            }
        };
        // Use the per-widget timer as the connection context so removing the
        // pan also removes these model subscriptions.  MainWindow outlives
        // every pan, so using `this` would retain one inert QPointer callback
        // for every pan that had ever existed.
        connect(spots, &SpotModel::spotAdded,
                spotRebuildTimer.timer, scheduleRebuildSpots);
        connect(spots, &SpotModel::spotUpdated,
                spotRebuildTimer.timer, scheduleRebuildSpots);
        connect(spots, &SpotModel::spotRemoved,
                spotRebuildTimer.timer, scheduleRebuildSpots);
        connect(spots, &SpotModel::spotsCleared,
                spotRebuildTimer.timer, scheduleRebuildSpots);
        connect(spots, &SpotModel::spotsRefreshed,
                spotRebuildTimer.timer, scheduleRebuildSpots);
    }
    {
        auto& s = AppSettings::instance();
        sw->setShowSpots(s.value("IsSpotsEnabled", "True").toString() == "True");
        sw->setSpotFontSize(s.value("SpotFontSize", "16").toInt());
        sw->setSpotMaxLevels(s.value("SpotsMaxLevel", "3").toInt());
        sw->setSpotStartPct(s.value("SpotsStartingHeightPercentage", "50").toInt());
        sw->setSpotOverrideColors(s.value("IsSpotsOverrideColorsEnabled", "False").toString() == "True");
        sw->setSpotOverrideBg(s.value("IsSpotsOverrideBackgroundColorsEnabled", "True").toString() == "True");
        sw->setSpotColor(QColor(s.value("SpotsOverrideColor", "#FFFF00").toString()));
        sw->setSpotBgColor(QColor(s.value("SpotsOverrideBgColor", "#000000").toString()));
        sw->setSpotBgOpacity(s.value("SpotsBackgroundOpacity", 48).toInt());
        sw->setSpotShowLines(s.value("IsSpotsLinesEnabled", "True").toString() == "True");
    }

    // ── S History Markers ─────────────────────────────────────────────────
    sw->setShowSHistory(m_sHistoryEnabled);
    sw->setShowSHistoryQrm(m_sHistoryQrmEnabled);
    sw->setSHistorySnapToStep(
        AppSettings::instance().value("SHistorySnapToStep", "False").toString() == "True");
    if (m_sHistoryEnabled || m_sHistoryQrmEnabled) {
        // Seed a 10-second warmup so QRM classification can establish a
        // baseline before any markers appear on the newly-wired pan.
        auto& ps = m_sHistoryPanState[applet->panId()];
        if (ps.suppressUntilMs == 0) {
            ps.suppressUntilMs = QDateTime::currentMSecsSinceEpoch() + 10000;
        }
        if (m_sHistoryData.contains(applet->panId()))
            rebuildSHistoryForPan(applet->panId());
    }

    // ── Per-pan display controls (client-side) ───────────────────────────
    connect(menu, &SpectrumOverlayMenu::fftFillAlphaChanged,
            sw, &SpectrumWidget::setFftFillAlpha,
            Qt::UniqueConnection);
    connect(menu, &SpectrumOverlayMenu::fftFillColorChanged,
            sw, &SpectrumWidget::setFftFillColor,
            Qt::UniqueConnection);
    connect(menu, &SpectrumOverlayMenu::fftLineColorChanged,
            sw, &SpectrumWidget::setFftLineColor,
            Qt::UniqueConnection);
    connect(menu, &SpectrumOverlayMenu::fftHeatMapChanged,
            sw, &SpectrumWidget::setFftHeatMap,
            Qt::UniqueConnection);
    connect(menu, &SpectrumOverlayMenu::showGridChanged,
            sw, &SpectrumWidget::setShowGrid,
            Qt::UniqueConnection);
    connect(menu, &SpectrumOverlayMenu::freqGridSpacingChanged,
            sw, &SpectrumWidget::setFreqGridSpacing,
            Qt::UniqueConnection);
    connect(menu, &SpectrumOverlayMenu::freqScaleFontPtChanged,
            sw, &SpectrumWidget::setFreqScaleFontPt,
            Qt::UniqueConnection);
    connect(menu, &SpectrumOverlayMenu::fftLineWidthChanged,
            sw, &SpectrumWidget::setFftLineWidth,
            Qt::UniqueConnection);
    connect(menu, &SpectrumOverlayMenu::noiseFloorPositionChanged,
            sw, &SpectrumWidget::setNoiseFloorPosition,
            Qt::UniqueConnection);
    connect(menu, &SpectrumOverlayMenu::noiseFloorEnableChanged,
            sw, &SpectrumWidget::setNoiseFloorEnable,
            Qt::UniqueConnection);
    connect(sw, &SpectrumWidget::noiseFloorPositionResolved,
            menu, &SpectrumOverlayMenu::syncNoiseFloorPosition,
            Qt::UniqueConnection);

    // ── Auto-squelch wiring ───────────────────────────────────────────────
    // RxApplet signals → per-pan spectrum widget
    connect(m_appletPanel->rxApplet(), &RxApplet::sqlAutoChanged,
            sw, [this, applet, sw](bool) {
        const QString panId = applet ? applet->panId() : QString();
        sw->setAutoSquelchEnable(
            autoSquelchShouldRunOnSpectrum(panId, sw));
    });
    connect(m_appletPanel->rxApplet(), &RxApplet::squelchStateChanged,
            sw, [this, applet, sw](bool on, int level) {
        SliceModel* s = activeSlice();
        const bool kiwiActive = s && m_kiwiSdrManager
            && !m_kiwiSdrManager->assignedProfileForSlice(s->sliceId()).isEmpty();
        if (kiwiActive) {
            if (applet && applet->panId() == s->panId()) {
                sw->setKiwiSdrSquelchLine(
                    on, kiwiSdrSquelchMarginDbForSliceLevel(s, level),
                    s->externalReceiveAutoSquelchOn());
            } else {
                sw->clearKiwiSdrSquelchLine();
                sw->setSquelchLine(false, 0);
            }
            return;
        }
        const QString panId = applet ? applet->panId() : QString();
        if (!kiwiSdrProfileForPan(panId).isEmpty()
            || sw->kiwiSdrWaterfallActive()) {
            sw->clearKiwiSdrSquelchLine();
            sw->setSquelchLine(false, 0);
            return;
        }
        sw->clearKiwiSdrSquelchLine();
        sw->setSquelchLine(on, level);
    });
    // Spectrum widget → radio + back to overlay: apply level and immediately
    // update the squelch line on sw so the yellow line never depends on the
    // RxApplet → squelchStateChanged → setSquelchLine round-trip, which can
    // miss the first emission if wirePanadapter runs before setSlice.
    connect(sw, &SpectrumWidget::autoSquelchLevelSuggested,
            this, [this, applet, sw](int level) {
        const QString panId = applet ? applet->panId() : QString();
        if (!autoSquelchShouldRunOnSpectrum(panId, sw)) {
            return;
        }
        if (SliceModel* kiwiSlice =
                kiwiAssignedSliceForPan(m_radioModel, m_kiwiSdrManager, panId)) {
            const int margin = currentAutoSqlMarginDb();
            kiwiSlice->setSquelch(true, margin);
            sw->setKiwiSdrSquelchLine(true, margin, true);
            return;
        }
        SliceModel* s = activeSlice();
        if (!s) {
            return;
        }
        const bool kiwiActive = m_kiwiSdrManager
            && !m_kiwiSdrManager->assignedProfileForSlice(s->sliceId()).isEmpty();
        if (kiwiActive) {
            const int margin = currentAutoSqlMarginDb();
            s->setSquelch(true, margin);
            sw->setKiwiSdrSquelchLine(
                true, margin, true);
        } else {
            s->setSquelch(true, level);
            sw->setSquelchLine(true, level);
        }
    });
    // Auto-squelch margin: now driven by the RX Applet's SQL slider when
    // SQL mode is Auto (instead of a separate slider in the Display
    // overlay).  The slider repurposes itself in Auto mode to set the
    // dB margin above the measured noise floor; manual mode is unchanged.
    connect(m_appletPanel->rxApplet(), &RxApplet::autoSqlMarginDbChanged,
            sw, &SpectrumWidget::setAutoSqlMarginDb);
    // Initialize from AppSettings for this pan's spectrum widget.
    {
        auto& s = AppSettings::instance();
        int margin = std::clamp(s.value("AutoSqlMarginDb", "10").toInt(), 5, 20);
        sw->setAutoSqlMarginDb(margin);
    }
    {
        const QString panId = applet ? applet->panId() : QString();
        sw->setAutoSquelchEnable(
            autoSquelchShouldRunOnSpectrum(panId, sw));
    }

    // Pop out / dock panadapter
    connect(sw, &SpectrumWidget::popOutRequested, this, [this, applet](bool popOut) {
        if (popOut) {
            m_panStack->floatPanadapter(applet->panId());
        } else {
            m_panStack->dockPanadapter(applet->panId());
        }
    });
    connect(applet, &PanadapterApplet::popOutClicked, this, [this, applet]() {
        m_panStack->floatPanadapter(applet->panId());
    });

    // ── DAX IQ pan routing from overlay menu ───────────────────────────
    // The overlay controls which pan feeds which IQ channel (routing only).
    // Stream create/destroy and rate are managed by the DIGI applet.
    connect(menu, &SpectrumOverlayMenu::daxIqChannelChanged,
            this, [this, applet](int channel) {
        auto* pan = m_radioModel.panadapter(applet->panId());
        if (!pan) return;
        m_radioModel.sendCommand(
            QString("display pan set %1 daxiq_channel=%2")
                .arg(applet->panId()).arg(channel));
    });

    // WFM software-demod toggle from the DAX panel. Acts on the menu's slice;
    // WFM is mode-independent (raw IQ from this pan's DAX stream). Mirrors the
    // VFO-flag toggle's deactivate guard: only the WFM slice may deactivate,
    // and not during the activation cooldown. (#3853)
    connect(menu, &SpectrumOverlayMenu::wfmToggleRequested,
            this, [this](bool on, int sliceId) {
        if (on) activateWFM(sliceId);
        else if (sliceId == m_wfmSliceId && !m_wfmCooldown) deactivateWFM();
    });

    // Sync DAX IQ combo from radio status + restore saved assignment (#1221)
    {
        auto* pan = m_radioModel.panadapter(applet->panId());
        if (pan) {
            connect(pan, &PanadapterModel::daxiqChannelChanged,
                    menu, &SpectrumOverlayMenu::syncDaxIqChannel,
                    Qt::UniqueConnection);
            menu->syncDaxIqChannel(pan->daxiqChannel());

            // DAX IQ channel restore deferred — the radio persists
            // daxiq_channel on the pan, so it arrives via status echo.
            // Overlay combo syncs automatically via daxiqChannelChanged.
        }
    }

    // ── Per-pan display controls → radio commands ────────────────────────
    // Each pan's overlay sends commands with its own panId/wfId, not the
    // global active pan. This ensures display settings work independently.
    connect(menu, &SpectrumOverlayMenu::fftAverageChanged,
            this, [this, applet, sw](int v) {
        sw->setFftAverage(v);
        m_radioModel.sendCommand(
            QString("display pan set %1 average=%2").arg(applet->panId()).arg(v));
    });
    connect(menu, &SpectrumOverlayMenu::fftFpsChanged,
            this, [this, applet, sw](int v) {
        sw->setFftFps(v);  // always update restore target for when throttle lifts
        if (m_adaptiveThrottleActive)
            return;
        // Through the model, not a raw sendCommand: on a backend that streams
        // raw spectra this is the engine's own shaping target, and the Flex wire
        // text it used to send reached nothing there — the slider moved the
        // widget's render cap while frames kept arriving at the IQ sample rate.
        m_radioModel.requestPanDisplayRates(applet->panId(), v, /*wfMs=*/0);
    });
    connect(menu, &SpectrumOverlayMenu::fftWeightedAverageChanged,
            this, [this, applet, sw](bool on) {
        sw->setFftWeightedAvg(on);
        m_radioModel.sendCommand(
            QString("display pan set %1 weighted_average=%2").arg(applet->panId()).arg(on ? 1 : 0));
    });
    connect(menu, &SpectrumOverlayMenu::wfColorSchemeChanged,
            sw, &SpectrumWidget::setWfColorScheme,
            Qt::UniqueConnection);
    connect(menu, &SpectrumOverlayMenu::spectrumRenderModeChanged,
            sw, &SpectrumWidget::setSpectrumRenderMode,
            Qt::UniqueConnection);
    connect(menu, &SpectrumOverlayMenu::dssFloorDepthChanged,
            sw, &SpectrumWidget::setDssFloorDepth,
            Qt::UniqueConnection);
    connect(sw, &SpectrumWidget::dssFloorDepthResolved,
            menu, &SpectrumOverlayMenu::syncDssFloorDepth,
            Qt::UniqueConnection);
    connect(menu, &SpectrumOverlayMenu::dssGainChanged,
            sw, &SpectrumWidget::setDssGain,
            Qt::UniqueConnection);
    connect(menu, &SpectrumOverlayMenu::dssRowSpanChanged,
            sw, &SpectrumWidget::setDssRowSpan,
            Qt::UniqueConnection);
    connect(menu, &SpectrumOverlayMenu::wfColorGainChanged,
            this, [this, applet, sw](int v) {
        if (kiwiSdrPanDisplaysKiwi(applet->panId())) {
            return;
        }
        sw->setWfColorGain(v);
        auto* pan = m_radioModel.panadapter(applet->panId());
        if (pan && !pan->waterfallId().isEmpty())
            m_radioModel.sendCommand(
                QString("display panafall set %1 color_gain=%2").arg(pan->waterfallId()).arg(v));
    });
    connect(menu, &SpectrumOverlayMenu::wfBlackLevelChanged,
            this, [this, applet, sw](int v) {
        if (kiwiSdrPanDisplaysKiwi(applet->panId())) {
            return;
        }
        sw->setWfBlackLevel(v);
        auto* pan = m_radioModel.panadapter(applet->panId());
        if (pan && !pan->waterfallId().isEmpty())
            m_radioModel.sendCommand(
                QString("display panafall set %1 black_level=%2").arg(pan->waterfallId()).arg(v));
    });
    connect(menu, &SpectrumOverlayMenu::wfAutoBlackChanged,
            this, [this, applet, sw](bool on) {
        if (kiwiSdrPanDisplaysKiwi(applet->panId())) {
            return;
        }
        sw->setWfAutoBlack(on);
        // Keep the radio's auto_black flag in sync at runtime; it only reaches
        // 1 when auto-black is on AND the radio-side source is selected.
        m_radioModel.setWaterfallAutoBlack(on);
    });
    connect(menu, &SpectrumOverlayMenu::wfAutoBlackOffsetChanged,
            this, [this, applet, sw](int offset) {
        if (kiwiSdrPanDisplaysKiwi(applet->panId())) {
            return;
        }
        sw->setWfAutoBlackOffset(offset);
    });
    connect(menu, &SpectrumOverlayMenu::wfAutoBlackSourceChanged,
            this, [this, applet, sw](bool radioSide) {
        if (kiwiSdrPanDisplaysKiwi(applet->panId())) {
            return;
        }
        sw->setWfAutoBlackRadioSide(radioSide);
        // Radio-side → tell the radio to compute its per-tile auto-black level;
        // client-side → stop it (auto_black=0) and use our own estimate.
        m_radioModel.setWaterfallAutoBlackSource(radioSide);
    });
    // `rate` is the 1..100 control value, low slow / high fast — NOT the
    // milliseconds its Flex wire name (`line_duration`) suggests. See
    // core/WaterfallRate.h; reading it as ms is what inverted the slider on the
    // Hermes-Lite 2 (#4606).
    const auto applyWaterfallLineDuration = [this, applet, sw](int rate) {
        const int clampedRate = std::clamp(rate,
                                           AetherSDR::WaterfallRate::kMin,
                                           AetherSDR::WaterfallRate::kMax);
        const QString profileId = kiwiSdrProfileForPan(applet->panId());
        if (!profileId.isEmpty() && kiwiSdrPanDisplaysKiwi(applet->panId())) {
            const KiwiSdrAntennaProfile profile =
                m_kiwiSdrManager ? m_kiwiSdrManager->profile(profileId)
                                 : KiwiSdrAntennaProfile{};
            if (m_kiwiSdrManager && profile.waterfallRate == 0) {
                for (SliceModel* slice : m_radioModel.slices()) {
                    if (!slice || slice->panId() != applet->panId()
                        || m_kiwiSdrManager->assignedProfileForSlice(slice->sliceId())
                            != profileId) {
                        continue;
                    }
                    m_kiwiSdrManager->updateWaterfallView(
                        slice->sliceId(), applet->panId(), sw->centerMhz(),
                        sw->bandwidthMhz(), clampedRate);
                    break;
                }
            }
            return;
        }
        sw->setWfLineDuration(clampedRate);
        // Same reason as the FPS slider above: on a raw-spectrum backend this is
        // the engine's waterfall shaping target, not a radio setting.
        m_radioModel.requestPanDisplayRates(applet->panId(), /*fps=*/0, clampedRate);
    };
    connect(menu, &SpectrumOverlayMenu::wfLineDurationChanged,
            this, applyWaterfallLineDuration);
    connect(sw, &SpectrumWidget::waterfallLineDurationChangeRequested,
            this, applyWaterfallLineDuration);
    connect(menu, &SpectrumOverlayMenu::kiwiWaterfallMaxChanged,
            this, [this, applet, sw](int maxDbm) {
        if (!m_kiwiSdrManager) {
            return;
        }
        const QString profileId = kiwiSdrProfileForPan(applet->panId());
        if (profileId.isEmpty() || !kiwiSdrPanDisplaysKiwi(applet->panId())) {
            return;
        }
        const KiwiSdrAntennaProfile profile =
            m_kiwiSdrManager->profile(profileId);
        const KiwiSdrWaterfallDisplayRange displayRange =
            m_kiwiSdrManager->waterfallDisplayRange(profileId);
        const int currentMin = displayRange.valid
            ? static_cast<int>(std::lround(displayRange.minDbm))
            : profile.waterfallMinDbm;
        const int clampedMax = std::max(maxDbm, currentMin + 1);
        m_kiwiSdrManager->setProfileWaterfallDisplayRange(
            profileId, currentMin, clampedMax,
            false, profile.waterfallRate);
        sw->setKiwiSdrWaterfallDisplayRange(currentMin,
                                            clampedMax,
                                            false);
        syncKiwiSdrPanadapterUiState(applet->panId());
    });
    connect(menu, &SpectrumOverlayMenu::kiwiWaterfallMinChanged,
            this, [this, applet, sw](int minDbm) {
        if (!m_kiwiSdrManager) {
            return;
        }
        const QString profileId = kiwiSdrProfileForPan(applet->panId());
        if (profileId.isEmpty() || !kiwiSdrPanDisplaysKiwi(applet->panId())) {
            return;
        }
        const KiwiSdrAntennaProfile profile =
            m_kiwiSdrManager->profile(profileId);
        const KiwiSdrWaterfallDisplayRange displayRange =
            m_kiwiSdrManager->waterfallDisplayRange(profileId);
        const int currentMax = displayRange.valid
            ? static_cast<int>(std::lround(displayRange.maxDbm))
            : profile.waterfallMaxDbm;
        const int clampedMin = std::min(minDbm, currentMax - 1);
        m_kiwiSdrManager->setProfileWaterfallDisplayRange(
            profileId, clampedMin, currentMax,
            false, profile.waterfallRate);
        sw->setKiwiSdrWaterfallDisplayRange(clampedMin,
                                            currentMax,
                                            false);
        syncKiwiSdrPanadapterUiState(applet->panId());
    });
    connect(menu, &SpectrumOverlayMenu::kiwiWaterfallAutoRequested,
            this, [this, applet]() {
        if (!m_kiwiSdrManager) {
            return;
        }
        const QString profileId = kiwiSdrProfileForPan(applet->panId());
        if (profileId.isEmpty()
            || !kiwiSdrPanDisplaysKiwi(applet->panId())) {
            return;
        }
        const KiwiSdrAntennaProfile profile =
            m_kiwiSdrManager->profile(profileId);
        m_kiwiSdrManager->setProfileWaterfallDisplayRange(
            profileId, profile.waterfallMinDbm, profile.waterfallMaxDbm,
            true, profile.waterfallRate);
        m_kiwiSdrManager->requestProfileWaterfallAutoScale(profileId);
        syncKiwiSdrPanadapterUiState(applet->panId());
    });
    connect(menu, &SpectrumOverlayMenu::kiwiWaterfallRateChanged,
            this, [this, applet](int rate) {
        if (!m_kiwiSdrManager) {
            return;
        }
        const QString profileId = kiwiSdrProfileForPan(applet->panId());
        if (profileId.isEmpty() || !kiwiSdrPanDisplaysKiwi(applet->panId())) {
            return;
        }
        const KiwiSdrAntennaProfile profile =
            m_kiwiSdrManager->profile(profileId);
        m_kiwiSdrManager->setProfileWaterfallDisplayRange(
            profileId, profile.waterfallMinDbm, profile.waterfallMaxDbm,
            profile.waterfallAutoScale, rate);
        syncKiwiSdrPanadapterUiState(applet->panId());
    });
    // NB Waterfall Blanker (#277)
    connect(menu, &SpectrumOverlayMenu::wfBlankerEnabledChanged,
            sw, &SpectrumWidget::setWfBlankerEnabled,
            Qt::UniqueConnection);
    connect(menu, &SpectrumOverlayMenu::wfBlankerThresholdChanged,
            sw, &SpectrumWidget::setWfBlankerThreshold,
            Qt::UniqueConnection);
    connect(menu, &SpectrumOverlayMenu::backgroundImageRequested,
            this, [sw] {
        const QString path = getBackgroundImagePath(sw->window(), "Choose Background Image");
        if (path.isEmpty()) return;
        sw->setBackgroundImage(path);
        auto& s = AppSettings::instance();
        s.setValue(sw->settingsKey("BackgroundImage"), path);
        s.save();
    });
    connect(menu, &SpectrumOverlayMenu::backgroundImageCleared,
            this, [sw] {
        sw->setBackgroundImage(":/bg-default.jpg");
        auto& s = AppSettings::instance();
        s.setValue(sw->settingsKey("BackgroundImage"), ":/bg-default.jpg");
        s.save();
    });
    // Right-click "Clear": turn the background off entirely (no image, just the
    // fill colour) and persist as "none" so it stays off across restarts.
    connect(menu, &SpectrumOverlayMenu::backgroundImageDisabled,
            this, [sw] {
        sw->setBackgroundImage(QString());
        auto& s = AppSettings::instance();
        s.setValue(sw->settingsKey("BackgroundImage"), "none");
        s.save();
    });
    connect(menu, &SpectrumOverlayMenu::backgroundOpacityChanged,
            this, [sw](int pct) {
        sw->setBackgroundOpacity(pct);
        auto& s = AppSettings::instance();
        s.setValue(sw->settingsKey("BackgroundOpacity"), QString::number(pct));
        s.save();
    });
    connect(menu, &SpectrumOverlayMenu::backgroundFillColorChanged,
            this, [sw, menu](const QColor& c) {
        sw->setBackgroundFillColor(c);
        // Update the swatch button so it reflects the chosen colour without
        // waiting for the next syncExtraDisplaySettings round.
        menu->syncExtraDisplaySettings(sw->wfBlankerEnabled(),
            sw->wfBlankerThreshold(), sw->backgroundOpacity(),
            sw->freqGridSpacing(), c, sw->freqScaleFontPt());
    });
    connect(menu, &SpectrumOverlayMenu::displaySettingsReset,
            this, [this, applet, sw, menu] {
        // Apply all SpectrumWidget defaults
        sw->setFftAverage(0);
        sw->setFftFps(25);
        sw->setFftFillAlpha(0.70f);
        sw->setFftFillColor(QColor(0x00, 0xe5, 0xff));
        sw->setFftLineColor(QColor(0x00, 0xe5, 0xff));
        sw->setFftLineWidth(2.0f);
        sw->setFftWeightedAvg(false);
        sw->setFftHeatMap(true);
        sw->setWfColorScheme(0);
        sw->setWfColorGain(50);
        sw->setWfBlackLevel(15);
        sw->setWfAutoBlack(true);
        sw->setWfAutoBlackOffset(50);
        sw->setWfAutoBlackRadioSide(false);
        sw->setWfLineDuration(100);
        sw->setWfBlankerEnabled(false);
        sw->setWfBlankerThreshold(1.15f);
        sw->setWfBlankerMode(0);
        sw->setShowCursorFreq(false);
        sw->setBackgroundImage(":/bg-default.jpg");
        sw->setBackgroundOpacity(80);
        sw->setBackgroundFillColor(QColor(0x0a, 0x0a, 0x14));
        sw->setNoiseFloorEnable(false);
        sw->setNoiseFloorPosition(75);
        sw->setFreqGridSpacing(0);  // Auto (#1390)
        sw->setFreqScaleFontPt(8);  // default scale text size (#3501)

        // Radio commands for radio-authoritative display settings.
        // fps and line_duration are suppressed while adaptive throttle is active
        // so the reset doesn't fight the congestion-aware cap. The SpectrumWidget
        // values above (sw->setFftFps / sw->setWfLineDuration) are already updated,
        // so they become the new restore targets when the throttle lifts.
        m_radioModel.sendCommand(
            QString("display pan set %1 average=0").arg(applet->panId()));
        m_radioModel.sendCommand(
            QString("display pan set %1 weighted_average=0").arg(applet->panId()));
        // fps + line_duration go through the dispatcher rather than as Flex wire
        // text: on a backend that shapes its own display rate the reset updated
        // the widget only, leaving the backend cap and the pan's stored line
        // duration on the OLD values — so "reset display defaults" visibly did
        // not reset the rate. (#4470)
        if (!m_adaptiveThrottleActive)
            m_radioModel.requestPanDisplayRates(applet->panId(), 25, 100);
        auto* pan = m_radioModel.panadapter(applet->panId());
        if (pan && !pan->waterfallId().isEmpty()) {
            m_radioModel.sendCommand(
                QString("display panafall set %1 color_gain=50").arg(pan->waterfallId()));
            m_radioModel.sendCommand(
                QString("display panafall set %1 black_level=15").arg(pan->waterfallId()));
        }

        // Persist all defaults to AppSettings
        auto& s = AppSettings::instance();
        s.setValue(sw->settingsKey("DisplayFftFillAlpha"),        "0.70");
        s.setValue(sw->settingsKey("DisplayFftFillColor"),        "#00e5ff");
        s.setValue(sw->settingsKey("DisplayFftLineColor"),        "#00e5ff");
        s.setValue(sw->settingsKey("DisplayFftLineWidth"),        "2.0");
        s.setValue(sw->settingsKey("DisplayFftHeatMap"),          "True");
        s.setValue(sw->settingsKey("DisplayWfColorScheme"),       "0");
        s.setValue(sw->settingsKey("DisplayWfColorGain"),         "50");
        s.setValue(sw->settingsKey("DisplayWfBlackLevel"),        "15");
        s.setValue(sw->settingsKey("DisplayWfAutoBlack"),         "True");
        s.setValue(sw->settingsKey("DisplayWfAutoBlackRadioSide"), "False");
        s.setValue(sw->settingsKey("WaterfallBlankingEnabled"),   "False");
        s.setValue(sw->settingsKey("WaterfallBlankingThreshold"), "1.15");
        s.setValue(sw->settingsKey("WaterfallBlankingMode"),      "0");
        s.setValue(sw->settingsKey("CursorFreqLabel"),            "False");
        s.setValue(sw->settingsKey("BackgroundImage"),            ":/bg-default.jpg");
        s.setValue(sw->settingsKey("BackgroundOpacity"),          "80");
        s.setValue(sw->settingsKey("BackgroundFillColor"),        "#0a0a14");
        s.setValue(sw->settingsKey("DisplayFreqGridSpacing"),     "0");
        s.setValue(sw->settingsKey("DisplayNoiseFloorEnable"),    "False");
        s.setValue(sw->settingsKey("DisplayNoiseFloorPosition"),  "75");
        s.setValue(sw->settingsKey("DisplaySourceTraceSettings"),
                   QStringLiteral(
                       "{\"flex\":{\"dssFloorDepth\":6,\"noiseFloorPosition\":75},"
                       "\"kiwi\":{\"dssFloorDepth\":6,\"noiseFloorPosition\":75},"
                       "\"version\":1}"));
        s.setValue(sw->settingsKey("DisplaySpectrumRenderMode"),  "0");
        s.setValue(sw->settingsKey("Display3DFloorDepth"),        "6");
        s.save();

        // Apply the render-mode + 3D-floor reset to the widget too (the keys
        // above only update settings, not the live SpectrumWidget).
        sw->setSpectrumRenderMode(0);
        sw->setDssFloorDepth(6);
        // Writes the whole 3D object once, and unconditionally — the setters
        // early-return when a value already matches, which would otherwise
        // leave a stale object behind on a partial reset.
        sw->resetDisplay3DSettings();

        // Sync all Display panel UI controls (incl. the 2D/3D combo + 3D Floor).
        menu->syncDisplaySettings(0, 25, 70, false, QColor(0x00, 0xe5, 0xff),
                                  50, 15, true, 50, 100, 75, false, true, 0,
                                  true, 2.0f, false, 0, 6, 70,
                                  QColor(0x00, 0xe5, 0xff), 100);
        menu->syncExtraDisplaySettings(false, 1.15f, 80, 0,
                                       QColor(0x0a, 0x0a, 0x14));
    });
    connect(menu, &SpectrumOverlayMenu::displaySettingsCloneRequested,
            this, [this, applet, sw] {
        const int cloned = cloneDisplaySettingsToAllPans(applet);

        // Feedback on the source pan: the operator is looking at THIS panel, and
        // with a single pan open (or every other pan floated off-screen) a silent
        // button is indistinguishable from a broken one.
        PanadapterOverlayMessage message;
        message.id = QStringLiteral("display.clone-to-all-pans");
        message.timeoutMs = 4000;
        message.dismissible = true;
        if (cloned > 0) {
            message.title = tr("Display settings cloned");
            message.detail = tr("Applied to %n other panadapter(s).", nullptr, cloned);
            message.tone = PanadapterOverlayMessageTone::Info;
        } else {
            message.title = tr("Nothing to clone");
            message.detail = tr("This is the only open panadapter.");
            message.tone = PanadapterOverlayMessageTone::Warning;
        }
        sw->upsertOverlayMessage(std::move(message));
    });

    auto resolveClickedPanId = [this, sw]() -> QString {
        for (auto* panApplet : m_panStack->allApplets()) {
            if (panApplet->spectrumWidget() == sw)
                return panApplet->panId();
        }
        return {};
    };

    auto resolveSpectrumTuneTarget = [this, resolveClickedPanId]() -> SliceModel* {
        const QString panId = resolveClickedPanId();

        // Pan not in stack — legacy fallback to the active slice.
        if (panId.isEmpty())
            return activeSlice();

        if (auto* current = activeSlice(); current && current->panId() == panId)
            return current;

        for (auto* candidate : m_radioModel.slices()) {
            if (candidate->panId() == panId)
                return candidate;
        }

        // Empty pan (user closed the last slice with ✕ — the pan stays open
        // by design). Do NOT fall back to activeSlice(): that slice lives on
        // a *different* pan and would get dragged onto this band. Let the
        // caller decide (click → spawn slice; scroll → no-op). Fixes #3086.
        return nullptr;
    };

    // ── Click-to-tune ────────────────────────────────────────────────────
    // Same-pan and cross-pan tune requests both resolve a target slice first,
    // then run through the shared absolute/incremental tuning policy.
    connect(sw, &SpectrumWidget::frequencyClicked,
            this, [this, resolveSpectrumTuneTarget, resolveClickedPanId](double mhz) {
        if (auto* target = resolveSpectrumTuneTarget()) {
            queueActiveSliceForSpectrumTarget(target->sliceId());
            applyTuneRequest(target, mhz, TuneIntent::AbsoluteJump, "spectrum-click");
            return;
        }
        // Empty pan → spawn a new slice on it at the clicked frequency,
        // matching SmartSDR. addSliceOnPan honors the radio's max-slices
        // limit via sliceCreateFailed. Fixes #3086.
        const QString panId = resolveClickedPanId();
        if (!panId.isEmpty())
            m_radioModel.addSliceOnPan(panId, mhz);
    });
    connect(sw, &SpectrumWidget::kiwiSpotClicked,
            this, [this, resolveSpectrumTuneTarget, resolveClickedPanId](double mhz, const QString& mode, int loHz, int hiHz) {
        SliceModel* target = resolveSpectrumTuneTarget();
        if (!target) {
            // Empty pan → spawn a slice on it at the spot frequency, matching
            // the frequencyClicked path. Do NOT fall back to activeSlice():
            // that slice lives on a different pan (#3086). slice create is
            // async, so the mode/filter below cannot target the new slice.
            const QString panId = resolveClickedPanId();
            if (!panId.isEmpty())
                m_radioModel.addSliceOnPan(panId, mhz);
            return;
        }
        queueActiveSliceForSpectrumTarget(target->sliceId());
        applyTuneRequest(target, mhz, TuneIntent::AbsoluteJump, "kiwi-spot-click");

        const bool autoSwitchMode =
            AppSettings::instance().value("SpotAutoSwitchMode", "True").toString() == "True";
        if (autoSwitchMode) {
            const QString mappedMode = BandPlanManager::normalizeSpotMode(mode);
            if (!mappedMode.isEmpty()) {
                target->setMode(mappedMode);
            }
        }

        if (loHz != 0 || hiHz != 0) {
            target->setFilterWidth(loHz, hiHz);
        }
    });
    connect(sw, &SpectrumWidget::incrementalTuneRequested,
            this, [this, resolveSpectrumTuneTarget](double mhz) {
        if (auto* target = resolveSpectrumTuneTarget()) {
            queueActiveSliceForSpectrumTarget(target->sliceId());
            applyTuneRequest(target, mhz, TuneIntent::IncrementalTune, "spectrum-incremental");
        }
        // Empty pan + wheel scroll → no-op. A stray scroll gesture shouldn't
        // conjure a slice; click is the explicit "create here" affordance.
    });

    // ── Edge auto-pan step during slice drag (user-reported) ─────────────────
    // Pan the view AND tune the slice in one shot, deliberately bypassing the
    // pan-follow/reveal path: the SpectrumWidget already computed the explicit
    // center, so re-running reveal here would fight the velocity controller.
    // The slice tune uses setFrequency (autopan=0), so the radio doesn't
    // recenter either.
    connect(sw, &SpectrumWidget::edgePanTuneRequested,
            this, [this, resolveSpectrumTuneTarget](double centerMhz, double sliceFreqMhz) {
        auto* target = resolveSpectrumTuneTarget();
        if (!target) {
            return;
        }
        // Honour the same lock / SWR-sweep guards as applyTuneRequest: bypassing
        // it (to avoid pan-follow) must not also drop the lock affordance or
        // retune mid-sweep. A blocked tune pans nothing either, matching the
        // pre-existing locked-slice drag behaviour.
        if (tuneBlockedByGuards(target)) {
            return;
        }
        if (auto* pan = m_radioModel.panadapter(target->panId())) {
            // A kiwi-display pan has no radio-side span to catch up: its radio
            // geometry is frozen (#3825/#4081) and the widget already advanced
            // its own view for this drag tick. Writing the center through
            // anyway would pair it with the model's frozen bandwidth and snap
            // the zoom back to the kiwi-assignment span — see PanRecenterPolicy.
            const PanRecenterPolicy::Write panWrite =
                PanRecenterPolicy::recenterWrite(
                    kiwiSdrPanDisplaysKiwi(pan->panId()),
                    /*widgetOwnsViewDuringGesture=*/true);
            if (panWrite == PanRecenterPolicy::Write::RadioAndModel) {
                // Effective (pending-else-model) geometry, so a deferred write
                // in flight dedupes instead of re-issuing per drag tick (#4142).
                centerMhz = std::max(
                    centerMhz,
                    m_radioModel.effectivePanBandwidthMhz(pan->panId()) / 2.0);
                // In-window drag moves pass the unchanged center purely to tune
                // without reveal — only emit the pan command when it moves.
                if (!qFuzzyCompare(centerMhz,
                                   m_radioModel.effectivePanCenterMhz(pan->panId()))) {
                    // Deferred rather than dropped during a profile load
                    // (#4142). The SpectrumWidget owns its own view during an
                    // edge-pan drag, so the gesture still tracks the cursor;
                    // the radio catches up when the deferred center flushes.
                    //
                    // DRAG: the gesture this whole block exists for. On a
                    // slaved-scope backend it is the one centre write that is
                    // MEANT to move the radio — see
                    // RadioModel::requestPanCenter().
                    m_radioModel.requestPanCenter(
                        pan->panId(), centerMhz, -1.0,
                        IRadioBackend::PanCenterIntent::Drag);
                }
            }
        }
        queueActiveSliceForSpectrumTarget(target->sliceId());
        m_sliceDragTargetSliceId = target->sliceId();
        m_sliceDragTargetMhz = sliceFreqMhz;
        m_sliceDragEchoHoldUntilMs = 0;
        pushSliceFrequencyToOverlays(target, sliceFreqMhz);
        target->setFrequency(sliceFreqMhz);
    });
    // Center Lock stands down for the whole duration of a slice drag so it
    // doesn't fight the drag (in-window tune or edge auto-pan) with per-tick
    // recenters; on release it recenters once so the locked pan re-asserts.
    connect(sw, &SpectrumWidget::sliceDragActiveChanged, this, [this](bool active) {
        m_sliceDragInProgress = active;
        if (active) {
            m_sliceDragTargetSliceId = -1;
            m_sliceDragTargetMhz = 0.0;
            m_sliceDragEchoHoldUntilMs = 0;
            // A drag supersedes any in-flight wheel/MIDI tune: a live tune
            // hold would override the drag's overlay pushes with the stale
            // wheel target and recenter to it on release (#3854 review).
            m_centerLockTuneHoldBySlice.clear();
            return;
        }
        if (!active) {
            if (m_sliceDragTargetSliceId >= 0 && m_sliceDragTargetMhz > 0.0) {
                if (SliceModel* target = m_radioModel.slice(m_sliceDragTargetSliceId)) {
                    pushSliceFrequencyToOverlays(target, m_sliceDragTargetMhz);
                    target->setFrequency(m_sliceDragTargetMhz);
                }
                m_sliceDragEchoHoldUntilMs = QDateTime::currentMSecsSinceEpoch() + 350;
            }
            recenterCenterLocks();
            // A leave-kiwi reconcile that deferred behind this drag can run now.
            retryAllDeferredKiwiLeaveReconcile();
        }
    });

    connect(sw, &SpectrumWidget::centerLockRequested,
            this, [this, applet](int sliceId, bool locked) {
        if (!applet)
            return;

        const QString panId = applet->panId();
        if (panId.isEmpty())
            return;

        if (!locked) {
            setCenterLockForPan(panId, sliceId, false);
            return;
        }

        SliceModel* slice = m_radioModel.slice(sliceId);
        if (!slice || slice->panId() != panId)
            return;

        setCenterLockForPan(panId, sliceId, true);
    });
    connect(sw, &SpectrumWidget::sliceLinkRequested,
            this, [this](int aSliceId, int bSliceId, bool on) {
        if (on) {
            engageSliceLink(aSliceId, bSliceId);
        } else if (sliceLinkPeerOf(aSliceId) == bSliceId) {
            dissolveSliceLink(aSliceId, bSliceId, "menu request");
        }
    });
    restoreCenterLockForPan(applet->panId());
    sw->setCenterLockSliceId(centerLockSliceForPan(applet->panId()));
    refreshSliceLinkUi();

    // ── Spot trigger — notify the radio/TCI clients when a spot label is clicked (#341)
    connect(sw, &SpectrumWidget::spotTriggered, this, [this, applet](int spotIndex) {
        const auto& spots = m_radioModel.spotModel().spots();
        auto it = spots.find(spotIndex);
        if (it == spots.end()) return;

        if (it->source == "Memory") {
            int memoryIndex = memoryIndexFromSpotId(spotIndex);
            if (memoryIndex >= 0)
                activateMemorySpot(memoryIndex, applet->panId());
            return;
        }

        auto* s = preferredMemorySlice(applet->panId());
        const bool tciSpot = it->source.compare(QStringLiteral("TCI"), Qt::CaseInsensitive) == 0;
        // The two branches gate asymmetrically on purpose (#3145, #3152):
        //   TCI branch  — must mirror AetherSDR's own local-tune outcome, so it
        //                 suppresses when a local tune would also be blocked
        //                 (no slice, slice locked, or SWR sweep in progress).
        //                 Notifying Log4OM under lock/SWR would diverge the
        //                 client's view from the radio's actual state.
        //   Radio branch — delegates gating to the radio firmware via the
        //                  `spot trigger` command; the firmware applies its
        //                  own lock/SWR/passive checks. We only suppress
        //                  passive-local spots that have no upstream trigger.
        // Do not "normalise" these — collapsing the guards regresses Log4OM
        // behavior under lock or SWR sweep.
        if (tciSpot) {
            if (tciServer() && s && !s->isLocked() && !m_swrSweep.running)
                tciServer()->notifySpotClicked(spotIndex, s);
        } else if (!isPassiveLocalSpotId(spotIndex)) {
            m_radioModel.sendCommand(
                QString("spot trigger %1 pan=%2").arg(spotIndex).arg(applet->panId()));
        }

        // Auto-switch mode from spot metadata (#424). Default flipped to True
        // in #1846 since spots now ship with trigger_action=none — the radio
        // no longer changes mode on click, so auto-mode is the only path.
        if (AppSettings::instance().value("SpotAutoSwitchMode", "True").toString() != "True")
            return;
        if (!s) return;

        // FreeDV spots imply RADE — activate the RADE engine on this slice
        // (sets DIGU/DIGL by band convention + starts the OFDM modem) rather
        // than landing on a plain digital mode. Without HAVE_RADE the build
        // can't run the modem, so fall through to the regular mode-set. (#1846)
#ifdef HAVE_RADE
        if (it->source == "FreeDV") {
            activateRADE(s->sliceId());
            return;
        }
#endif
        const QString radioMode = SpotModeResolver::resolveSpotRadioMode(
            it->mode, it->comment, it->rxFreqMhz);
        if (!radioMode.isEmpty() && radioMode != s->mode())
            s->setMode(radioMode);
    });

    // ── Manual spot add/remove (#36)
    connect(sw, &SpectrumWidget::spotAddRequested, this,
            [this](double freqMhz, const QString& callsign, const QString& comment,
                   int lifetimeSec, bool forwardToCluster) {
        auto& as = AppSettings::instance();
        // Seed dedup so a cluster echo of our own DX command (when
        // forwardToCluster is on) is suppressed by isDuplicateSpot. (#2658)
        m_spotDedup[callsign] = { freqMhz, QDateTime::currentMSecsSinceEpoch() };
        QString call = QString(callsign).replace(' ', QChar(0x7f));
        QString freq = QString::number(freqMhz, 'f', 6);
        QString myCall = as.value("DxClusterCallsign").toString();
        if (myCall.isEmpty()) myCall = "AetherSDR";
        QString cmd = "spot add callsign=" + call + " rx_freq=" + freq
                     + " tx_freq=" + freq
                     + " source=Manual"
                     + " spotter_callsign=" + myCall
                     + " trigger_action=none"  // (#1846)
                     + " lifetime_seconds=" + QString::number(lifetimeSec);
        if (!comment.isEmpty())
            cmd += " comment=" + QString(comment).replace(' ', QChar(0x7f));
        QString spotColor = as.value("ManualSpotColor", "#00FF00").toString();
        if (spotColor.length() == 7) spotColor = "#FF" + spotColor.mid(1);
        cmd += " color=" + spotColor;
        if (SpotCommandPolicy::shouldSendSpotAddCommands()) {
            m_radioModel.sendCommand(cmd);
        } else {
            QMap<QString, QString> kvs;
            kvs["callsign"] = QString(callsign).replace(' ', QChar(0x7f));
            kvs["rx_freq"] = freq;
            kvs["tx_freq"] = freq;
            kvs["source"] = "Manual";
            kvs["spotter_callsign"] = myCall;
            kvs["lifetime_seconds"] = QString::number(lifetimeSec);
            kvs["timestamp"] = QString::number(QDateTime::currentSecsSinceEpoch());
            if (!comment.isEmpty())
                kvs["comment"] = QString(comment).replace(' ', QChar(0x7f));
            kvs["color"] = spotColor;

            const int spotId = m_nextPassiveSpotId--;
            m_radioModel.spotModel().applySpotStatus(spotId, kvs);
            if (lifetimeSec > 0) {
                m_passiveSpotExpiryMs.insert(
                    spotId,
                    QDateTime::currentMSecsSinceEpoch() + qint64(lifetimeSec) * 1000);
            }
        }

        // Forward to DX cluster if requested. Every failure mode here used to
        // be silent, so the operator saw the local "Manual" spot appear but had
        // no way to tell whether the forward reached the cluster — the report
        // behind #2857. Give explicit status-bar feedback on each outcome.
        if (forwardToCluster) {
            if (!m_dxCluster || !m_dxCluster->isConnected()) {
                // Most common cause in the field: only the RBN tab (read-only)
                // is connected, or the DX Cluster tab dropped. Say so instead
                // of no-op'ing.
                statusBar()->showMessage(
                    "DX Cluster not connected — spot not forwarded", 5000);
            } else {
                // DX Spider silently drops a `DX <freq> <call>` line with an
                // empty remark, which looks identical to "the forward did
                // nothing". Substitute a non-empty default so the spot is
                // accepted; the operator's own comment passes through unchanged.
                const QString remark = comment.isEmpty()
                    ? QStringLiteral("Spotted via AetherSDR") : comment;
                QString dxCmd = QString("DX %1 %2 %3")
                    .arg(freqMhz * 1000.0, 0, 'f', 1)
                    .arg(callsign)
                    .arg(remark);
                QMetaObject::invokeMethod(m_dxCluster, [this, dxCmd] {
                    m_dxCluster->sendCommand(dxCmd);
                });
                statusBar()->showMessage(
                    QString("Spot forwarded to DX Cluster: %1").arg(callsign), 5000);
            }
        }
    });
    connect(sw, &SpectrumWidget::spotRemoveRequested, this, [this](int spotIndex) {
        if (isPassiveLocalSpotId(spotIndex)) {
            m_passiveSpotExpiryMs.remove(spotIndex);
            m_radioModel.spotModel().removeSpot(spotIndex);
            return;
        }
        m_radioModel.sendCommand(QString("spot remove %1").arg(spotIndex));
    });

    // ── +RX / +TNF buttons ───────────────────────────────────────────────
    connect(menu, &SpectrumOverlayMenu::addRxClicked,
            this, [this](const QString& panId) {
        int limit = m_radioModel.maxSlices();
        if (m_radioModel.slices().size() < limit) {
            m_radioModel.addSliceOnPan(panId);
        } else {
            statusBar()->showMessage(
                QString("%1 supports a maximum of %2 slices")
                    .arg(m_radioModel.model()).arg(limit), 4000);
        }
    });
    connect(menu, &SpectrumOverlayMenu::addTnfClicked,
            this, [this]() {
        auto* s = activeSlice();
        if (!s) return;
        double tnfFreq = s->frequency()
            + (s->filterLow() + s->filterHigh()) / 2.0 / 1.0e6;
        m_radioModel.tnfModel().createTnf(tnfFreq);
    });
    connect(menu, &SpectrumOverlayMenu::memoryActivated,
            this, [this](int memoryIndex, const QString& panId) {
        activateMemorySpot(memoryIndex, panId);
    });
    connect(menu, &SpectrumOverlayMenu::quickAddMemoryRequested,
            this, [this](const QString& panId) {
        showQuickAddMemoryDialog(panId);
    });
    connect(menu, &SpectrumOverlayMenu::kiwiRxAntennaSelected,
            this, &MainWindow::setKiwiSdrVirtualAntennaForSlice);
    connect(menu, &SpectrumOverlayMenu::flexRxAntennaSelected,
            this, &MainWindow::clearKiwiSdrVirtualAntennaForSlice);

    // ── Slice marker clicks ──────────────────────────────────────────────
    connect(sw, &SpectrumWidget::sliceClicked,
            this, &MainWindow::setActiveSlice);
    connect(sw, &SpectrumWidget::offScreenSliceCenterRequested,
            this, [this](int sliceId) {
        SliceModel* slice = m_radioModel.slice(sliceId);
        if (!slice) {
            return;
        }
        const QString panId = slice->panId();
        const int lockedSliceId = centerLockSliceForPan(panId);
        const double targetMhz = slice->frequency();
        setActiveSliceInternal(sliceId, false);
        if (lockedSliceId < 0) {
            centerActiveSliceInPanadapter(true, targetMhz);
        } else if (lockedSliceId == sliceId) {
            recenterCenterLockForPan(panId);
        }
    });
    connect(sw, &SpectrumWidget::sliceTxRequested,
            this, [this](int sliceId) {
        if (auto* s = m_radioModel.slice(sliceId))
            s->setTxSlice(true);
    });
    connect(sw, &SpectrumWidget::sliceCloseRequested,
            this, [this](int sliceId) {
        if (m_radioModel.slices().size() <= 1) return;
        if (SliceModel* slice = m_radioModel.slice(sliceId);
            centerLockActiveForSlice(slice)) {
            clearCenterLockForPan(slice->panId(), true);
        }
        m_radioModel.sendCommand(QString("slice remove %1").arg(sliceId));
    });
    connect(sw, &SpectrumWidget::sliceCreateRequested,
            this, [this, applet](double freqMhz) {
        const int limit = m_radioModel.maxSlices();
        if (m_radioModel.slices().size() < limit) {
            m_radioModel.addSliceOnPan(applet->panId(), freqMhz);
        } else {
            statusBar()->showMessage(
                QString("%1 supports a maximum of %2 slices")
                    .arg(m_radioModel.model()).arg(limit), 4000);
        }
    });
    connect(sw, &SpectrumWidget::sliceTuneRequested,
            this, [this](int sliceId, double freqMhz) {
        if (auto* s = m_radioModel.slice(sliceId))
            applyTuneRequest(s, freqMhz, TuneIntent::AbsoluteJump, "slice-move-here");
    });

    // ── Band selection ───────────────────────────────────────────────────
    connect(menu, &SpectrumOverlayMenu::bandSelected,
            this, [this, applet]
                  (const QString& bandName, double freqMhz, const QString& mode,
                   const QString& stackKeyHint) {
        qDebug() << "MainWindow: switching to band" << bandName
                 << "freq:" << freqMhz << "mode:" << mode;

        // Maintainer note: keep band changes radio-authoritative.
        //
        // The Flex band stack owns the state users expect to survive a band
        // jump: frequency, mode, filters, pan center, bandwidth/zoom, and
        // built-in antenna selection. Aether should therefore send exactly one
        // `display pan set <panId> band=<key>` command when it can form a
        // spec-correct key. Do not use the `freqMhz` / `mode` arguments as a
        // local fallback; those are static UI defaults, and the old fallback
        // reset users to band-center SSB instead of restoring their saved stack
        // state (#1876, #1852, #1856, #1849).
        //
        // UI band names are not always protocol keys:
        //   - Native bands are displayed as "20m", "630m", etc., but Flex
        //     expects bare keys such as "20" and "630".
        //   - XVTR names are user labels such as "2m" or "70cm"; do not strip
        //     those into native-band keys. Flex expects `X<index>`, where
        //     `index` is the xvtr status-object number from `xvtr <n>` messages
        //     (0-based), not the radio's 1-based setup-order field (#2342).
        //   - WWV / GEN use numeric band-stack slots 33 / 34 from SmartSDR
        //     capture history (#1540/#1211).
        //   - Built-in 4m / 2m hardware bands use bare keys "4" / "2" only
        //     when the connected model reports those capabilities.
        //   - Configured XVTR buttons also pass explicit X<n> keys so a
        //     user XVTR named "4m" can still be selected on a radio that
        //     also has native 4m hardware.
        //
        // If no supported mapping exists, refuse the band change and leave the
        // current slice/pan state untouched. Guessing is worse than failing
        // visibly because a wrong tune destroys the very band-stack state this
        // path exists to preserve.
        // ── Radios with no band stack of their own ────────────────────────
        //
        // Everything below this branch is Flex band-stack machinery: it resolves
        // a protocol band key and sends "display pan set … band=", and the radio
        // restores frequency, mode, filters and antenna from state IT owns. A
        // Hermes-Lite 2 owns none of that — it has no VFO to read back and no
        // stack to restore — so that command reached nothing and the band
        // buttons did precisely nothing on this family.
        //
        // Here the APP is authoritative, so the freqMhz/mode the button carries
        // are used directly. That is the exact opposite of the rule stated above
        // for Flex, and deliberately so: those arguments are "static UI
        // defaults" only when something better exists, and on this family
        // nothing does. Tuning to band centre in the band's usual mode is what
        // every radio without a stack does.
        if (!m_radioModel.usesFlexCommandPlane()) {
            const RadioCapabilities caps = m_radioModel.backendCapabilities();
            const double hz = freqMhz * 1.0e6;
            // Refuse rather than tune somewhere the receiver cannot hear. Only
            // when the backend actually reported a range — a backend that
            // reports none keeps the previous unconditional behaviour.
            if (caps.tuningMaxHz > caps.tuningMinHz
                && (hz < caps.tuningMinHz || hz > caps.tuningMaxHz)) {
                const QString reason =
                    tr("%1 is outside this radio's tuning range (%2–%3 MHz)")
                        .arg(bandName)
                        .arg(caps.tuningMinHz / 1.0e6, 0, 'f', 3)
                        .arg(caps.tuningMaxHz / 1.0e6, 0, 'f', 3);
                qCWarning(lcProtocol).noquote() << "MainWindow: " << reason;
                statusBar()->showMessage(reason, 5000);
                return;
            }

            SliceModel* s = activeSlice();
            if (!s) {
                statusBar()->showMessage(tr("No active slice to tune"), 4000);
                return;
            }
            clearSwrSweepForBandChange(s->sliceId(), applet->panId(), bandName);
            m_bandSettings.setCurrentBand(bandName);
            // MODE FIRST, then frequency. The backend adopts a mode-appropriate
            // passband on a mode CHANGE (Hl2Backend::setSliceMode), and the
            // frequency move is what re-selects the companion filter board — so
            // this order leaves both the passband and the band filter settled
            // for the band being arrived at, not the one being left.
            if (!mode.isEmpty())
                s->setMode(mode);
            s->setFrequency(freqMhz);
            // Centre the window on the new band. Without this the panadapter
            // keeps the old band's NCO until the tune happens to fall outside
            // the usable passband, so a band change could leave the trace
            // centred a whole band away from the slice that just moved.
            m_radioModel.requestPanCenter(applet->panId(), freqMhz);
            qCDebug(lcProtocol).noquote().nospace()
                << "MainWindow: band switch (no radio band stack) band=" << bandName
                << " pan=" << applet->panId()
                << " freq_mhz=" << QString::number(freqMhz, 'f', 6)
                << " mode=" << mode;
            return;
        }

        const auto xvtrs = xvtrPolicyBandsFrom(m_radioModel.xvtrList());
        QString stackKey = stackKeyHint;
        QString unsupportedBandReason;
        if (stackKey.isEmpty()) {
            const auto stackKeyResult =
                XvtrPolicy::resolveBandStackKey(bandName, xvtrs, m_radioModel.capabilities());
            stackKey = stackKeyResult.key;
            unsupportedBandReason = stackKeyResult.unsupportedReason;
        }

        if (stackKey.isEmpty()
            && m_radioModel.declaredBands().contains(bandName, Qt::CaseInsensitive)) {
            // Radio-declared band (see RadioModel::declaredBands): the radio
            // told us it tunes this natively, so pass the declared name
            // through as the band-stack key — the declaring radio defines
            // and honours these keys (e.g. band=440 on an IC-9700 gateway).
            // Real Flex radios never declare bands, so their unsupported-band
            // refusal below is unchanged.
            //
            // NB the resolveBandStackKey() attempt above runs FIRST, so a
            // declared band that ALSO matches the impersonated model's native
            // capability resolves natively and never reaches here — e.g. a
            // gateway advertising FLEX-6700 (has2Meters) + bands=2m,440,23cm
            // sends the bare native key `2` for 2m, but the declared tokens
            // `440`/`23cm` for the bands the model has no native slot for. A
            // declaring bridge must therefore honour both the bare Flex keys
            // and its declared tokens, per band.
            //
            // The same FIRST-wins rule means a user XVTR labelled e.g. "2m"
            // out-ranks a gateway's declared "2m": resolveBandStackKey()
            // resolves the XVTR to its X<n> key above and this fallthrough
            // never runs. That is intentional and consistent with the
            // XVTR-priority precedent already codified for native bands
            // (#2342, above) — declared tokens do not override a same-named
            // user XVTR (#4191, follow-up #2 from the #4027 review).
            //
            // The contains() match is CaseInsensitive so a declared-band
            // selection arriving from a non-UI path (memory-channel recall,
            // CAT) with off-canonical casing like "23CM" still matches the
            // canonical BandDefs spelling parseDeclaredBands() stored; UI band
            // buttons already emit canonical names (#4191, follow-up #3).
            stackKey = bandName;
        }

        if (stackKey.isEmpty()) {
            qCWarning(lcProtocol).noquote().nospace()
                << "MainWindow: refusing unsupported band change band=" << bandName
                << " reason=" << unsupportedBandReason
                << " available_xvtrs=" << xvtrListSummary(xvtrs);
            statusBar()->showMessage(unsupportedBandReason, 5000);
            return;
        } else {
            qCDebug(lcProtocol).noquote().nospace()
                << "MainWindow: band switch band=" << bandName
                << " pan=" << applet->panId()
                << " key=" << stackKey
                << " freq_hint_mhz=" << QString::number(freqMhz, 'f', 6)
                << " mode_hint=" << mode
                << " xvtr=" << xvtrForBandSummary(bandName, xvtrs);
            // A band stack restore is radio-authoritative and may carry a dBm
            // range different from a just-released scale drag. Abandon that
            // client request before the band command so the restored range
            // cannot be rejected as a stale echo and wedge FFT decoding.
            emit bandStackRestoreStarting(applet->panId());
            clearSwrSweepForBandChange(-1, applet->panId(), bandName);
            m_bandSettings.setCurrentBand(bandName);
            // #4142: during the profile-load hold a bare sendCommand() band=
            // write is silently destroyed. requestPanBand defers it instead;
            // panBandAboutToDispatch starts every slice/Center Lock/Kiwi recall
            // guard immediately before the command actually reaches the wire.
            m_radioModel.requestPanBand(applet->panId(), stackKey);
            QTimer::singleShot(300, this, [this, panId = applet->panId()]() {
                reassertUnmutedSliceAudioForPan(panId);
            });
        }
    });

    // XVTR button → open Radio Setup XVTR tab (#571)
    connect(menu, &SpectrumOverlayMenu::xvtrSetupRequested,
            this, [this]() {
        const QString prevComp = m_radioModel.audioCompressionParam();
        const bool wasFresh = !m_radioSetupDialog;
        showOrRaisePersistent(m_radioSetupDialog,
                              &m_radioModel, m_audio,
                              &m_tgxlConn, &m_pgxlConn, &m_antennaGenius,
                              m_kiwiSdrManager, &m_acomConn, &m_speConn, &m_vkampConn);
        if (wasFresh && m_radioSetupDialog)
            wireRadioSetupDialogSignals(m_radioSetupDialog, prevComp);
        if (m_radioSetupDialog)
            m_radioSetupDialog->selectTab(QStringLiteral("XVTR"));
    });

    // ── WNB / RF Gain ────────────────────────────────────────────────────
    connect(menu, &SpectrumOverlayMenu::wnbToggled,
            this, [this, sw, applet](bool on) {
        m_radioModel.sendCommand(
            QString("display pan set %1 wnb=%2").arg(applet->panId()).arg(on ? 1 : 0));
        // The radio echoes WNB state, level, and normalization progress.
        // Let PanadapterModel drive the spectrum indicator and related menus.
        auto& s = AppSettings::instance();
        s.setValue(sw->settingsKey("DisplayWnbEnabled"), on ? "True" : "False");
        s.save();
    });
    connect(menu, &SpectrumOverlayMenu::wnbLevelChanged,
            this, [this, sw, applet](int level) {
        m_radioModel.sendCommand(
            QString("display pan set %1 wnb_level=%2").arg(applet->panId()).arg(level));
        auto& s = AppSettings::instance();
        s.setValue(sw->settingsKey("DisplayWnbLevel"), QString::number(level));
        s.save();
    });
    connect(menu, &SpectrumOverlayMenu::rfGainChanged,
            this, [this, sw, applet](int gain) {
        // Through the model, not raw wire text. RadioModel::setPanRfGain routes
        // a non-Flex backend's gain through the seam (the HL2 keeps it in an
        // AD9866 register, not in a "display pan set" command), and emitting the
        // Flex string here bypassed that entirely — the slider moved, the value
        // persisted, and nothing reached the radio.
        m_radioModel.setPanRfGainFor(applet->panId(), gain);
        sw->setRfGain(gain);
        auto& s = AppSettings::instance();
        s.setValue(rfGainSettingsKey(sw), QString::number(gain));
        s.save();
    });
    connect(menu, &SpectrumOverlayMenu::loopAToggled,
            this, [this, applet](bool on) {
        m_radioModel.sendCommand(
            QString("display pan set %1 loopa=%2").arg(applet->panId()).arg(on ? 1 : 0));
    });
    connect(menu, &SpectrumOverlayMenu::loopBToggled,
            this, [this, applet](bool on) {
        m_radioModel.sendCommand(
            QString("display pan set %1 loopb=%2").arg(applet->panId()).arg(on ? 1 : 0));
    });
    connect(menu, &SpectrumOverlayMenu::swrSweepStartRequested,
            this, &MainWindow::startSwrSweep);
    connect(menu, &SpectrumOverlayMenu::swrSweepClearRequested,
            this, &MainWindow::clearSwrSweepPlot);
    connect(menu, &SpectrumOverlayMenu::swrSweepSaveCsvRequested,
            this, &MainWindow::saveSwrSweepCsv);

    // Client-side DSP toggles (NR2 / RN2 / NR4 / MNR / BNR / DFNR) now
    // live exclusively in the AetherDSP applet; the spectrum overlay menu
    // no longer surfaces them.
    refreshKiwiSdrWaterfallAvailability();
}

MainWindow::TuneCenteringResult MainWindow::revealFrequencyIfNeeded(
    SliceModel* slice, double mhz, TuneIntent intent, const char* source,
    double leftFlagEdgeOffsetMhz, double rightFlagEdgeOffsetMhz)
{
    TuneCenteringResult result;
    if (!slice)
        return result;

    auto* pan = m_radioModel.panadapter(slice->panId());
    if (!pan)
        return result;

    // Prefer the visible spectrum state when available so reveal/follow stays
    // aligned with recent manual pan/zoom gestures even if the model echo lags.
    auto* sw = spectrumForSlice(slice);
    result.oldCenterMhz = sw ? sw->centerMhz() : pan->centerMhz();
    result.newCenterMhz = result.oldCenterMhz;
    result.bandwidthMhz = sw ? sw->bandwidthMhz() : pan->bandwidthMhz();

    const double bandwidthMhz = result.bandwidthMhz;
    const double halfBw = bandwidthMhz / 2.0;
    if (halfBw <= 0.0)
        return result;

    if (centerLockSliceForPan(pan->panId()) >= 0) {
        return result;
    }

    if (intent == TuneIntent::CommandedTargetCenter) {
        result.newCenterMhz = mhz;
        if (qFuzzyCompare(result.oldCenterMhz, result.newCenterMhz))
            return result;

        result.followRevealTriggered = true;
        result.hardCenterUsed = true;
        const double centerDelta = std::abs(result.newCenterMhz - result.oldCenterMhz);
        result.animationDurationMs = (centerDelta <= halfBw * 0.25)
            ? kPanFollowAnimationDurationMs
            : 0;

        applyTuneCenteringWrite(pan, sw, result.newCenterMhz);
        return result;
    }

    const bool incremental = (intent == TuneIntent::IncrementalTune);
    if (incremental && !panFollowEnabled())
        return result;

    const bool isSpectrumClick = source && qstrcmp(source, "spectrum-click") == 0;
    // Spectrum clicks settle with a tight margin (the user picked the spot);
    // off-screen-indicator reveals and other non-incremental tunes share the
    // wider comfort margin so the slice lands clearly inside the visible band
    // rather than on the edge.  RevealOffscreen previously used 0.0 — partially
    // clipped and indistinguishable from "still off-screen".  (#2941, regression of #2371.)
    const double comfortMargin = isSpectrumClick
        ? kSpectrumClickEdgeMarginFrac
        : kRevealComfortEdgeMarginFrac;
    const double triggerEdgeMarginFrac =
        incremental ? kIncrementalTriggerEdgeMarginFrac : comfortMargin;
    const double settleEdgeMarginFrac =
        incremental ? kIncrementalSettleEdgeMarginFrac : comfortMargin;
    const double triggerDistanceFromCenter = halfBw - bandwidthMhz * triggerEdgeMarginFrac;
    const double settleDistanceFromCenter = halfBw - bandwidthMhz * settleEdgeMarginFrac;
    const double center = result.oldCenterMhz;
    const int stepHz = incremental
        ? (slice->stepHz() > 0 ? slice->stepHz() : (sw ? sw->stepSize() : 0))
        : 0;
    const double stepMhz = stepHz > 0 ? stepHz / 1e6 : 0.0;

    auto incrementalFollowCenter = [&](double triggerBoundaryCenter,
                                       double settleCenterTarget) {
        const double direction = (triggerBoundaryCenter >= center) ? 1.0 : -1.0;
        const double overshootMhz = std::abs(triggerBoundaryCenter - center);
        const double quantizedDeltaMhz = quantizeIncrementalFollowDelta(overshootMhz, stepMhz);
        const double cappedDeltaMhz =
            std::min(quantizedDeltaMhz, std::abs(settleCenterTarget - center));
        return center + direction * cappedDeltaMhz;
    };

    // The IncrementalTune trigger compares the *outer edge of the VFO flag*
    // (mhz ± flag-width-in-MHz) against the pan boundary, so the flag panel
    // never clips the pan edge.  Non-flag callers pass 0.0 for both offsets
    // and effectiveLeftMhz / effectiveRightMhz collapse back to mhz —
    // preserving the original slice-frequency comparison.  Clamp the offsets
    // so neither effective edge crosses the *other* trigger boundary at very
    // narrow panadapters (avoids the trigger oscillating around the slice).
    // (#2761)
    const double safeOffsetCap = std::max(0.0, triggerDistanceFromCenter * 0.95);
    const double clampedRightOffset = std::min(rightFlagEdgeOffsetMhz, safeOffsetCap);
    const double clampedLeftOffset  = std::min(leftFlagEdgeOffsetMhz,  safeOffsetCap);
    const double effectiveLeftMhz   = incremental ? mhz - clampedLeftOffset  : mhz;
    const double effectiveRightMhz  = incremental ? mhz + clampedRightOffset : mhz;

    if (effectiveRightMhz > center + triggerDistanceFromCenter) {
        const double settleCenterTarget = effectiveRightMhz - settleDistanceFromCenter;
        if (incremental && stepMhz > 0.0) {
            const double triggerBoundaryCenter = effectiveRightMhz - triggerDistanceFromCenter;
            result.newCenterMhz =
                incrementalFollowCenter(triggerBoundaryCenter, settleCenterTarget);
        } else {
            result.newCenterMhz = settleCenterTarget;
        }
    } else if (effectiveLeftMhz < center - triggerDistanceFromCenter) {
        const double settleCenterTarget = effectiveLeftMhz + settleDistanceFromCenter;
        if (incremental && stepMhz > 0.0) {
            const double triggerBoundaryCenter = effectiveLeftMhz + triggerDistanceFromCenter;
            result.newCenterMhz =
                incrementalFollowCenter(triggerBoundaryCenter, settleCenterTarget);
        } else {
            result.newCenterMhz = settleCenterTarget;
        }
    } else {
        return result;
    }

    if (qFuzzyCompare(result.oldCenterMhz, result.newCenterMhz))
        return result;

    result.followRevealTriggered = true;
    const double centerDelta = std::abs(result.newCenterMhz - result.oldCenterMhz);
    result.animationDurationMs = (centerDelta <= halfBw * 0.25)
        ? kPanFollowAnimationDurationMs
        : 0;

    // Apply the center so the owning spectrum repaints immediately;
    // SpectrumWidget decides whether that becomes a short retargetable animation
    // or an immediate snap based on shift size.
    applyTuneCenteringWrite(pan, sw, result.newCenterMhz);
    return result;
}

// Shared write step for revealFrequencyIfNeeded's recenter decisions. A
// flex-display pan writes through the radio and model — #4142: defer, never
// drop; during a profile load the wire write is suppressed and requestPanCenter()
// advances the model only when the command actually goes out, replaying it once
// the hold lifts, so the pan keeps showing truthful spectrum for its real span.
// A kiwi-display pan's radio geometry is frozen (#3825/#4081): a center-only
// write-through would re-broadcast the model's frozen bandwidth over the
// widget's live zoom and snap the view back to the kiwi-assignment span, so the
// recenter is applied to the widget alone — see PanRecenterPolicy.
void MainWindow::applyTuneCenteringWrite(PanadapterModel* pan,
                                         SpectrumWidget* sw,
                                         double newCenterMhz)
{
    if (!pan)
        return;

    switch (PanRecenterPolicy::recenterWrite(
                kiwiSdrPanDisplaysKiwi(pan->panId()),
                /*widgetOwnsViewDuringGesture=*/false)) {
    case PanRecenterPolicy::Write::RadioAndModel:
        m_radioModel.requestPanCenter(pan->panId(), newCenterMhz);
        break;
    case PanRecenterPolicy::Write::WidgetLocal:
        if (sw) {
            const double bwMhz = PanRecenterPolicy::recenterBandwidthMhz(
                /*kiwiDisplayActive=*/true, sw->bandwidthMhz(),
                pan->bandwidthMhz());
            // Low edge stays >= 0 Hz, matching every other center writer
            // (snapCenterLockForSlice's kiwi branch, the gesture paths, and
            // dispatchPanCenterBandwidth on the flex side).
            sw->setFrequencyRange(std::max(newCenterMhz, bwMhz / 2.0), bwMhz);
        }
        break;
    case PanRecenterPolicy::Write::None:
        break;
    }
}

MainWindow::TuneCenteringResult MainWindow::panFollowVfo(
    SliceModel* s, double mhz, const char* source)
{
    // Incremental tuning uses a trigger margin and a slightly wider settle
    // margin so the slice glides back into a comfortable visible area without
    // dead-centering or waiting until it actually crosses the pan edge.
    //
    // Per #2761, when this slice has a visible VFO flag, the trigger compares
    // against the *outer edge of the flag* rather than the slice frequency
    // itself — so the flag panel doesn't clip the pan edge before the pan
    // starts to scroll.  Split pairs (LockLeft + LockRight rendered on the
    // same marker) extend the trigger on *both* sides; non-split slices extend
    // on the side the flag placement rules predict for the new frequency.
    // Non-flagged and compact-mode slices fall through to the original
    // slice-frequency comparison (both offsets stay 0.0).
    double leftFlagOffsetMhz  = 0.0;
    double rightFlagOffsetMhz = 0.0;
    if (auto* sw = spectrumForSlice(s)) {
        if (auto* vfo = sw->vfoWidget(s->sliceId())) {
            const double bw = sw->bandwidthMhz();
            // Frequency maps across the content canvas (width minus the right
            // dBm / time strip), so convert the flag's pixel width to MHz with
            // the same denominator mhzToX uses — otherwise the flag-edge trigger
            // drifts from the rendered flag position by the strip ratio (#3482).
            const int specW = sw->contentWidth();
            if (bw > 0.0 && specW > 0 && !vfo->isCollapsed()) {
                const double mhzPerPixel = bw / static_cast<double>(specW);
                const double flagWidthMhz =
                    static_cast<double>(vfo->width()) * mhzPerPixel;
                if (sw->sliceHasSplitPartner(s->sliceId())) {
                    // Split pair: LockLeft + LockRight, flags on both sides.
                    leftFlagOffsetMhz  = flagWidthMhz;
                    rightFlagOffsetMhz = flagWidthMhz;
                } else if (sw->vfoFlagOnLeftForSlice(
                               s->sliceId(), mhz, vfo->width(), vfo->onLeft())) {
                    leftFlagOffsetMhz  = flagWidthMhz;
                } else {
                    rightFlagOffsetMhz = flagWidthMhz;
                }
            }
        }
    }

    return revealFrequencyIfNeeded(s, mhz, TuneIntent::IncrementalTune, source,
                                   leftFlagOffsetMhz, rightFlagOffsetMhz);
}

void MainWindow::wireVfoWidget(VfoWidget* w, SliceModel* s)
{
    const int sliceId = s->sliceId();

    // Bidirectional SQL sync — the VfoWidget mirrors the RxApplet's 3-way
    // SQL state (Off / Manual / Auto), the manual-level cache, and the
    // Auto dB margin.  RxApplet is the source of truth; VfoWidget pipes
    // its own button + slider events through RxApplet so persistence,
    // algorithm enable/disable, and the manual cache stay in one place.
    if (m_appletPanel && m_appletPanel->rxApplet())
        w->setRxApplet(m_appletPanel->rxApplet());
    w->setKiwiSdrManager(m_kiwiSdrManager);

    // Note: w->setSlice(s) is called at the end of this method (line ~1895)

    // Per-slice signals — these are specific to the slice this widget represents
    connect(w, &VfoWidget::kiwiRxAntennaSelected,
            this, &MainWindow::setKiwiSdrVirtualAntennaForSlice);
    connect(w, &VfoWidget::flexRxAntennaSelected,
            this, &MainWindow::clearKiwiSdrVirtualAntennaForSlice);
    connect(s, &SliceModel::frequencyChanged, this, [this, s](double) {
        updateKiwiSdrVirtualTrackingForSlice(s);
    });
    connect(s, &SliceModel::modeChanged, this, [this, s](const QString&) {
        updateKiwiSdrVirtualTrackingForSlice(s);
    });
    connect(s, &SliceModel::filterChanged, this, [this, s](int, int) {
        updateKiwiSdrVirtualTrackingForSlice(s);
    });
    connect(s, &SliceModel::panIdChanged, this, [this, s](const QString&) {
        updateKiwiSdrVirtualTrackingForSlice(s);
    });
    // Re-send the tracked-slice command when the radio's CW pitch changes so
    // an already-active KiwiSDR CW session's BFO offset stays in sync (#4423)
    // instead of going stale until the next frequency/mode/filter edit.
    // cwPitchChanged (not phoneStateChanged) so this doesn't re-run on every
    // unrelated VOX/mic/dexp status update for every wired slice.
    // Context is `s` (not `this`) so Qt disconnects when the slice dies —
    // slices come and go far more often than MainWindow does (band changes,
    // stale-slice pruning); that was a real UAF fixed earlier on this PR.
    // But that leaves the captured `this` untracked by Qt's own lifetime
    // handling, so guard it separately with a QPointer (same idiom as
    // AetherDspWidget::onDspToggled) for the rare case MainWindow is torn
    // down while the slice and TransmitModel are still alive.
    QPointer<MainWindow> self(this);
    connect(&m_radioModel.transmitModel(), &TransmitModel::cwPitchChanged,
            s, [self, s](int) {
        if (self) {
            self->updateKiwiSdrVirtualTrackingForSlice(s);
        }
    });
    connect(s, &SliceModel::audioGainChanged, this, [this, s](float) {
        updateKiwiSdrVirtualAudioControlsForSlice(s);
        updateAetherDspModePolicy();
    });
    connect(s, &SliceModel::audioMuteChanged, this, [this, s](bool) {
        updateKiwiSdrVirtualAudioControlsForSlice(s);
        syncFlexRxPanToAudioEngine();
        updateAetherDspModePolicy();
    });
    connect(s, &SliceModel::audioPanChanged, this, [this, s](int) {
        updateKiwiSdrVirtualAudioControlsForSlice(s);
    });
    connect(s, &SliceModel::agcModeChanged, this, [this, s](const QString&) {
        updateKiwiSdrVirtualReceiverControlsForSlice(s);
    });
    connect(s, &SliceModel::agcThresholdChanged, this, [this, s](int) {
        updateKiwiSdrVirtualReceiverControlsForSlice(s);
    });
    connect(s, &SliceModel::agcOffLevelChanged, this, [this, s](int) {
        updateKiwiSdrVirtualReceiverControlsForSlice(s);
    });
    connect(s, &SliceModel::externalReceiveAgcModeChanged,
            this, [this, s](const QString&) {
        updateKiwiSdrVirtualReceiverControlsForSlice(s);
    });
    connect(s, &SliceModel::externalReceiveAgcThresholdChanged,
            this, [this, s](int) {
        updateKiwiSdrVirtualReceiverControlsForSlice(s);
    });
    connect(s, &SliceModel::externalReceiveAgcOffLevelChanged,
            this, [this, s](int) {
        updateKiwiSdrVirtualReceiverControlsForSlice(s);
    });
    connect(s, &SliceModel::squelchChanged, this, [this, s](bool, int) {
        updateKiwiSdrVirtualReceiverControlsForSlice(s);
    });
    connect(s, &SliceModel::externalReceiveSquelchChanged,
            this, [this, s](bool, int) {
        updateKiwiSdrVirtualReceiverControlsForSlice(s);
    });
    connect(s, &SliceModel::externalReceiveAutoSquelchChanged,
            this, [this, s](bool) {
        updateKiwiSdrVirtualReceiverControlsForSlice(s);
        syncActiveSliceAutoSquelchToSpectrums();
        syncActiveSliceSquelchLineToSpectrums();
    });
    connect(m_appletPanel->rxApplet(), &RxApplet::autoSqlMarginDbChanged,
            s, [this, sliceId](int) {
        if (sliceId == m_activeSliceId) {
            updateKiwiSdrVirtualReceiverControlsForSlice(
                m_radioModel.slice(sliceId));
        }
    });
    connect(s, &SliceModel::diversityChanged, this, [this, s](bool) {
        pushSliceOverlay(s);
        syncKiwiSdrDiversityEscControls();
    });
    connect(w, &VfoWidget::closeSliceRequested, this, [this, sliceId]() {
        if (m_radioModel.slices().size() <= 1) return;
        if (SliceModel* slice = m_radioModel.slice(sliceId);
            centerLockActiveForSlice(slice)) {
            clearCenterLockForPan(slice->panId(), true);
        }
        m_radioModel.sendCommand(QString("slice remove %1").arg(sliceId));
    });
    connect(w, &VfoWidget::stepTuneRequested, this, [this, sliceId](double mhz) {
        if (auto* sl = m_radioModel.slice(sliceId))
            applyTuneRequest(sl, mhz, TuneIntent::IncrementalTune, "vfo-wheel");
    });
    connect(w, &VfoWidget::directEntryCommitted, this, [this, sliceId](double mhz, const QString& source) {
        if (auto* sl = m_radioModel.slice(sliceId)) {
            const QByteArray sourceUtf8 = source.toUtf8();
            applyTuneRequest(sl, mhz, TuneIntent::CommandedTargetCenter, sourceUtf8.constData());
        }
    });
    connect(w, &VfoWidget::lockToggled, this, [this, sliceId](bool locked) {
        if (auto* sl = m_radioModel.slice(sliceId))
            sl->setLocked(locked);
    });
    connect(w, &VfoWidget::afGainChanged, this, [this, sliceId](int v) {
        if (auto* sl = m_radioModel.slice(sliceId))
            sl->setAudioGain(v);
    });
    // Record/playback — route to radio or client-side QsoRecorder (#1297)
    connect(w, &VfoWidget::recordToggled, this, [this, w, sliceId](bool on) {
        bool clientSide = AppSettings::instance().value("RecordingMode", "Client").toString() == "Client";
        if (clientSide) {
            if (on)
                m_qsoRecorder->startRecording();
            else
                m_qsoRecorder->stopRecording();
            // Follow what the recorder ACTUALLY did, not what was asked (#4629).
            // startRecording() can refuse (PC Audio off, unwritable directory),
            // and driving the pulse from `on` latched the button red over a
            // recording that never began.
            w->setRecordOn(m_qsoRecorder->isRecording());
        } else {
            if (auto* sl = m_radioModel.slice(sliceId))
                sl->setRecordOn(on);
        }
    });
    // Client-side recording stopped by idle timeout → update VFO button
    connect(m_qsoRecorder, &QsoRecorder::recordingStopped, w, [w]() {
        w->setRecordOn(false);
        w->setPlayEnabled(true);
    });
    // Client-side playback
    connect(w, &VfoWidget::playToggled, this, [this, sliceId](bool on) {
        bool clientSide = AppSettings::instance().value("RecordingMode", "Client").toString() == "Client";
        if (clientSide) {
            if (on)
                m_qsoRecorder->startPlayback();
            else
                m_qsoRecorder->stopPlayback();
        } else {
            if (auto* sl = m_radioModel.slice(sliceId))
                sl->setPlayOn(on);
        }
    });
    connect(m_qsoRecorder, &QsoRecorder::playbackStopped, w, [w]() {
        w->setPlayOn(false);
    });
    connect(s, &SliceModel::recordOnChanged, w, &VfoWidget::setRecordOn);
    connect(s, &SliceModel::playOnChanged, w, &VfoWidget::setPlayOn);
    connect(s, &SliceModel::playEnabledChanged, w, &VfoWidget::setPlayEnabled);
    connect(w, &VfoWidget::autotuneRequested, this, [this, sliceId](bool intermittent) {
        if (m_radioModel.slice(sliceId))
            m_radioModel.cwAutoTune(sliceId, intermittent);
    });
    connect(w, &VfoWidget::autotuneOnceRequested, this, [this, sliceId]() {
        if (m_radioModel.slice(sliceId))
            m_radioModel.cwAutoTuneOnce(sliceId);
    });
    connect(w, &VfoWidget::zeroBeatRequested, this, [this, sliceId]() {
        // #2516: act on the slice that owns the clicked VfoWidget, NOT the
        // active slice — otherwise pressing Zero Beat on slice A while slice
        // B is active would tune B.
        SliceModel* slice = m_radioModel.slice(sliceId);
        if (!slice) return;
        // The shared CW decoder is fed by the active slice's audio and its
        // estimatedPitch() re-routes per active slice (see routeCwDecoderOutput()).
        // The detected pitch is therefore only meaningful for the active slice,
        // so refuse to apply it to a non-active slice rather than tuning on a
        // pitch that belongs to a different slice's audio.
        SliceModel* active = activeSlice();
        if (!active || active->sliceId() != sliceId) return;
        float detected = m_cwDecoder.estimatedPitch();
        if (detected <= 0.0f) return;
        int configured = m_radioModel.transmitModel().cwPitch();
        double offsetMhz = (detected - configured) / 1.0e6;
        applyTuneRequest(slice, slice->frequency() + offsetMhz,
                         TuneIntent::IncrementalTune, "zero-beat");
    });
    connect(w, &VfoWidget::addSpotRequested, this, [this](double freqMhz) {
        if (auto* sw = spectrum()) sw->showAddSpotDialog(freqMhz);
    });

    // Clicking an inactive VfoWidget activates that slice
    connect(w, &VfoWidget::sliceActivationRequested, this, [this](int id) {
        if (id != m_activeSliceId)
            setActiveSlice(id);
    });

    // SWAP — swap RX and TX frequencies (keep TX/RX assignments)
    connect(w, &VfoWidget::swapRequested, this, [this]() {
        if (!m_splitActive || m_splitRxSliceId < 0 || m_splitTxSliceId < 0) return;
        auto* rx = m_radioModel.slice(m_splitRxSliceId);
        auto* tx = m_radioModel.slice(m_splitTxSliceId);
        if (!rx || !tx) return;
        double rxFreq = rx->frequency();
        double txFreq = tx->frequency();
        applyTuneRequest(rx, txFreq, TuneIntent::IncrementalTune, "split-swap-rx");
        applyTuneRequest(tx, rxFreq, TuneIntent::IncrementalTune, "split-swap-tx");
    });

    // Split toggle — per-widget, slice-aware (#328)
    connect(w, &VfoWidget::splitToggled, this, [this, sliceId]() {
        if (!m_splitActive) {
            // Entering split: this slice becomes RX, create a new TX slice
            if (m_radioModel.slices().size() >= m_radioModel.maxSlices())
                return;
            auto* rxSlice = m_radioModel.slice(sliceId);
            if (!rxSlice) return;

            // Create TX slice on the SAME pan as the RX slice
            QString panId = rxSlice->panId();
            if (panId.isEmpty())
                panId = m_panStack ? m_panStack->activePanId() : m_radioModel.panId();

            // CW split: offset 1 kHz up (convention). Other modes: 5 kHz up.
            const QString mode = rxSlice->mode();
            bool isCw = isCwMode(mode);
            double offsetMhz = isCw ? 0.001 : 0.005;
            double txFreq = rxSlice->frequency() + offsetMhz;

            m_splitActive = true;
            m_splitRxSliceId = sliceId;
            m_radioModel.sendCommand(
                QString("slice create pan=%1 freq=%2")
                    .arg(panId).arg(txFreq, 0, 'f', 6));
        } else if (sliceId == m_splitRxSliceId) {
            // Clicking SPLIT on the RX VFO again → disable split, destroy TX slice
            disableSplit();
        }
    });

    // Client-side DSP toggles (NR2 / NR4 / MNR / BNR / DFNR / RN2) used
    // to live on the VFO DSP grid; they were removed from there in
    // favour of the spectrum overlay menu and the AetherDSP applet, so
    // the per-VFO connect block that handled them is gone.

#ifdef HAVE_RADE
    connect(w, &VfoWidget::radeActivated, this, [this](bool on, int sliceId) {
        if (on) activateRADE(sliceId);
        else if (sliceId == m_radeSliceId) deactivateRADE();
    });
#endif

    // WFM is toggled from the spectrum overlay DAX menu (wired in the per-pan
    // setup beside daxIqChannelChanged), not the flag — no connect here. (#3853)

    // AetherDSP button on the per-slice DSP tab — toggles the modeless
    // m_dspDialog (press to open, press again to close) so it matches its
    // sibling AetherVoice button instead of being a one-way launcher (#3877).
    // The Settings menu action and the RX chain double-click keep pure open
    // semantics by calling ensureAetherDspDialog() directly.
    connect(w, &VfoWidget::aetherDspRequested, this, [this] {
        toggleAetherDspDialog();
    });

    // Accent the ADSP launcher whenever any client-side NR module is active, so
    // the reporter's gap — "enable NR4 and nothing on the main surface shows it"
    // — is closed without opening the applet (#3800). The client modules are
    // global AudioEngine state, so every slice's ADSP button tracks the same OR
    // of the six *Enabled() flags. Bound to w so it drops when the slice closes.
    if (m_audio) {
        auto syncAetherDsp = [this, w] {
            if (!m_audio) return;
            const bool active = m_audio->nr2Enabled() || m_audio->nr4Enabled()
                             || m_audio->mnrEnabled()
                             || m_audio->dfnrEnabled() || m_audio->rn2Enabled()
                             || m_audio->nvAfxEnabled();
            w->setAetherDspActive(active);
        };
        connect(m_audio, &AudioEngine::nr2EnabledChanged,  w, [syncAetherDsp](bool){ syncAetherDsp(); });
        connect(m_audio, &AudioEngine::nr4EnabledChanged,  w, [syncAetherDsp](bool){ syncAetherDsp(); });
        connect(m_audio, &AudioEngine::mnrEnabledChanged,  w, [syncAetherDsp](bool){ syncAetherDsp(); });
        connect(m_audio, &AudioEngine::dfnrEnabledChanged, w, [syncAetherDsp](bool){ syncAetherDsp(); });
        connect(m_audio, &AudioEngine::rn2EnabledChanged,  w, [syncAetherDsp](bool){ syncAetherDsp(); });
        connect(m_audio, &AudioEngine::nvAfxEnabledChanged, w, [syncAetherDsp](bool){ syncAetherDsp(); });
        syncAetherDsp();  // apply current state to this freshly-wired slice
    }

    // AetherVoice button on the per-slice DSP tab — toggles the Aetherial
    // Audio Channel Strip, matching the existing menu / chain entry points.
    connect(w, &VfoWidget::aetherVoiceRequested,
            this, &MainWindow::toggleAetherialStrip);

    // Per-slice VFO marker style (#1526): push user's saved line thickness and
    // filter-edge visibility preferences to this slice's overlay whenever they
    // change. Also fires once on setSlice() below to apply loaded defaults.
    connect(w, &VfoWidget::markerStyleChanged, this,
            [this, sliceId](int markerWidth, bool hideEdges) {
        auto* sl = m_radioModel.slice(sliceId);
        if (!sl) return;
        if (auto* sw = spectrumForSlice(sl))
            sw->setSliceOverlayMarkerStyle(sliceId, markerWidth, hideEdges);
    });

    // Adaptive RX filter (RFC #3878): show/hide this slice's floor-level edge
    // markers on its panadapter overlay as the feature is toggled.
    connect(s, &SliceModel::adaptiveFilterEnabledChanged, this,
            [this, sliceId](bool on) {
        auto* sl = m_radioModel.slice(sliceId);
        if (!sl) return;
        if (auto* sw = spectrumForSlice(sl))
            sw->setSliceOverlayAdaptive(sliceId, on);
    });
    // ...and the green/red status ball follows the live-fit (active) state.
    connect(s, &SliceModel::adaptiveActiveChanged, this,
            [this, sliceId](bool active) {
        auto* sl = m_radioModel.slice(sliceId);
        if (!sl) return;
        if (auto* sw = spectrumForSlice(sl))
            sw->setSliceOverlayAdaptiveActive(sliceId, active);
    });

    // Pan re-apply after NR mono-mix (#1460, #1796): keep AudioEngine in sync
    // with the active Flex-backed slice's radio-side pan value. Kiwi-backed
    // slices update their own external source pan via
    // updateKiwiSdrVirtualAudioControlsForSlice().
    connect(s, &SliceModel::audioPanChanged, this, [this](int) {
        syncFlexRxPanToAudioEngine();
    });
    connect(s, &SliceModel::rxAntennaChanged, this, [this, sliceId](const QString&) {
        if (auto* sl = m_radioModel.slice(sliceId)) {
            if (auto* sw = spectrumForSlice(sl)) {
                sw->reacquireNoiseFloorLock();
            }
        }
    });
    connect(w, &VfoWidget::autoSqlMarginDbChanged,
            this, [this, s](int marginDb) {
        if (m_panStack) {
            for (PanadapterApplet* applet : m_panStack->allApplets()) {
                if (!applet || !applet->spectrumWidget()) {
                    continue;
                }
                applet->spectrumWidget()->setAutoSqlMarginDb(marginDb);
            }
        } else if (SpectrumWidget* sw = spectrumForSlice(s)) {
            sw->setAutoSqlMarginDb(marginDb);
        }
        updateKiwiSdrVirtualReceiverControlsForSlice(s);
        syncActiveSliceSquelchLineToSpectrums();
    });

    // Wire slice data into widget
    w->setRadioModel(&m_radioModel);
    w->setSlice(s);
    w->setAntennaList(m_radioModel.antennaList());
    w->setTransmitModel(&m_radioModel.transmitModel());
}

// wireActiveVfoSignals removed — NR2/RN2/RADE are now wired permanently
// in wireVfoWidget() so connections survive focus switches (#227).


// ─── One-shot meter wiring (#3351 Phase 2a) ─────────────────────────────────
//
// Extracted verbatim from the constructor: MeterModel → S-Meter / Tuner /
// MTR / HLTH / TX applet routing. Runs once at construction; kept in this
// TU with the rest of the model→UI wiring.

void MainWindow::wireMeters()
{
    // ── S-Meter: MeterModel → SMeterWidget (active slice only) ─────────────
    connect(&m_radioModel.meterModel(), &MeterModel::sLevelChanged,
            this, [this](int sliceIndex, float dbm) {
        if (sliceIndex == m_activeSliceId
            && (!m_kiwiSdrManager
                || m_kiwiSdrManager
                       ->assignedProfileForSlice(sliceIndex).isEmpty())) {
            deferReceivePresentation(
                ReceivePresentationSource::Flex,
                ReceivePresentationSurface::Meter,
                [this, sliceIndex, dbm]() {
                    if (!m_appletPanel
                        || sliceIndex != m_activeSliceId
                        || (m_kiwiSdrManager
                            && !m_kiwiSdrManager
                                    ->assignedProfileForSlice(sliceIndex)
                                    .isEmpty())) {
                        return;
                    }
                    m_appletPanel->sMeterWidget()->setLevel(dbm);
#ifdef HAVE_HIDAPI
                    m_tmate2SmeterDbm = dbm;
                    updateTMate2Display();
                    updateTMate2Indicators();
#endif
                },
                QString::number(sliceIndex));
        }
    });
    // Symmetric with the amp-side guard at line ~3654 and the PGXL TCP path
    // at line ~3525: in OPERATE the amp owns the analog S-Meter, so drop the
    // exciter sample here to stop the alternating-writer pulse where exciter
    // (~100 W) and amp (~1500 W) values race into the same widget. (#2927)
    connect(&m_radioModel.meterModel(), &MeterModel::txMetersChanged,
            this, [this](float fwd, float swr, bool swrValid) {
        if (m_radioModel.amplifier().present() && m_radioModel.amplifier().operate())
            return;
        // Absent SWR is forwarded as 1.0: RadioSwrValidityFilter downstream
        // reads <1.0 WITH forward power as the radio's over-range sentinel
        // and would peg the S-meter full-scale on a matched antenna (#4536
        // review, blocker 2). 1.0 is the meter's rest position.
        m_appletPanel->setStandardRadioMeterTxValues(
            fwd, m_radioModel.meterModel().fwdPowerInstant(),
            swrValid ? swr : 1.0f);
#ifdef HAVE_HIDAPI
        m_tmate2TxWatts = fwd;
        if (m_radioModel.transmitModel().isTransmitting()) {
            updateTMate2Display();
            updateTMate2Indicators();
        }
#endif
    });
    connect(&m_radioModel.meterModel(),
            &MeterModel::directionalPowerMetersChanged,
            this, [this](float fwd, float reflected, float swr,
                         bool swrValid, bool reflectedPowerMeasured) {
        if (m_radioModel.amplifier().present()
            && m_radioModel.amplifier().operate()) {
            return;
        }
        // The cross-needle already clamps sub-1.0 values to 1.0 (its rest
        // position); absent maps there explicitly rather than by accident.
        m_appletPanel->setCrossNeedleDirectionalValues(
            fwd, reflected, swrValid ? swr : 1.0f, reflectedPowerMeasured);
    });
    connect(&m_radioModel.meterModel(), &MeterModel::micMetersChanged,
            m_appletPanel->sMeterWidget(), &SMeterWidget::setMicMeters);
    connect(&m_radioModel.transmitModel(), &TransmitModel::moxChanged,
            m_appletPanel, &AppletPanel::setMeterTransmitting);

    // ── Tuner: MeterModel TX meters → TunerApplet gauges ────────────────
    // Use TGXL-specific meters when available (disambiguated from PGXL by handle)
    connect(&m_radioModel.meterModel(), &MeterModel::tgxlMetersChanged,
            m_appletPanel->tunerApplet(), &TunerApplet::updateMeters);
    // Note: txMetersChanged NOT connected to TunerApplet — exciter power
    // would overwrite TGXL readings. TGXL meters come from TunerModel
    // via the direct TCP connection (port 9010). (#625)
    m_appletPanel->tunerApplet()->setTunerModel(&m_radioModel.tunerModel());
    m_appletPanel->tunerApplet()->setMeterModel(&m_radioModel.meterModel());

    // Show/hide TUNE button + applet based on TGXL presence
    connect(&m_radioModel.tunerModel(), &TunerModel::presenceChanged,
            m_appletPanel, &AppletPanel::setTunerVisible);
    connect(&m_radioModel.tunerModel(), &TunerModel::presenceChanged,
            this, [this](bool present) {
        m_tgxlContainer->setVisible(present);
        m_tgxlSeparator->setVisible(present);
        updateStatusBarMinimumWidth();
        // Auto-connect/disconnect direct TGXL connection for manual relay control (#469)
        if (present) {
            QString ip = m_radioModel.tunerModel().tgxlIp();
            if (!ip.isEmpty() && !m_tgxlConn.isConnected()) {
                m_tgxlConn.connectToTgxl(ip);
            }
        } else {
            m_tgxlConn.disconnect();
        }
    });
    // Apply auto-reconnect setting at startup so it's active before any connection is made
    {
        const bool ar = PeripheralSettings::autoReconnect();
        m_tgxlConn.setAutoReconnect(ar);
        m_pgxlConn.setAutoReconnect(ar);
        m_antennaGenius.setAutoReconnect(ar);
        m_acomConn.setAutoReconnect(ar);
        m_speConn.setAutoReconnect(ar);
        m_vkampConn.setAutoReconnect(ar);
    }

    // Wire TgxlConnection to TunerModel
    m_radioModel.tunerModel().setDirectConnection(&m_tgxlConn);
    // ACOM deliberately does NOT route through AmpModel — it has its own
    // dedicated AcomApplet talking straight to AcomConnection (commands and
    // telemetry alike), so AmpModel stays 100% PGXL/Flex-relay-only. Wiring
    // ACOM into AmpModel's shared presence signal previously caused the AMP
    // (PGXL) panel to appear whenever ACOM connected, since AmpModel::present()
    // couldn't distinguish "PGXL relayed" from "ACOM direct". See
    // docs/architecture/acom-600s-amplifier-design.md.
    // Also attempt connection when TGXL IP arrives (may come after presence)
    connect(&m_radioModel.tunerModel(), &TunerModel::stateChanged, this, [this]() {
        auto* tuner = &m_radioModel.tunerModel();
        if (tuner->isPresent() && !tuner->tgxlIp().isEmpty() && !m_tgxlConn.isConnected()) {
            m_tgxlConn.connectToTgxl(tuner->tgxlIp());
        }
    });

    // Auto-connect to PGXL when detected
    connect(&m_radioModel.amplifier(), &AmpModel::presenceChanged, this, [this](bool present) {
        if (present && !m_radioModel.amplifier().ip().isEmpty() && !m_pgxlConn.isConnected()) {
            m_pgxlConn.connectToPgxl(m_radioModel.amplifier().ip());
        } else if (!present) {
            m_pgxlConn.disconnect();
        }
    });
    // PGXL status → AmpApplet (direct telemetry: vac, vdd, id, temp, tempb, state, etc.)
    connect(&m_pgxlConn, &PgxlConnection::statusUpdated, this, [this](const QMap<QString, QString>& kvs) {
        qCDebug(lcTuner) << "PGXL status:" << kvs;
        auto* amp = m_appletPanel->ampApplet();
        if (kvs.contains("temp")) {
            // Some PGXL firmware encodes both PA module temps as "A/B" in a
            // single field (e.g. "30.5/26.5"); others use separate tempb key.
            const QString tv = kvs["temp"];
            const int slash = tv.indexOf('/');
            if (slash >= 0) {
                amp->setTemp(tv.left(slash).toFloat());
                amp->setTempB(tv.mid(slash + 1).toFloat());
            } else {
                amp->setTemp(tv.toFloat());
            }
        }
        // Separate tempb field (firmware variant)
        if (kvs.contains("tempb"))
            amp->setTempB(kvs["tempb"].toFloat());
        if (kvs.contains("id"))
            amp->setDrainCurrent(kvs["id"].toFloat());
        if (kvs.contains("vdd"))
            amp->setDrainVoltage(kvs["vdd"].toFloat());
        if (kvs.contains("vac"))
            amp->setMainsVoltage(kvs["vac"].toInt());
        if (kvs.contains("state"))
            amp->setState(kvs["state"]);
        if (kvs.contains("fanmode"))
            amp->setFanMode(kvs["fanmode"]);
        if (kvs.contains("meffa"))
            amp->setMeff(kvs["meffa"]);
        // Convert PGXL dBm to watts and feed S-Meter alongside radio meters.
        // Use peakfwd (actual peak power) not fwd (floor/minimum).
        // Skip when amp is STANDBY — peakfwd reads ~0 dBm in standby and would
        // stomp on the exciter feed that should drive the barefoot scale.
        if (kvs.contains("peakfwd") && m_radioModel.amplifier().present()
                && m_radioModel.amplifier().operate()) {
            float dbm = kvs["peakfwd"].toFloat();
            float watts = std::pow(10.0f, (dbm - 30.0f) / 10.0f);
            qCDebug(lcTuner) << "PGXL→SMeter: peakfwd=" << dbm << "dBm =" << watts << "W";
            float swr = 1.0f;
            if (kvs.contains("swr")) {
                float rl = std::abs(kvs["swr"].toFloat());
                float rho = std::pow(10.0f, -rl / 20.0f);
                swr = (rho < 0.999f) ? (1.0f + rho) / (1.0f - rho) : 99.0f;
            }
            // Ensure S-Meter is in TX mode when PGXL reports transmitting
            if (kvs.value("state").startsWith("TRANSMIT"))
                m_appletPanel->setMeterTransmitting(true);
            else if (kvs.contains("state") && !kvs.value("state").startsWith("TRANSMIT"))
                m_appletPanel->setMeterTransmitting(false);
            m_appletPanel->setMeterTxValues(watts, swr);
#ifdef HAVE_HIDAPI
            m_tmate2TxWatts = watts;
            if (m_radioModel.transmitModel().isTransmitting()) {
                updateTMate2Display();
                updateTMate2Indicators();
            }
#endif
        }
    });
    connect(&m_pgxlConn, &PgxlConnection::connected, this, [this]() {
        qDebug() << "PGXL direct connection established, version:" << m_pgxlConn.version();
        m_appletPanel->ampApplet()->setDirectConnected(true);
    });
    connect(&m_pgxlConn, &PgxlConnection::disconnected, this, [this]() {
        m_appletPanel->ampApplet()->setDirectConnected(false);
    });
    // Radio amplifier status → AmpApplet telemetry (fallback path).
    // The radio proxies PGXL telemetry fields (id, vac, vdd, meffa, temp, tempb, state) in its
    // amplifier status messages, so the applet keeps updating even when the direct
    // PGXL TCP connection isn't established.  When direct TCP IS connected, that
    // path is faster and higher-precision (the radio rebroadcast may round/lag),
    // so we skip the radio fallback to avoid display jitter from two paths
    // alternately writing slightly-different values.
    connect(&m_radioModel.amplifier(), &AmpModel::telemetryUpdated,
            this, [this](const QMap<QString, QString>& kvs) {
        if (m_pgxlConn.isConnected()) return;
        auto* amp = m_appletPanel->ampApplet();
        if (kvs.contains("temp")) {
            const QString tv = kvs["temp"];
            const int slash = tv.indexOf('/');
            if (slash >= 0) {
                amp->setTemp(tv.left(slash).toFloat());
                amp->setTempB(tv.mid(slash + 1).toFloat());
            } else {
                amp->setTemp(tv.toFloat());
            }
        }
        if (kvs.contains("tempb"))
            amp->setTempB(kvs["tempb"].toFloat());
        if (kvs.contains("id"))
            amp->setDrainCurrent(kvs["id"].toFloat());
        if (kvs.contains("vdd"))
            amp->setDrainVoltage(kvs["vdd"].toFloat());
        if (kvs.contains("vac"))
            amp->setMainsVoltage(kvs["vac"].toInt());
        if (kvs.contains("state"))
            amp->setState(kvs["state"]);
        if (kvs.contains("meffa"))
            amp->setMeff(kvs["meffa"]);
    });
    // Fan mode cycle button → direct PGXL command (fan control is not in the radio API)
    connect(m_appletPanel->ampApplet(), &AmpApplet::fanModeChanged, this, [this](const QString& mode) {
        m_pgxlConn.sendCommand(QString("setup fanmode=%1").arg(mode));
    });
    // OPERATE button → PGXL standby/operate command (relayed via the radio's
    // amplifier API by AmpModel::setOperate; no-op if no amp handle). #4094.
    connect(m_appletPanel->ampApplet(), &AmpApplet::operateToggled, this, [this](bool on) {
        m_radioModel.amplifier().setOperate(on);
    });

    // ── ACOM S-series amplifier — serial or ser2net, no FlexRadio relay ─────
    // See docs/architecture/acom-600s-amplifier-design.md. All signal wiring
    // happens before the startup auto-connect trigger at the bottom of this
    // block — connectSerial() can call onTransportUp() synchronously (unlike
    // connectNetwork(), which waits on the TCP handshake), so wiring after
    // triggering auto-connect would silently miss the first connected()/
    // modelChanged() emission on every app start with a saved serial config.
    connect(&m_acomConn, &AcomConnection::connected, this, [this]() {
        auto* acom = m_appletPanel->acomApplet();
        // Source label from the live transport, not the persisted
        // ConnectionMode setting — the two can diverge (mode combo switched in
        // Radio Setup without a reconnect), which would mislabel an active
        // serial link as "NETWORK". See AcomConnection::sourceLabel().
        acom->setSource(m_acomConn.sourceLabel());
        acom->setConnected(true);
        m_appletPanel->setAcomVisible(true);
    });
    connect(&m_acomConn, &AcomConnection::disconnected, this, [this]() {
        m_appletPanel->acomApplet()->setConnected(false);
        m_appletPanel->setAcomVisible(false);
    });
    connect(m_appletPanel->acomApplet(), &AcomApplet::clearFaultClicked, this, [this]() {
        m_acomConn.clearFaults();
    });
    connect(m_appletPanel->acomApplet(), &AcomApplet::operateToggled, this, [this](bool on) {
        m_acomConn.setOperate(on);
    });
    connect(m_appletPanel->acomApplet(), &AcomApplet::offClicked, this, [this]() {
        m_acomConn.powerOff();
    });

    // Gauge ranges (and the temperature-offset used per telemetry frame below)
    // re-apply every time the effective model changes — at connect ("default"
    // = 600S, our one confirmed model), when a SystemConfig reply confirms a
    // model ("confirmed"), and whenever observed forward power outgrows the
    // current tier ("auto-scaled"). See design doc §6 for why there's no
    // model-selector dropdown driving this instead.
    connect(&m_acomConn, &AcomConnection::modelChanged, this,
            [this](const QString& model, const QString& reason) {
        const auto& spec = AetherSDR::Acom::modelSpec(model);
        auto* acom = m_appletPanel->acomApplet();
        acom->setPowerRange(spec.nominalForwardW, spec.maxForwardW);
        acom->setReflectedRange(spec.nominalReflectedW, spec.maxReflectedW);
        acom->setDiagnosticTooltip(QStringLiteral("Scaled for %1 (%2)").arg(model, reason));
    });
    // SystemConfig (0x11) is one-shot diagnostic info — firmware/serial/the
    // raw amplifierType byte — surfaced so a user with an unrecognized model
    // can report it back to the project (design doc §6's "help us confirm").
    connect(&m_acomConn, &AcomConnection::systemConfigReceived, this,
            [this](const AetherSDR::Acom::SystemConfig& cfg) {
        auto* acom = m_appletPanel->acomApplet();
        const QString modelGuess = AetherSDR::Acom::modelNameForAmplifierType(cfg.amplifierType);
        const QString identity = modelGuess.isEmpty()
            ? QStringLiteral("type %1 (unrecognized)").arg(cfg.amplifierType)
            : modelGuess;
        acom->setDiagnosticTooltip(QStringLiteral(
            "Detected: %1 · FW %2.%3 · S/N %4\nUnrecognized type? Please report it — "
            "see docs/architecture/acom-600s-amplifier-design.md")
            .arg(identity)
            .arg(cfg.fwVersion)
            .arg(cfg.fwSubVersion)
            .arg(cfg.serialNumberHex));
    });

    connect(&m_acomConn, &AcomConnection::telemetryUpdated, this,
            [this](const AetherSDR::Acom::Telemetry& t) {
        auto* acom = m_appletPanel->acomApplet();
        const QString effectiveModel = m_acomConn.currentModel();
        const auto& spec = AetherSDR::Acom::modelSpec(effectiveModel);

        acom->setForwardPower(static_cast<float>(t.forwardPowerW));
        acom->setReflectedPower(static_cast<float>(t.reflectedPowerW));
        acom->setSwr(t.swr_x100 / 100.0f);
        acom->setDrainCurrent(t.id1_mA / 1000.0f);
        acom->setDrainVoltage(t.hv1_x10V / 10.0f);
        acom->setTemp(static_cast<float>(t.paTempRaw) - static_cast<float>(spec.temperatureOffset));
        acom->setBand(AetherSDR::Acom::bandName(t.activeBand));
        acom->setUptime(t.systemClockSec);
        acom->setMode(t.mode);
        acom->setFaultText(t.errorCode == 0xFF
            ? QString()
            : AetherSDR::Acom::errorCodeName(t.errorCode));
        acom->setClearFaultEnabled(t.errorCode != 0xFF);
    });

    // Startup auto-connect from saved Peripherals settings — deliberately
    // last in this block (see the comment above). Unlike PGXL/TGXL (which
    // auto-connect when the *radio* reports their IP), the ACOM has no
    // radio-side presence signal at all — the saved setting is the only
    // trigger there will ever be.
    {
        const QString mode = PeripheralSettings::deviceString("Acom", "ConnectionMode",
#ifdef HAVE_SERIALPORT
            "Serial"
#else
            "Network"
#endif
        );
        if (mode == "Network") {
            const QString ip = PeripheralSettings::deviceString("Acom", "ManualIp");
            const int port = PeripheralSettings::deviceInt("Acom", "ManualPort", 7000);
            if (!ip.isEmpty())
                m_acomConn.connectNetwork(ip, static_cast<quint16>(port));
        }
#ifdef HAVE_SERIALPORT
        else {
            const QString port = PeripheralSettings::deviceString("Acom", "SerialPort");
            if (!port.isEmpty())
                m_acomConn.connectSerial(port);
        }
#endif
    }

    // ── SPE Expert amplifier — serial or ser2net, no FlexRadio relay ────────
    // See docs/architecture/spe-expert-amplifier-design.md. Same structure as
    // the ACOM block above, and the same ordering rule: all signal wiring
    // happens before the startup auto-connect trigger at the bottom, because
    // connectSerial() can call onTransportUp() synchronously.
    connect(&m_speConn, &SpeConnection::connected, this, [this]() {
        auto* spe = m_appletPanel->speApplet();
        // Source label from the live transport, not the persisted
        // ConnectionMode setting — same divergence rationale as ACOM.
        spe->setSource(m_speConn.sourceLabel());
        spe->setConnected(true);
        m_appletPanel->setSpeVisible(true);
    });
    connect(&m_speConn, &SpeConnection::disconnected, this, [this]() {
        m_appletPanel->speApplet()->setConnected(false);
        m_appletPanel->setSpeVisible(false);
    });
    // Poll-silence tracking: with ser2net the TCP link outlives the amp being
    // switched off, so respondingChanged — not disconnected — is what greys
    // the panel out in that topology.
    connect(&m_speConn, &SpeConnection::respondingChanged, this, [this](bool responding) {
        m_appletPanel->speApplet()->setResponding(responding);
    });

    // Model-dependent layout follows the Status ID field — the SPE identifies
    // itself in every status reply, so this fires once on the first poll reply
    // (no auto-ranging heuristics; contrast the ACOM block). The gauge range
    // is NOT set here: modelChanged and statusUpdated are emitted from the same
    // frame, and the statusUpdated handler below derives the level-dependent
    // scale that supersedes the model's HIGH scale anyway.
    connect(&m_speConn, &SpeConnection::modelChanged, this, [this](const QString& modelId) {
        const auto& spec = AetherSDR::Spe::modelSpec(modelId);
        m_appletPanel->speApplet()->setModelName(spec.displayName);
    });

    connect(&m_speConn, &SpeConnection::statusUpdated, this,
            [this](const AetherSDR::Spe::Status& s) {
        auto* spe = m_appletPanel->speApplet();
        const auto& spec = AetherSDR::Spe::modelSpec(s.id);

        // The power gauge rescales with the selected LOW/MID/HIGH level,
        // exactly like the amplifier's own bar display. The whole axis comes
        // from the protocol layer's model table (Spe::levelGaugeRange) rather
        // than being re-derived here, so a corrected model row moves the bar.
        // setPowerRange no-ops when the level hasn't changed.
        const auto range = AetherSDR::Spe::levelGaugeRange(spec, s.powerLevel);
        spe->setPowerRange(range.nominalW, range.warnW, range.maxW);

        spe->setForwardPower(s.outputPowerW);
        spe->setSwrAnt(s.swrAnt);
        spe->setSwrAtu(s.swrAtu);
        spe->setSupplyVoltage(s.paVoltageV);
        spe->setSupplyCurrent(s.paCurrentA);
        spe->setTemps(s.tempUpper, s.tempLower, s.tempCombiner, spec.hasCombiner);
        spe->setBand(AetherSDR::Spe::bandName(s.bandIndex));
        spe->setAntenna(s.txAntenna, s.atuState);
        spe->setInputPort(s.input);
        spe->setPowerLevel(AetherSDR::Spe::powerLevelName(s.powerLevel));
        spe->setMode(s.operate, s.transmitting);

        // One banner for both severity tiers, alarms first.
        const QString alarm = AetherSDR::Spe::alarmText(s.alarm);
        const QString warning = AetherSDR::Spe::warningText(s.warning);
        QString fault;
        if (!alarm.isEmpty())
            fault = QStringLiteral("ALARM: %1").arg(alarm);
        if (!warning.isEmpty())
            fault += (fault.isEmpty() ? QString() : QStringLiteral("  ·  "))
                + QStringLiteral("Warning: %1").arg(warning);
        spe->setFaultText(fault);
    });

    // Applet buttons -> front-panel keystrokes.
    {
        auto* spe = m_appletPanel->speApplet();
        // ON is a hardware pulse on the serial control lines, not a
        // keystroke — works while the amp is off/silent (design note §4).
        connect(spe, &SpeApplet::powerOnClicked, this, [this]() { m_speConn.powerOn(); });
        connect(spe, &SpeApplet::operateClicked, this, [this]() { m_speConn.toggleOperate(); });
        connect(spe, &SpeApplet::powerLevelClicked, this, [this]() { m_speConn.cyclePowerLevel(); });
        connect(spe, &SpeApplet::tuneClicked, this, [this]() { m_speConn.tune(); });
        connect(spe, &SpeApplet::offClicked, this, [this]() { m_speConn.switchOff(); });
        connect(spe, &SpeApplet::inputClicked, this, [this]() {
            m_speConn.sendKey(AetherSDR::Spe::Key::Input);
        });
        connect(spe, &SpeApplet::antennaClicked, this, [this]() {
            m_speConn.sendKey(AetherSDR::Spe::Key::Antenna);
        });
        // ▲/▼ are the Expert's front-panel arrow keys — they adjust the
        // drive power the amp requests from the radio over CAT.
        connect(spe, &SpeApplet::driveDownClicked, this, [this]() {
            m_speConn.sendKey(AetherSDR::Spe::Key::LeftArrow);
        });
        connect(spe, &SpeApplet::driveUpClicked, this, [this]() {
            m_speConn.sendKey(AetherSDR::Spe::Key::RightArrow);
        });
    }

    // Startup auto-connect from saved Peripherals settings — deliberately
    // last in this block (see the ordering comment above). Like the ACOM,
    // the SPE has no radio-side presence signal; the saved setting is the
    // only trigger there will ever be.
    {
        const QString mode = PeripheralSettings::deviceString("SpeExpert", "ConnectionMode",
#ifdef HAVE_SERIALPORT
            "Serial"
#else
            "Network"
#endif
        );
        if (mode == "Network") {
            const QString ip = PeripheralSettings::deviceString("SpeExpert", "ManualIp");
            const int port = PeripheralSettings::deviceInt("SpeExpert", "ManualPort", 7000);
            if (!ip.isEmpty())
                m_speConn.connectNetwork(ip, static_cast<quint16>(port));
        }
#ifdef HAVE_SERIALPORT
        else {
            const QString port = PeripheralSettings::deviceString("SpeExpert", "SerialPort");
            if (!port.isEmpty())
                m_speConn.connectSerial(port);
        }
#endif
    }

    // ── VK3AMP amplifier — TCP control/status + UDP telemetry, no FlexRadio
    // relay ─────────────────────────────────────────────────────────────────
    // See docs/architecture/vkamp-amplifier-design.md. Deliberately does NOT
    // route through AmpModel, same reasoning as ACOM above (§2 of the ACOM
    // design doc, applied here from the start): a shared AmpModel/AmpApplet
    // would make the PGXL panel appear whenever an unrelated peripheral
    // connects.
    connect(&m_vkampConn, &VkampConnection::connected, this, [this]() {
        m_appletPanel->vkampApplet()->setConnected(true);
        m_appletPanel->setVkampVisible(true);
    });
    connect(&m_vkampConn, &VkampConnection::disconnected, this, [this]() {
        m_appletPanel->vkampApplet()->setConnected(false);
        m_appletPanel->setVkampVisible(false);
    });
    connect(m_appletPanel->vkampApplet(), &VkampApplet::bypassToggled, this, [this](bool on) {
        m_vkampConn.setBypass(on);
    });
    connect(m_appletPanel->vkampApplet(), &VkampApplet::coolingToggled, this, [this](bool on) {
        m_vkampConn.setCoolingOverride(on);
    });
    connect(m_appletPanel->vkampApplet(), &VkampApplet::antennaSelected, this, [this](int port) {
        m_vkampConn.selectAntenna(port);
    });
    connect(m_appletPanel->vkampApplet(), &VkampApplet::voltageSelected, this, [this](bool low) {
        m_vkampConn.setVoltage(low);
    });
    connect(m_appletPanel->vkampApplet(), &VkampApplet::resetRequested, this, [this]() {
        m_vkampConn.startReset();
    });
    connect(&m_vkampConn, &VkampConnection::resetProgress, this, [this](double remaining) {
        m_appletPanel->vkampApplet()->setResetProgress(remaining, remaining > 0.0);
    });
    connect(&m_vkampConn, &VkampConnection::resetFinished, this, [this]() {
        m_appletPanel->vkampApplet()->setResetProgress(0.0, false);
    });

    connect(&m_vkampConn, &VkampConnection::statusUpdated, this,
            [this](const AetherSDR::Vkamp::Status& s) {
        auto* vkamp = m_appletPanel->vkampApplet();
        vkamp->setTemp(static_cast<float>(s.temp_c));
        vkamp->setSupplyVoltage(s.volts);
        vkamp->setBand(AetherSDR::Vkamp::bandName(s.band));
        vkamp->setAntenna(s.antenna);
        vkamp->setBypass(s.bypass);
        vkamp->setCoolingOverride(s.cooling_override);
        // Inferred from the live volts reading, gated on !bypass at the
        // source (Status::voltageLow()) — see design doc Section 5's
        // real-hardware safety finding on why bypass must never be read as
        // a rail selection.
        vkamp->setVoltageLow(s.bypass ? false : s.voltageLow());
        vkamp->setFaultCode(s.error_code);
    });
    connect(&m_vkampConn, &VkampConnection::telemetryUpdated, this,
            [this](const AetherSDR::Vkamp::Telemetry& t) {
        auto* vkamp = m_appletPanel->vkampApplet();
        vkamp->setForwardPower(t.outputWatts());
        vkamp->setReflectedPower(t.reflectedWatts());
        vkamp->setSwr(t.swr());
        vkamp->setCurrent(t.currentAmps());
    });
    // Telemetry is TX-gated (design doc Section 3.2) -- the amp goes silent
    // the moment a transmission ends, so without this the last transmit
    // frame stays on the power/SWR/current readouts for the whole idle
    // period, showing a keyed amplifier that isn't transmitting. Status
    // fields keep flowing on TCP and are deliberately left alone.
    connect(&m_vkampConn, &VkampConnection::telemetryStalled, this, [this]() {
        m_appletPanel->vkampApplet()->clearTelemetry();
    });

    // Startup auto-connect from saved Peripherals settings — same reasoning
    // as ACOM above: no radio-side presence signal exists for this
    // peripheral, so the saved setting is the only trigger there will ever
    // be.
    {
        const QString ip = PeripheralSettings::deviceString("Vkamp", "ManualIp");
        const int port = PeripheralSettings::deviceInt("Vkamp", "ManualPort", 5005);
        if (!ip.isEmpty())
            m_vkampConn.connectNetwork(ip, static_cast<quint16>(port));
    }
    // Forward-power gauge scale for whichever hardware variant (600W/1000W/
    // 2000W) was last selected in Peripherals settings -- see
    // RadioSetupDialog::vkampVariantChanged for the live-update path.
    {
        const int savedVariant = PeripheralSettings::deviceInt(
            "Vkamp", "Variant", static_cast<int>(AetherSDR::Vkamp::Variant::W2000));
        m_appletPanel->vkampApplet()->setVariant(static_cast<AetherSDR::Vkamp::Variant>(savedVariant));
    }

    // Switch Fwd Power gauge scale based on radio max power and amplifier presence.
    // All three power gauges (TxApplet, TunerApplet, SMeterWidget) update together.
    // When the PGXL is in STANDBY we fall back to the barefoot scale — only the
    // radio's forward power is reaching the meter, so the 2kW arc would make
    // every reading look tiny and useless.
    // Redundant-apply skipping (#4845 review) lives in each gauge's own
    // setPowerScale(), not here. It is worth having because infoChanged is
    // emitted from ordinary radio status parsing (headphone/lineout gain,
    // TNF, filter sharpness) and not just the connect-time "info" reply, so
    // a slider drag on the radio bursts it and every burst would otherwise
    // re-push an unchanged scale. It has to sit in the widget because a
    // widget is not always the sole writer of its own scale:
    // CrossNeedleMeterApplet's "Test 100 W / 4 W reflected" automation
    // action calls setPowerScale(200, false) straight at the widget, and a
    // cache out here would then skip the real re-apply that has to
    // overwrite it.
    auto updatePowerScale = [this]() {
        int maxW = m_radioModel.transmitModel().maxPowerLevel();
        // Aurora (AU-) radios have an integrated 600W PA (Overlord) but
        // max_power_level only reports the exciter limit (100W). Use model
        // name to detect the true PA capability. (#484)
        const QString& model = m_radioModel.model();
        if (model.startsWith("AU-") && maxW <= 100) {
            maxW = 500;
        }
        const bool ampActive = m_radioModel.amplifier().present()
                            && m_radioModel.amplifier().operate();
        m_appletPanel->txApplet()->setPowerScale(maxW, ampActive);
        m_appletPanel->tunerApplet()->setPowerScale(maxW, ampActive);
        m_appletPanel->setMeterPowerScale(maxW, ampActive);
        m_appletPanel->healthApplet()->setPowerScale(maxW, ampActive);
    };
    connect(&m_radioModel.amplifier(), &AmpModel::presenceChanged, this, updatePowerScale);
    connect(&m_radioModel.amplifier(), &AmpModel::stateChanged, this, updatePowerScale);
    // Also refresh on infoChanged (#4813): maxPowerLevelChanged only fires when
    // the numeric value actually changes, which it doesn't on connect if the
    // radio's exciter limit matches TransmitModel's compiled-in default (100W —
    // exactly the AU- case above). infoChanged fires once "info" comes back and
    // model() is populated, so this is what actually catches the initial scale
    // on connect instead of leaving it wrong until the next real power-level
    // edge (e.g. a band change that happens to carry a different limit).
    // infoChanged rather than connectionStateChanged: model() does happen to
    // be seeded from the discovery packet before that one fires on a LAN
    // connect (RadioModel.cpp, connectToRadio()), but infoChanged is the
    // signal actually tied to the info data this branch reads, so it doesn't
    // depend on that seeding holding for every connect path.
    connect(&m_radioModel, &RadioModel::infoChanged, this, updatePowerScale);

    // TGXL indicator: two-line rich text — label on top, state smaller below.
    // Green = OPERATE, amber = BYPASS, grey = STANDBY (matches SmartSDR)
    auto setIndicatorHtml = [](QLabel* nameLbl, QLabel* stateLbl,
                               const QString& state, const QString& color) {
        nameLbl->setStyleSheet(
            QString("QLabel { color: %1; font-size:18px; font-weight:bold; }").arg(color));
        stateLbl->setStyleSheet(
            QString("QLabel { color: %1; font-size:11px; }").arg(color));
        stateLbl->setText(state);
    };

    auto updateTgxlStyle = [this, setIndicatorHtml]() {
        auto& t = m_radioModel.tunerModel();
        if (t.isOperate() && !t.isBypass())
            setIndicatorHtml(m_tgxlIndicator, m_tgxlStateLabel, "OPERATE", "#00e060");
        else if (t.isOperate() && t.isBypass())
            setIndicatorHtml(m_tgxlIndicator, m_tgxlStateLabel, "BYPASS", "#e0a000");
        else
            setIndicatorHtml(m_tgxlIndicator, m_tgxlStateLabel, "STANDBY", "#404858");
    };
    connect(&m_radioModel.tunerModel(), &TunerModel::stateChanged, this, updateTgxlStyle);

    // PGXL indicator: OPERATE (green) or STANDBY (grey) — no bypass for PGXL
    auto updatePgxlStyle = [this, setIndicatorHtml]() {
        if (m_radioModel.amplifier().operate())
            setIndicatorHtml(m_pgxlIndicator, m_pgxlStateLabel, "OPERATE", "#00e060");
        else
            setIndicatorHtml(m_pgxlIndicator, m_pgxlStateLabel, "STANDBY", "#404858");
    };
    connect(&m_radioModel.amplifier(), &AmpModel::stateChanged, this, [this, updatePgxlStyle]() {
        updatePgxlStyle();
        // Sync the AmpApplet button — the direct PGXL TCP path may not deliver
        // a state update fast enough, leaving the button stuck on the old state.
        // RadioModel is authoritative; use it to keep the button consistent.
        m_appletPanel->ampApplet()->setState(
            m_radioModel.amplifier().operate() ? QStringLiteral("OPERATE") : QStringLiteral("STANDBY"));
    });

    connect(&m_radioModel.amplifier(), &AmpModel::presenceChanged, this, [this, updatePgxlStyle](bool present) {
        m_pgxlContainer->setVisible(present);
        m_pgxlSeparator->setVisible(present);
        m_appletPanel->setAmpVisible(present);
        updateStatusBarMinimumWidth();
        if (present) {
            updatePgxlStyle();
            m_appletPanel->ampApplet()->setState(
                m_radioModel.amplifier().operate() ? QStringLiteral("OPERATE") : QStringLiteral("STANDBY"));
        }
    });
    connect(&m_radioModel.meterModel(), &MeterModel::ampMetersChanged,
            this, [this](float fwdPwr, float swr, float temp) {
        m_appletPanel->ampApplet()->setFwdPower(fwdPwr);
        m_appletPanel->ampApplet()->setSwr(swr);
        m_appletPanel->ampApplet()->setTemp(temp);
        // S-Meter TX power follows the scale: amp output when PGXL is OPERATE,
        // exciter output when it's STANDBY (txMetersChanged already handles that
        // path, so we just stop overriding it here).
        if (m_radioModel.amplifier().present() && m_radioModel.amplifier().operate()) {
            m_appletPanel->setMeterTxValues(fwdPwr, swr);
#ifdef HAVE_HIDAPI
            m_tmate2TxWatts = fwdPwr;
            if (m_radioModel.transmitModel().isTransmitting()) {
                updateTMate2Display();
                updateTMate2Indicators();
            }
#endif
            static int ampDbg = 0;
            if (++ampDbg % 50 == 1)
                qCDebug(lcTuner) << "AMP→SMeter: fwd=" << fwdPwr << "W swr=" << swr;
        }
    });
    connect(&m_radioModel.transmitModel(), &TransmitModel::maxPowerLevelChanged,
            this, updatePowerScale);

    // ── Meter applet: all meters consolidated ──────────────────────────────
    m_appletPanel->meterApplet()->setMeterModel(&m_radioModel.meterModel());

    // ── PROF applet: Global / TX / Mic profile switcher (#3376) ─────────────
    m_appletPanel->profileSwitcherApplet()->setRadioModel(&m_radioModel);

    // ── HLTH applet: same meter model — derives antenna-health state from
    //    SWR / power trends across radio / tuner / amp sources.
    m_appletPanel->healthApplet()->setMeterModel(&m_radioModel.meterModel());

    // ── TX applet: meters + model ───────────────────────────────────────────
    connect(&m_radioModel.meterModel(), &MeterModel::txMetersChanged,
            m_appletPanel->txApplet(), &TxApplet::updateMeters);
    // PEP peak-hold tick on the FWDPWR gauge: feed the raw pre-smoothed
    // sample and reset the hold on un-key. (#2561)
    connect(&m_radioModel.meterModel(), &MeterModel::txPeakChanged,
            m_appletPanel->txApplet(), &TxApplet::updatePeakPower);
    auto updateTxMeterGate = [this]() {
        const auto& tx = m_radioModel.transmitModel();
        m_appletPanel->txApplet()->setTransmitting(
            tx.isTransmitting() || tx.isTuning()
            || m_radioModel.isRadioTransmitting());
    };
    connect(&m_radioModel.transmitModel(), &TransmitModel::moxChanged,
            this, [updateTxMeterGate](bool) { updateTxMeterGate(); });
    connect(&m_radioModel.transmitModel(), &TransmitModel::tuneChanged,
            this, [updateTxMeterGate](bool) { updateTxMeterGate(); });
    connect(&m_radioModel, &RadioModel::radioTransmittingChanged,
            this, [updateTxMeterGate](bool) { updateTxMeterGate(); });
    updateTxMeterGate();
    m_appletPanel->txApplet()->setTransmitModel(&m_radioModel.transmitModel());
    m_appletPanel->txApplet()->setTunerModel(&m_radioModel.tunerModel());
    // ATU right-click → pre-tune dialog needs RadioModel for slice access
    // and BandPlanManager for region-correct band edges. (#2624)
    m_appletPanel->txApplet()->setRadioModel(&m_radioModel);
    m_appletPanel->txApplet()->setBandPlanManager(m_bandPlanMgr);
    m_appletPanel->rxApplet()->setRadioModel(&m_radioModel);
    m_appletPanel->rxApplet()->setKiwiSdrManager(m_kiwiSdrManager);
    m_appletPanel->rxApplet()->setTransmitModel(&m_radioModel.transmitModel());
    connect(m_appletPanel->rxApplet(), &RxApplet::kiwiRxAntennaSelected,
            this, &MainWindow::setKiwiSdrVirtualAntennaForSlice);
    connect(m_appletPanel->rxApplet(), &RxApplet::flexRxAntennaSelected,
            this, &MainWindow::clearKiwiSdrVirtualAntennaForSlice);

    // Hide APD row on radios that don't support it
    connect(&m_radioModel.transmitModel(), &TransmitModel::apdStateChanged, this, [this]() {
        m_appletPanel->txApplet()->setApdVisible(
            m_radioModel.transmitModel().apdConfigurable());
    });

}

// Adaptive RX filter (RFC #3878): per FFT frame, drive the fit engine for every
// SSB slice on the pan that produced the frame. The engine itself gates on
// mode/enabled and only does real work for enabled SSB slices, so this is a
// cheap pan/slice lookup otherwise. The pan span + noise floor come from the
// same source the S-History detector uses (SpectrumWidget::noiseFloorDbm()).
void MainWindow::onSpectrumReadyForAdaptiveFilter(quint32 streamId,
                                                  const QVector<float>& bins,
                                                  qint64 emittedNs)
{
    if (m_shuttingDown || !m_panStack) return;
    // Suspend while transmitting: the panadapter shows the TX signal (or muted
    // RX), so fitting against it would chase garbage. Returning early holds the
    // current passband and per-slice state untouched, so the fit resumes cleanly
    // on unkey instead of re-fitting from scratch.
    if (m_radioModel.isRadioTransmitting()) return;
    if (!m_adaptiveFilterEngine)
        m_adaptiveFilterEngine = new AdaptiveFilterEngine(this);

    for (auto* pan : m_radioModel.panadapters()) {
        if (!pan || pan->panStreamId() != streamId) continue;

        SpectrumWidget* sw = m_panStack->spectrum(pan->panId());
        const float noiseFloor = sw ? sw->noiseFloorDbm() : -1000.0f;

        for (auto* slice : m_radioModel.slices()) {
            if (!slice || slice->panId() != pan->panId()) continue;
            m_adaptiveFilterEngine->processFrame(
                slice, pan->centerMhz(), pan->bandwidthMhz(), bins, noiseFloor,
                emittedNs);
        }
        break;  // one pan owns this stream id
    }
}

} // namespace AetherSDR
