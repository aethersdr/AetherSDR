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
#include "Ax25HfPacketDecodeDialog.h"
#include "AppletPanel.h"
#include "MainWindowHelpers.h"
#include "PanadapterApplet.h"
#include "PanadapterStack.h"
#include "RadioSetupDialog.h"
#include "RxApplet.h"
#include "AmpApplet.h"
#include "HealthApplet.h"
#include "MeterApplet.h"
#include "ProfileSwitcherApplet.h"
#include "SMeterWidget.h"
#include "TunerApplet.h"
#include "TxApplet.h"
#include "core/PeripheralSettings.h"
#include "core/KiwiSdrClient.h"
#include "core/KiwiSdrManager.h"
#include "core/KiwiSdrProtocol.h"
#include "SpectrumOverlayMenu.h"
#include "SpectrumWidget.h"
#include "VfoWidget.h"
#include "core/BandStackSettings.h"
#include "core/AppSettings.h"
#include "core/SpotCommandPolicy.h"
#include "core/SpotModeResolver.h"
#include "core/LogManager.h"
#ifdef HAVE_RADE
#include "core/RADEEngine.h"
#endif
#include "models/ProfileLoadCommand.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QDateTime>
#include <QFileDialog>
#include <QSet>
#include <QTimer>

#include <algorithm>
#include <memory>
#include <cmath>

namespace AetherSDR {

namespace {

constexpr double kIncrementalTriggerEdgeMarginFrac = 0.05;
constexpr double kIncrementalSettleEdgeMarginFrac = 0.06;
constexpr double kRevealComfortEdgeMarginFrac = 0.18;
constexpr double kSpectrumClickEdgeMarginFrac = 0.05;

constexpr double kMemoryRevealTargetToleranceMhz = 0.000001;
constexpr int kPanDimensionDecoderFallbackMs = 650;
constexpr qint64 kProfileLoadDimensionSettleMs = kPanDimensionDecoderFallbackMs + 100;

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

const SliceModel* kiwiSliceForPan(const RadioModel& radioModel,
                                  const QString& panId,
                                  int activeSliceId)
{
    if (panId.isEmpty()) {
        return nullptr;
    }

    for (SliceModel* slice : radioModel.slices()) {
        if (!slice || slice->panId() != panId
            || !radioModel.sliceMayBelongToUs(slice->sliceId())) {
            continue;
        }

        if (slice->sliceId() == activeSliceId) {
            return slice;
        }
    }
    return nullptr;
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

    // NR2
    connect(w, &AetherDspWidget::nr2GainMaxChanged, this, [this](float v) {
        QMetaObject::invokeMethod(m_audio, [this, v]() { m_audio->setNr2GainMax(v); });
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
    connect(w, &AetherDspWidget::dfnrAttenLimitChanged, this, [this](float v) {
        QMetaObject::invokeMethod(m_audio, [this, v]() { m_audio->setDfnrAttenLimit(v); });
    });
    connect(w, &AetherDspWidget::dfnrPostFilterBetaChanged, this, [this](float v) {
        QMetaObject::invokeMethod(m_audio, [this, v]() { m_audio->setDfnrPostFilterBeta(v); });
    });
    // MNR
    connect(w, &AetherDspWidget::mnrEnabledChanged, this, [this](bool on) {
        QMetaObject::invokeMethod(m_audio, [this, on]() { m_audio->setMnrEnabled(on); });
    });
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


void MainWindow::onSliceAdded(SliceModel* s)
{
    // During layout transition, spectrums are being destroyed/recreated — skip
    if (m_applyingLayout) return;

    qDebug() << "MainWindow: slice added" << s->sliceId();
    const bool firstSlice = (m_activeSliceId < 0);

    // First slice — wire everything up
    if (firstSlice) {
        setActiveSlice(s->sliceId());

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
            if (settings.value("ClientNr2Enabled", "False").toString() == "True")
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
    {
        const int sliceIdx = m_radioModel.slices().indexOf(s);
        if (sliceIdx >= 0) {
            const QString key = QString("DaxChannel_Slice%1").arg(QChar('A' + sliceIdx));
            int savedDax = AppSettings::instance().value(key, "0").toInt();
            if (savedDax > 0) {
                // QPointer guards against a dangling slice: a raw SliceModel*
                // stays non-null after the slice is destroyed, so the `s &&`
                // check below is only meaningful if the capture auto-nulls.
                // Removing the slice within this 300 ms window (rapid
                // add/remove) would otherwise dereference freed memory. (#3646)
                QPointer<SliceModel> sp(s);
                QTimer::singleShot(300, this, [this, sp, savedDax]() {
                    if (sp && !profileLoadRadioStateWritesHeld()) {
                        sp->setDaxChannel(savedDax);
                    }
                });
            }
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
    });

    // Connect slice state changes → spectrum overlay updates
    connect(s, &SliceModel::frequencyChanged, this, [this, s](double mhz) {
        // Don't snap overlay back to stale radio-confirmed freq during active
        // encoder tuning — the optimistic VFO position is already ahead (#1524)
        bool activeTuning = false;
#ifdef HAVE_SERIALPORT
        activeTuning = activeTuning || m_flexCoalesceTimer.isActive();
#endif
        // HID encoder frequency tuning routes through applyFlexControlWheelAction,
        // so m_flexCoalesceTimer above already covers it.
        const bool memoryRevealPending = (m_pendingMemoryRevealSliceId == s->sliceId());
        if (activeTuning && s->sliceId() == m_activeSliceId && !memoryRevealPending)
            return;

        m_updatingFromModel = true;
        if (auto* sw = spectrumForSlice(s))
            sw->setSliceOverlay(s->sliceId(), mhz,
                s->filterLow(), s->filterHigh(), s->isTxSlice(),
                s->sliceId() == m_activeSliceId,
                s->mode(), s->rttyMark(), s->rttyShift(),
                s->ritOn(), s->ritFreq(), s->xitOn(), s->xitFreq());
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
            s->ritOn(), s->ritFreq(), s->xitOn(), s->xitFreq());
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
                s->ritOn(), s->ritFreq(), s->xitOn(), s->xitFreq());
        syncTxWaterfallSliceToSpectrums();
        updateSplitState();

        // Active follows TX slice (#1351) — switch the displayed/active slice
        // when an external program (e.g. WSJT-X) moves the TX flag
        if (tx && s->sliceId() != m_activeSliceId
            && AppSettings::instance().value("ActiveFollowsTxSlice", "False").toString() == "True") {
            setActiveSlice(s->sliceId());
        }
    });

    // When the radio notifies us that this slice became active, switch to it
    connect(s, &SliceModel::activeChanged, this, [this, s](bool active) {
        if (!active) return;
        // Accept radio's active echo — update client state but use
        // m_updatingFromModel to prevent sending active=1 back (feedback loop).
        m_updatingFromModel = true;
        setActiveSlice(s->sliceId());
        m_updatingFromModel = false;
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

            // Update CWX/DVK indicator availability for new mode
            updateKeyerAvailability(mode);

            // Disable client-side DSP in digital and CW modes — NR2/RN2/BNR
            // corrupt digital data (#534) and suppress CW tones (#784)
            bool disableDsp = (mode == "DIGU" || mode == "DIGL" || mode == "RTTY"
                            || mode == "CW"   || mode == "CWL"  || mode == "NT");
            if (disableDsp) {
                if (m_audio->nr2Enabled())
                    QMetaObject::invokeMethod(m_audio, [this]() { m_audio->setNr2Enabled(false); });
                if (m_audio->rn2Enabled())
                    QMetaObject::invokeMethod(m_audio, [this]() { m_audio->setRn2Enabled(false); });
#ifdef HAVE_BNR
                if (m_audio->bnrEnabled())
                    QMetaObject::invokeMethod(m_audio, [this]() { m_audio->setBnrEnabled(false); });
#endif
                if (m_audio->nr4Enabled())
                    QMetaObject::invokeMethod(m_audio, [this]() { m_audio->setNr4Enabled(false); });
                if (m_audio->dfnrEnabled())
                    QMetaObject::invokeMethod(m_audio, [this]() { m_audio->setDfnrEnabled(false); });
            }
        }
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
        // Remove overlay/VFO from all spectrums
        if (m_panStack) {
            for (auto* pan : m_radioModel.panadapters()) {
                if (auto* sw = m_panStack->spectrum(pan->panId())) {
                    sw->removeSliceOverlay(s->sliceId());
                    sw->removeVfoWidget(s->sliceId());
                }
            }
        }
        // Re-add on the new pan
        auto* sw = spectrumForSlice(s);
        if (!sw) return;
        auto* vfo = sw->addVfoWidget(s->sliceId());
        wireVfoWidget(vfo, s);
        pushSliceOverlay(s);
        if (s->isTxSlice())
            syncTxWaterfallSliceToSpectrums();
        updateKiwiSdrVirtualTrackingForSlice(s);
        refreshKiwiSdrWaterfallAvailability();
        syncKiwiSdrPanadapterUiStates();
        syncKiwiSdrDiversityEscControls();
    });

    // Create a VfoWidget for this slice on the correct panadapter
    auto* swForVfo = spectrumForSlice(s);
    if (!swForVfo) return;
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

    wireVfoWidget(vfo, s);

    // NR2/RN2/RADE are now wired permanently in wireVfoWidget — no
    // special handling needed here for active slice timing.

#ifdef HAVE_RADE
    // Reconnect scenario: slice may already be in FDVU/FDVL when AetherSDR connects
    if (s->mode().startsWith("FDV"))
        activateFdvDisplay(s->sliceId());
#endif

    // Show DIV button on dual-SCU radios
    {
        const QString& model = m_radioModel.model();
        bool divAllowed = model.contains("6500") || model.contains("6600")
                       || model.contains("6700") || model.contains("8600")
                       || model.contains("AU-520");
        vfo->setDiversityAllowed(divAllowed);
    }

    // Feed S-meter per-slice — only this VFO's slice level
    const int sid = s->sliceId();
    connect(&m_radioModel.meterModel(), &MeterModel::sLevelChanged,
            vfo, [this, vfo, sid](int sliceIndex, float dbm) {
        if (sliceIndex == sid
            && (!m_kiwiSdrManager
                || m_kiwiSdrManager->assignedProfileForSlice(sid).isEmpty())) {
            vfo->setSignalLevel(dbm);
        }
    });
    // Feed ESC meter per-slice — signal strength after ESC processing
    connect(&m_radioModel.meterModel(), &MeterModel::escLevelChanged,
            vfo, [vfo, sid](int sliceIndex, float dbm) {
        if (sliceIndex == sid)
            vfo->setEscLevel(dbm);
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
            vfo, [vfo](float fwd, float swr) {
        vfo->setTxPower(fwd);
        vfo->setTxSwr(swr);
    });
    connect(&m_radioModel.transmitModel(), &TransmitModel::moxChanged,
            vfo, &VfoWidget::setTransmitting);
    connect(&m_radioModel, &RadioModel::antListChanged,
            vfo, &VfoWidget::setAntennaList);

    // Refresh the split-pair visualization whenever this slice's TX role changes,
    // so an externally-initiated split (rigctld / CAT / TCI / front panel) is
    // reflected on the panadapter, not just GUI-initiated split. (#3726)
    connect(s, &SliceModel::txSliceChanged, this, [this](bool) { updateSplitState(); });

    // Reset band-stack auto-save dwell timer on every active-slice tune
    connect(s, &SliceModel::frequencyChanged, this, [this, s]() {
        if (s->sliceId() != m_activeSliceId) return;
        if (!m_bsAutoSaveTimer) return;
        if (profileLoadRadioStateWritesHeld()) return;
        const int dwellSec = BandStackSettings::instance().autoSaveDwellSeconds();
        if (dwellSec <= 0) return;
        m_bsAutoSaveTimer->start(dwellSec * 1000);
    });

    // Direct freq label update for runtime changes
    connect(s, &SliceModel::frequencyChanged, this, [this, s]() {
        auto* sw2 = spectrumForSlice(s);
        if (!sw2) return;
        auto* v = sw2->vfoWidget(s->sliceId());
        if (!v) return;
        long long hz = static_cast<long long>(std::round(s->frequency() * 1e6));
        int mhzPart = static_cast<int>(hz / 1000000);
        int khzPart = static_cast<int>((hz / 1000) % 1000);
        int hzPart  = static_cast<int>(hz % 1000);
        v->freqLabel()->setText(QString("%1.%2.%3")
            .arg(mhzPart)
            .arg(khzPart, 3, 10, QChar('0'))
            .arg(hzPart, 3, 10, QChar('0')));

        // Diversity: client-side sync — immediately update child VFO display
        // to avoid rubber-banding from the radio round-trip delay.
        // Only for diversity parent→child, NOT split RX→TX.
        if (s->isDiversityParent()) {
            for (auto* other : m_radioModel.slices()) {
                if (other->isDiversityChild() && other->sliceId() != s->sliceId()) {
                    auto* csw = spectrumForSlice(other);
                    if (!csw) continue;
                    auto* cv = csw->vfoWidget(other->sliceId());
                    if (!cv) continue;
                    cv->freqLabel()->setText(QString("%1.%2.%3")
                        .arg(mhzPart)
                        .arg(khzPart, 3, 10, QChar('0'))
                        .arg(hzPart, 3, 10, QChar('0')));
                }
            }
        }
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
    m_appletPanel->updateSliceButtons(m_radioModel.slices(), m_activeSliceId);
    refreshKiwiSdrSlices();
    refreshKiwiSdrWaterfallAvailability();
    syncKiwiSdrDiversityEscControls();

    // A slice can arrive already flagged as the TX slice (e.g. external split
    // created the TX slice out-of-band). Re-derive the split-pair from the model
    // so the panadapter pairs it without a GUI action. (#3726)
    updateSplitState();
}

void MainWindow::onSliceRemoved(int id)
{
    if (m_applyingLayout) return;

    qDebug() << "MainWindow: slice removed" << id;

    m_kiwiSdrVirtualPreviousMute.remove(id);
    if (m_kiwiSdrManager) {
        m_kiwiSdrManager->clearSliceAssignment(id);
    }

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
        if (!slices.isEmpty())
            setActiveSlice(slices.first()->sliceId());
        else {
            m_activeSliceId = -1;
            if (m_ax25HfPacketDecodeDialog)
                m_ax25HfPacketDecodeDialog->setAttachedSlice(nullptr);
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
            if (pan->panStreamId()) {
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
            m_radioModel.panStream()->setYPixels(streamId, ypix);
            swGuard->prepareForFftScaleChange();
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
        QSet<int> requestedChannels;
        bool txSliceIsDigital = false;

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
            if (channel < 1 || channel > 4) {
                continue;
            }

#ifdef HAVE_WEBSOCKETS
            const bool tciUsingChannel = tciServer() && tciServer()->ownsDaxChannel(channel);
#else
            const bool tciUsingChannel = false;
#endif
#ifdef HAVE_RADE
            const bool radeUsingChannel =
                (m_radeDaxStreamId != 0 && panStream
                 && m_radeDaxStreamId == panStream->daxStreamIdForChannel(channel));
#else
            const bool radeUsingChannel = false;
#endif
            quint32 existingStream = panStream ? panStream->daxStreamIdForChannel(channel) : 0;
            if (!tciUsingChannel && panStream && resetDaxRxStreams) {
                if (existingStream != 0 && !radeUsingChannel) {
                    m_radioModel.sendCommand(
                        QString("stream remove 0x%1").arg(existingStream, 0, 16));
                    panStream->unregisterDaxStream(existingStream);
                    existingStream = 0;
                }
            }

            if (!tciUsingChannel && !radeUsingChannel && existingStream == 0
                    && !requestedChannels.contains(channel)) {
                m_radioModel.sendCommand(
                    QString("stream create type=dax_rx dax_channel=%1").arg(channel));
                requestedChannels.insert(channel);
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

    struct PendingDbmRange {
        bool active{false};
        float minDbm{0.0f};
        float maxDbm{0.0f};
        qint64 requestedMs{0};
    };
    auto pendingDbm = std::make_shared<PendingDbmRange>();
    auto dbmMatches = [](float leftMin, float leftMax, float rightMin, float rightMax) {
        return std::abs(leftMin - rightMin) < 0.01f
            && std::abs(leftMax - rightMax) < 0.01f;
    };
    auto setStreamDbmRange = [this, applet](float minDbm, float maxDbm, bool waitForEcho = false) {
        if (auto* pan = m_radioModel.panadapter(applet->panId())) {
            if (pan->panStreamId()) {
                m_radioModel.panStream()->setDbmRange(pan->panStreamId(), minDbm, maxDbm, waitForEcho);
            }
        }
    };
    auto sendDbmRangeCommand = [this, applet](float minDbm, float maxDbm) {
        if (!dbmRangeLooksPlausible(minDbm, maxDbm)) {
            qCWarning(lcProtocol).noquote()
                << "MainWindow: rejecting implausible dBm range"
                << QStringLiteral("pan=%1").arg(applet->panId())
                << QStringLiteral("min=%1").arg(minDbm, 0, 'f', 2)
                << QStringLiteral("max=%1").arg(maxDbm, 0, 'f', 2);
            return;
        }

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
    if (m_appletPanel && m_appletPanel->rxApplet()) {
        QObject::disconnect(m_appletPanel->rxApplet(), nullptr, sw, nullptr);
    }

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
                if (profileId == displayProfileId
                    && m_kiwiSdrManager->isConnected(profileId)) {
                    sw->setKiwiSdrWaterfallAvailable(true);
                    sw->setKiwiSdrWaterfallProfile(profileId);
                    sw->setKiwiSdrWaterfallActive(true);
                    syncKiwiSdrAppletWaterfallState();
                }
            }
        }
        if (!m_kiwiSdrClient || !m_kiwiSdrClient->isConnected()
            || !applet || !sw
            || !kiwiSliceForPan(m_radioModel, applet->panId(), m_activeSliceId)) {
            return;
        }
        m_kiwiSdrClient->setWaterfallLineDurationMs(sw->wfLineDuration());
        m_kiwiSdrClient->setWaterfallView(
            applet->panId(), centerMhz, bandwidthMhz);
    };

    const auto updateKiwiWaterfallViewUnlessDeferred =
        [updateKiwiWaterfallView, sw](double centerMhz, double bandwidthMhz) {
            if (sw && sw->waterfallViewUpdateDeferred()) {
                return;
            }
            if (sw && sw->frequencyRangeGestureActive()) {
                return;
            }
            updateKiwiWaterfallView(centerMhz, bandwidthMhz);
        };

    connect(sw, &SpectrumWidget::frequencyRangeChanged,
            this, updateKiwiWaterfallViewUnlessDeferred);
    connect(sw, &SpectrumWidget::frequencyRangeChangeRequested,
            this, updateKiwiWaterfallViewUnlessDeferred);
    connect(sw, &SpectrumWidget::centerChangeRequested,
            this, [updateKiwiWaterfallView, sw](double centerMhz) {
                if (!sw) {
                    return;
                }
                if (sw->panDragActive()
                    || sw->frequencyRangeGestureActive()) {
                    return;
                }
                updateKiwiWaterfallView(centerMhz, sw->bandwidthMhz());
            });
    connect(sw, &SpectrumWidget::panDragSettled,
            this, updateKiwiWaterfallView);
    connect(sw, &SpectrumWidget::frequencyRangeSettled,
            this, updateKiwiWaterfallView);

    // Wire band plan manager to this spectrum widget
    sw->setBandPlanManager(m_bandPlanMgr);

    // Set panadapter bandwidth zoom limits based on radio model
    sw->setBandwidthLimits(m_radioModel.minPanBandwidthMhz(),
                           m_radioModel.maxPanBandwidthMhz());

    // Set panId on the overlay menu so +RX routes to the correct pan
    menu->setPanId(applet->panId());
    menu->setMemories(m_radioModel.memories());
    menu->setRadioModel(&m_radioModel);
    menu->setKiwiSdrManager(m_kiwiSdrManager);
    menu->setRadioCapabilities(m_radioModel.capabilities());

    // Antenna list → this overlay menu (per-pan, mirrors VfoWidget pattern) (#1260)
    connect(&m_radioModel, &RadioModel::antListChanged,
            menu, &SpectrumOverlayMenu::setAntennaList);
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
                sw, &SpectrumWidget::setWideActive);
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
        connect(pan, &PanadapterModel::levelChanged,
                sw, [sw, pendingDbm, dbmMatches, setStreamDbmRange](float minDbm, float maxDbm) {
            if (pendingDbm->active) {
                if (!dbmMatches(minDbm, maxDbm, pendingDbm->minDbm, pendingDbm->maxDbm)) {
                    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
                    if (pendingDbm->requestedMs > 0
                        && nowMs - pendingDbm->requestedMs > kDbmRangeHandshakeTimeoutMs) {
                        pendingDbm->active = false;
                        pendingDbm->requestedMs = 0;
                    } else {
                        setStreamDbmRange(pendingDbm->minDbm, pendingDbm->maxDbm, true);
                        return;
                    }
                } else {
                    pendingDbm->active = false;
                    pendingDbm->requestedMs = 0;
                }
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

        wirePanReconcilers(applet, pan);
    }
    syncTxWaterfallSliceToSpectrums();

    // ── Debounced resize → re-push xpixels/ypixels to the radio (#1511) ───
    // When the layout changes (e.g. adding a second pan, splitting, resizing),
    // the FFT pane dimensions change but the radio keeps encoding FFT bins to the
    // old yPixels scale.  This causes a ~5 dB noise-floor offset until restart.
    // A 300ms debounce avoids flooding the radio during animated resizes.
    {
        auto* resizeTimer = new QTimer(sw);  // parented to sw → auto-deleted
        resizeTimer->setSingleShot(true);
        resizeTimer->setInterval(300);
        connect(sw, &SpectrumWidget::dimensionsChanged,
                resizeTimer, [resizeTimer](int, int) { resizeTimer->start(); });
        connect(resizeTimer, &QTimer::timeout,
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

    // ── Tuning step size → this pan's spectrum widget ─────────────────────
    // The global connection only covers AppSettings + radio command.
    // Each pan must also receive stepSizeChanged so scroll-to-tune
    // uses the correct step regardless of which pan is active.
    connect(m_appletPanel->rxApplet(), &RxApplet::stepSizeChanged,
            sw, &SpectrumWidget::setStepSize);

    // ── Pan activation: clicking on this pan makes it active ─────────────
    connect(applet, &PanadapterApplet::activated,
            m_panStack, &PanadapterStack::setActivePan);

    // ── Close pan: X button on title bar closes this pan ────────────────
    connect(applet, &PanadapterApplet::closeRequested,
            this, [this](const QString& panId) {
        // Don't close the last pan
        if (m_panStack->count() <= 1) return;
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
        if (!kiwiSdrProfileForPan(applet->panId()).isEmpty()) {
            return;
        }
        m_radioModel.sendCommand(
            QString("display pan set %1 bandwidth=%2").arg(applet->panId()).arg(bw, 0, 'f', 6));
    });
    connect(sw, &SpectrumWidget::centerChangeRequested,
            this, [this, applet](double center) {
        if (!kiwiSdrProfileForPan(applet->panId()).isEmpty()) {
            return;
        }
        if (const auto* pan = m_radioModel.panadapter(applet->panId()))
            center = std::max(center, pan->bandwidthMhz() / 2.0);
        m_radioModel.sendCommand(
            QString("display pan set %1 center=%2").arg(applet->panId()).arg(center, 0, 'f', 6));
    });
    connect(sw, &SpectrumWidget::bandZoomRequested,
            this, [this, applet, bandZoomOn = std::make_shared<bool>(false)]() mutable {
        *bandZoomOn = !*bandZoomOn;
        m_radioModel.sendCommand(
            QString("display pan set %1 band_zoom=%2")
                .arg(applet->panId()).arg(*bandZoomOn ? 1 : 0));
    });
    connect(sw, &SpectrumWidget::segmentZoomRequested,
            this, [this, applet, segZoomOn = std::make_shared<bool>(false)]() mutable {
        *segZoomOn = !*segZoomOn;
        m_radioModel.sendCommand(
            QString("display pan set %1 segment_zoom=%2")
                .arg(applet->panId()).arg(*segZoomOn ? 1 : 0));
    });
    connect(sw, &SpectrumWidget::filterChangeRequested,
            this, [this](int lo, int hi) {
        if (auto* s = activeSlice()) s->setFilterWidth(lo, hi);
    });

    connect(sw, &SpectrumWidget::dbmRangeChangeRequested,
            this, [this, applet, sw, pendingDbm, setStreamDbmRange, sendDbmRangeCommand]
                  (float minDbm, float maxDbm) {
        if (sw->kiwiSdrWaterfallActive()) {
            sw->setDbmRange(minDbm, maxDbm);
            sw->prepareForFftScaleChange();
            sw->reacquireNoiseFloorLock();
            return;
        }

        const bool profileLoadHeld = profileLoadRadioStateWritesHeld();
        const bool autoFloorChange = sw->pendingAutoNoiseFloorDbmRange();
        if (profileLoadPanDisplaySettling(applet->panId()) && autoFloorChange) {
            if (auto* pan = m_radioModel.panadapter(applet->panId())) {
                if (pan->panStreamId()) {
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

        pendingDbm->active = true;
        pendingDbm->minDbm = minDbm;
        pendingDbm->maxDbm = maxDbm;
        pendingDbm->requestedMs = QDateTime::currentMSecsSinceEpoch();
        setStreamDbmRange(minDbm, maxDbm, true);
        sendDbmRangeCommand(minDbm, maxDbm);
    });
    connect(sw, &SpectrumWidget::dbmRangeDragFinished,
            this, [sw, pendingDbm, setStreamDbmRange, sendDbmRangeCommand](float minDbm, float maxDbm) {
        if (sw->kiwiSdrWaterfallActive()) {
            sw->setDbmRange(minDbm, maxDbm);
            sw->prepareForFftScaleChange();
            sw->reacquireNoiseFloorLock();
            return;
        }

        pendingDbm->active = true;
        pendingDbm->minDbm = minDbm;
        pendingDbm->maxDbm = maxDbm;
        pendingDbm->requestedMs = QDateTime::currentMSecsSinceEpoch();
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
    connect(tnf, &TnfModel::tnfChanged,  this, rebuildTnfMarkers);
    connect(tnf, &TnfModel::tnfRemoved,  this, rebuildTnfMarkers);
    connect(tnf, &TnfModel::globalEnabledChanged,
            sw, &SpectrumWidget::setTnfGlobalEnabled);
    connect(tnf, &TnfModel::globalEnabledChanged,
            this, [this](bool on) {
        m_tnfIndicator->setStyleSheet(on
            ? "QLabel { color: #00b4d8; font-weight: bold; font-size: 24px; }"
            : "QLabel { color: #404858; font-weight: bold; font-size: 24px; }");
    });

    // FDX indicator style update
    connect(&m_radioModel, &RadioModel::infoChanged, this, [this]() {
        bool fdx = m_radioModel.fullDuplexEnabled();
        m_fdxIndicator->setStyleSheet(fdx
            ? "QLabel { color: #00b4d8; font-weight: bold; font-size: 24px; }"
            : "QLabel { color: #404858; font-weight: bold; font-size: 24px; }");
    });
    connect(sw, &SpectrumWidget::tnfCreateRequested,   tnf, &TnfModel::createTnf);
    connect(sw, &SpectrumWidget::tnfMoveRequested,     tnf, &TnfModel::setTnfFreq);
    connect(sw, &SpectrumWidget::tnfRemoveRequested,   tnf, &TnfModel::requestRemoveTnf);
    connect(sw, &SpectrumWidget::tnfWidthRequested,    tnf, &TnfModel::setTnfWidth);
    connect(sw, &SpectrumWidget::tnfDepthRequested,    tnf, &TnfModel::setTnfDepth);
    connect(sw, &SpectrumWidget::tnfPermanentRequested,tnf, &TnfModel::setTnfPermanent);

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
    auto* spotRebuildTimer = new QTimer(sw);
    spotRebuildTimer->setSingleShot(true);
    spotRebuildTimer->setInterval(50);  // ~20 Hz, below the FFT repaint cadence
    connect(spotRebuildTimer, &QTimer::timeout, sw, rebuildSpots);
    QPointer<QTimer> rebuildTimerGuard(spotRebuildTimer);
    auto scheduleRebuildSpots = [rebuildTimerGuard]() {
        if (rebuildTimerGuard && !rebuildTimerGuard->isActive())
            rebuildTimerGuard->start();
    };
    connect(spots, &SpotModel::spotAdded,   this, scheduleRebuildSpots);
    connect(spots, &SpotModel::spotUpdated, this, scheduleRebuildSpots);
    connect(spots, &SpotModel::spotRemoved, this, scheduleRebuildSpots);
    connect(spots, &SpotModel::spotsCleared,this, scheduleRebuildSpots);
    connect(spots, &SpotModel::spotsRefreshed, this, scheduleRebuildSpots);
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
    // Global lean render mode — every pan's Lean button drives the same
    // app-wide toggle; seed this new pan's button + widget with current state.
    connect(menu, &SpectrumOverlayMenu::leanModeToggled,
            this, &MainWindow::applyLeanMode);
    menu->setLeanChecked(m_leanMode);
    sw->setLeanMode(m_leanMode);
    connect(menu, &SpectrumOverlayMenu::fftFillAlphaChanged,
            sw, &SpectrumWidget::setFftFillAlpha);
    connect(menu, &SpectrumOverlayMenu::fftFillColorChanged,
            sw, &SpectrumWidget::setFftFillColor);
    connect(menu, &SpectrumOverlayMenu::fftHeatMapChanged,
            sw, &SpectrumWidget::setFftHeatMap);
    connect(menu, &SpectrumOverlayMenu::showGridChanged,
            sw, &SpectrumWidget::setShowGrid);
    connect(menu, &SpectrumOverlayMenu::freqGridSpacingChanged,
            sw, &SpectrumWidget::setFreqGridSpacing);
    connect(menu, &SpectrumOverlayMenu::freqScaleFontPtChanged,
            sw, &SpectrumWidget::setFreqScaleFontPt);
    connect(menu, &SpectrumOverlayMenu::fftLineWidthChanged,
            sw, &SpectrumWidget::setFftLineWidth);
    connect(menu, &SpectrumOverlayMenu::noiseFloorPositionChanged,
            sw, &SpectrumWidget::setNoiseFloorPosition);
    connect(menu, &SpectrumOverlayMenu::noiseFloorEnableChanged,
            sw, &SpectrumWidget::setNoiseFloorEnable);
    connect(sw, &SpectrumWidget::noiseFloorPositionResolved,
            menu, &SpectrumOverlayMenu::syncNoiseFloorPosition);

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
                    menu, &SpectrumOverlayMenu::syncDaxIqChannel);
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
        m_radioModel.sendCommand(
            QString("display pan set %1 fps=%2").arg(applet->panId()).arg(v));
    });
    connect(menu, &SpectrumOverlayMenu::fftWeightedAverageChanged,
            this, [this, applet, sw](bool on) {
        sw->setFftWeightedAvg(on);
        m_radioModel.sendCommand(
            QString("display pan set %1 weighted_average=%2").arg(applet->panId()).arg(on ? 1 : 0));
    });
    connect(menu, &SpectrumOverlayMenu::wfColorSchemeChanged,
            sw, &SpectrumWidget::setWfColorScheme);
    connect(menu, &SpectrumOverlayMenu::wfColorGainChanged,
            this, [this, applet, sw](int v) {
        if (!kiwiSdrProfileForPan(applet->panId()).isEmpty()) {
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
        if (!kiwiSdrProfileForPan(applet->panId()).isEmpty()) {
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
        if (!kiwiSdrProfileForPan(applet->panId()).isEmpty()) {
            return;
        }
        sw->setWfAutoBlack(on);
        // Keep the radio's auto_black flag in sync at runtime; it only reaches
        // 1 when auto-black is on AND the radio-side source is selected.
        m_radioModel.setWaterfallAutoBlack(on);
    });
    connect(menu, &SpectrumOverlayMenu::wfAutoBlackOffsetChanged,
            this, [this, applet, sw](int offset) {
        if (!kiwiSdrProfileForPan(applet->panId()).isEmpty()) {
            return;
        }
        sw->setWfAutoBlackOffset(offset);
    });
    connect(menu, &SpectrumOverlayMenu::wfAutoBlackSourceChanged,
            this, [this, sw](bool radioSide) {
        sw->setWfAutoBlackRadioSide(radioSide);
        // Radio-side → tell the radio to compute its per-tile auto-black level;
        // client-side → stop it (auto_black=0) and use our own estimate.
        m_radioModel.setWaterfallAutoBlackSource(radioSide);
    });
    const auto applyWaterfallLineDuration = [this, applet, sw](int ms) {
        const int clampedMs = std::clamp(ms, 1, 100);
        const QString profileId = kiwiSdrProfileForPan(applet->panId());
        if (!profileId.isEmpty()) {
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
                        sw->bandwidthMhz(), clampedMs);
                    break;
                }
            }
            return;
        }
        sw->setWfLineDuration(clampedMs);
        if (m_kiwiSdrClient && m_kiwiSdrClient->isConnected()
            && kiwiSliceForPan(m_radioModel, applet->panId(), m_activeSliceId)) {
            m_kiwiSdrClient->setWaterfallLineDurationMs(clampedMs);
        }
        auto* pan = m_radioModel.panadapter(applet->panId());
        if (pan && !pan->waterfallId().isEmpty())
            m_radioModel.sendCommand(
                QString("display panafall set %1 line_duration=%2").arg(pan->waterfallId()).arg(clampedMs));
    };
    connect(menu, &SpectrumOverlayMenu::wfLineDurationChanged,
            this, applyWaterfallLineDuration);
    connect(sw, &SpectrumWidget::waterfallLineDurationChangeRequested,
            this, applyWaterfallLineDuration);
    connect(menu, &SpectrumOverlayMenu::kiwiWaterfallCellChanged,
            this, [this, applet, sw](int cellDb) {
        if (!m_kiwiSdrManager) {
            return;
        }
        const QString profileId = kiwiSdrProfileForPan(applet->panId());
        if (profileId.isEmpty()) {
            return;
        }
        const KiwiSdrAntennaProfile profile =
            m_kiwiSdrManager->profile(profileId);
        m_kiwiSdrManager->setProfileWaterfallSettings(
            profileId, cellDb, profile.waterfallFloorDb,
            profile.waterfallRate);
        sw->setKiwiSdrWaterfallAdjustments(cellDb,
                                           profile.waterfallFloorDb);
        syncKiwiSdrPanadapterUiState(applet->panId());
    });
    connect(menu, &SpectrumOverlayMenu::kiwiWaterfallFloorChanged,
            this, [this, applet, sw](int floorDb) {
        if (!m_kiwiSdrManager) {
            return;
        }
        const QString profileId = kiwiSdrProfileForPan(applet->panId());
        if (profileId.isEmpty()) {
            return;
        }
        const KiwiSdrAntennaProfile profile =
            m_kiwiSdrManager->profile(profileId);
        m_kiwiSdrManager->setProfileWaterfallSettings(
            profileId, profile.waterfallCellDb, floorDb,
            profile.waterfallRate);
        sw->setKiwiSdrWaterfallAdjustments(profile.waterfallCellDb,
                                           floorDb);
        syncKiwiSdrPanadapterUiState(applet->panId());
    });
    connect(menu, &SpectrumOverlayMenu::kiwiWaterfallRateChanged,
            this, [this, applet](int rate) {
        if (!m_kiwiSdrManager) {
            return;
        }
        const QString profileId = kiwiSdrProfileForPan(applet->panId());
        if (profileId.isEmpty()) {
            return;
        }
        const KiwiSdrAntennaProfile profile =
            m_kiwiSdrManager->profile(profileId);
        m_kiwiSdrManager->setProfileWaterfallSettings(
            profileId, profile.waterfallCellDb, profile.waterfallFloorDb,
            rate);
        syncKiwiSdrPanadapterUiState(applet->panId());
    });
    // NB Waterfall Blanker (#277)
    connect(menu, &SpectrumOverlayMenu::wfBlankerEnabledChanged,
            sw, &SpectrumWidget::setWfBlankerEnabled);
    connect(menu, &SpectrumOverlayMenu::wfBlankerThresholdChanged,
            sw, &SpectrumWidget::setWfBlankerThreshold);
    connect(menu, &SpectrumOverlayMenu::backgroundImageRequested,
            this, [sw] {
        QString path = QFileDialog::getOpenFileName(
            sw->window(),
            "Choose Background Image",
            QString(),
            "Images (*.png *.jpg *.jpeg *.bmp)",
            nullptr,
            QFileDialog::DontUseNativeDialog);
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
        if (!m_adaptiveThrottleActive)
            m_radioModel.sendCommand(
                QString("display pan set %1 fps=25").arg(applet->panId()));
        m_radioModel.sendCommand(
            QString("display pan set %1 weighted_average=0").arg(applet->panId()));
        auto* pan = m_radioModel.panadapter(applet->panId());
        if (pan && !pan->waterfallId().isEmpty()) {
            m_radioModel.sendCommand(
                QString("display panafall set %1 color_gain=50").arg(pan->waterfallId()));
            m_radioModel.sendCommand(
                QString("display panafall set %1 black_level=15").arg(pan->waterfallId()));
            if (!m_adaptiveThrottleActive)
                m_radioModel.sendCommand(
                    QString("display panafall set %1 line_duration=100").arg(pan->waterfallId()));
        }

        // Persist all defaults to AppSettings
        auto& s = AppSettings::instance();
        s.setValue(sw->settingsKey("DisplayFftAverage"),          "0");
        s.setValue(sw->settingsKey("DisplayFftFps"),              "25");
        s.setValue(sw->settingsKey("DisplayFftFillAlpha"),        "0.70");
        s.setValue(sw->settingsKey("DisplayFftFillColor"),        "#00e5ff");
        s.setValue(sw->settingsKey("DisplayFftLineWidth"),        "2.0");
        s.setValue(sw->settingsKey("DisplayFftWeightedAvg"),      "False");
        s.setValue(sw->settingsKey("DisplayFftHeatMap"),          "True");
        s.setValue(sw->settingsKey("DisplayWfColorScheme"),       "0");
        s.setValue(sw->settingsKey("DisplayWfColorGain"),         "50");
        s.setValue(sw->settingsKey("DisplayWfBlackLevel"),        "15");
        s.setValue(sw->settingsKey("DisplayWfAutoBlack"),         "True");
        s.setValue(sw->settingsKey("DisplayWfAutoBlackRadioSide"), "False");
        s.setValue(sw->settingsKey("DisplayWfLineDuration"),      "100");
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
        s.save();

        // Sync all Display panel UI controls
        menu->syncDisplaySettings(0, 25, 70, false, QColor(0x00, 0xe5, 0xff),
                                  50, 15, true, 50, 100, 75, false, true, 0,
                                  true, 2.0f, false);
        menu->syncExtraDisplaySettings(false, 1.15f, 80, 0,
                                       QColor(0x0a, 0x0a, 0x14));
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
            centerMhz = std::max(centerMhz, pan->bandwidthMhz() / 2.0);
            // In-window drag moves pass the unchanged center purely to tune
            // without reveal — only emit the pan command when it actually moves.
            if (!qFuzzyCompare(centerMhz, pan->centerMhz())) {
                m_radioModel.sendCommand(
                    QString("display pan set %1 center=%2")
                        .arg(pan->panId()).arg(centerMhz, 0, 'f', 6));
            }
        }
        queueActiveSliceForSpectrumTarget(target->sliceId());
        target->setFrequency(sliceFreqMhz);
        mirrorDiversityChildFrequency(target, sliceFreqMhz);  // keep diversity child in step
    });
    // Pan Follow ("Pan Lock") stands down for the whole duration of a slice drag
    // so it doesn't fight the drag (in-window tune or edge auto-pan) with per-tick
    // recenters; on release it recenters once so Pan Lock re-asserts. (user-reported)
    connect(sw, &SpectrumWidget::sliceDragActiveChanged, this, [this](bool active) {
        m_sliceDragInProgress = active;
        if (!active) {
            recenterPanFollowOnSlice0();   // re-assert Pan Lock on release (self-guards if off)
        }
    });

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

        // Forward to DX cluster if requested
        if (forwardToCluster && m_dxCluster && m_dxCluster->isConnected()) {
            QString dxCmd = QString("DX %1 %2 %3")
                .arg(freqMhz * 1000.0, 0, 'f', 1)
                .arg(callsign)
                .arg(comment);
            QMetaObject::invokeMethod(m_dxCluster, [this, dxCmd] {
                m_dxCluster->sendCommand(dxCmd);
            });
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
    connect(sw, &SpectrumWidget::sliceTxRequested,
            this, [this](int sliceId) {
        if (auto* s = m_radioModel.slice(sliceId))
            s->setTxSlice(true);
    });
    connect(sw, &SpectrumWidget::sliceCloseRequested,
            this, [this](int sliceId) {
        if (m_radioModel.slices().size() <= 1) return;
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
            this, [this, applet](const QString& bandName, double freqMhz, const QString& mode,
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
        const auto xvtrs = xvtrPolicyBandsFrom(m_radioModel.xvtrList());
        QString stackKey = stackKeyHint;
        QString unsupportedBandReason;
        if (stackKey.isEmpty()) {
            const auto stackKeyResult =
                XvtrPolicy::resolveBandStackKey(bandName, xvtrs, m_radioModel.capabilities());
            stackKey = stackKeyResult.key;
            unsupportedBandReason = stackKeyResult.unsupportedReason;
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
            clearSwrSweepForBandChange(-1, applet->panId(), bandName);
            m_bandSettings.setCurrentBand(bandName);
            m_radioModel.sendCommand(
                QString("display pan set %1 band=%2").arg(applet->panId()).arg(stackKey));
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
                              m_kiwiSdrManager);
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
        m_radioModel.sendCommand(
            QString("display pan set %1 rfgain=%2").arg(applet->panId()).arg(gain));
        sw->setRfGain(gain);
        auto& s = AppSettings::instance();
        s.setValue(sw->settingsKey("DisplayRfGain"), QString::number(gain));
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

        pan->applyPanStatus({{"center", QString::number(result.newCenterMhz, 'f', 6)}});
        m_radioModel.sendCommand(
            QString("display pan set %1 center=%2")
                .arg(pan->panId()).arg(result.newCenterMhz, 0, 'f', 6));
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

    // Apply the center optimistically so the owning spectrum repaints
    // immediately; SpectrumWidget decides whether that becomes a short
    // retargetable animation or an immediate snap based on shift size.
    pan->applyPanStatus({{"center", QString::number(result.newCenterMhz, 'f', 6)}});
    m_radioModel.sendCommand(
        QString("display pan set %1 center=%2")
            .arg(pan->panId()).arg(result.newCenterMhz, 0, 'f', 6));
    return result;
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
            const int specW = sw->width();
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
    connect(s, &SliceModel::audioGainChanged, this, [this, s](float) {
        updateKiwiSdrVirtualAudioControlsForSlice(s);
    });
    connect(s, &SliceModel::audioMuteChanged, this, [this, s](bool) {
        updateKiwiSdrVirtualAudioControlsForSlice(s);
        syncFlexRxPanToAudioEngine();
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
    connect(s, &SliceModel::diversityChanged, this, [this](bool) {
        syncKiwiSdrDiversityEscControls();
    });
    connect(w, &VfoWidget::closeSliceRequested, this, [this, sliceId]() {
        if (m_radioModel.slices().size() <= 1) return;
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
        bool clientSide = AppSettings::instance().value("RecordingMode", "Radio").toString() == "Client";
        if (clientSide) {
            if (on)
                m_qsoRecorder->startRecording();
            else
                m_qsoRecorder->stopRecording();
            w->setRecordOn(on);  // drive pulse animation for client-side
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
        bool clientSide = AppSettings::instance().value("RecordingMode", "Radio").toString() == "Client";
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
            bool isCw = mode == "CW" || mode == "CWL";
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

    // AetherDSP button on the per-slice DSP tab — same entry point as the
    // Settings menu action and the RX chain double-click; reuses the
    // existing modeless m_dspDialog when one is already open.
    connect(w, &VfoWidget::aetherDspRequested, this, [this] {
        ensureAetherDspDialog();
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
                             || m_audio->mnrEnabled() || m_audio->bnrEnabled()
                             || m_audio->dfnrEnabled() || m_audio->rn2Enabled();
            w->setAetherDspActive(active);
        };
        connect(m_audio, &AudioEngine::nr2EnabledChanged,  w, [syncAetherDsp](bool){ syncAetherDsp(); });
        connect(m_audio, &AudioEngine::nr4EnabledChanged,  w, [syncAetherDsp](bool){ syncAetherDsp(); });
        connect(m_audio, &AudioEngine::mnrEnabledChanged,  w, [syncAetherDsp](bool){ syncAetherDsp(); });
        connect(m_audio, &AudioEngine::bnrEnabledChanged,  w, [syncAetherDsp](bool){ syncAetherDsp(); });
        connect(m_audio, &AudioEngine::dfnrEnabledChanged, w, [syncAetherDsp](bool){ syncAetherDsp(); });
        connect(m_audio, &AudioEngine::rn2EnabledChanged,  w, [syncAetherDsp](bool){ syncAetherDsp(); });
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
            m_appletPanel->sMeterWidget()->setLevel(dbm);
#ifdef HAVE_HIDAPI
            m_tmate2SmeterDbm = dbm;
            updateTMate2Display();
            updateTMate2Indicators();
#endif
        }
    });
    // Symmetric with the amp-side guard at line ~3654 and the PGXL TCP path
    // at line ~3525: in OPERATE the amp owns the analog S-Meter, so drop the
    // exciter sample here to stop the alternating-writer pulse where exciter
    // (~100 W) and amp (~1500 W) values race into the same widget. (#2927)
    connect(&m_radioModel.meterModel(), &MeterModel::txMetersChanged,
            this, [this](float fwd, float swr) {
        if (m_radioModel.hasAmplifier() && m_radioModel.ampOperate())
            return;
        m_appletPanel->sMeterWidget()->setTxMeters(fwd, swr);
#ifdef HAVE_HIDAPI
        m_tmate2TxWatts = fwd;
        if (m_radioModel.transmitModel().isTransmitting()) {
            updateTMate2Display();
            updateTMate2Indicators();
        }
#endif
    });
    connect(&m_radioModel.meterModel(), &MeterModel::micMetersChanged,
            m_appletPanel->sMeterWidget(), &SMeterWidget::setMicMeters);
    connect(&m_radioModel.transmitModel(), &TransmitModel::moxChanged,
            m_appletPanel->sMeterWidget(), &SMeterWidget::setTransmitting);

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
    }

    // Wire TgxlConnection to TunerModel
    m_radioModel.tunerModel().setDirectConnection(&m_tgxlConn);
    // Also attempt connection when TGXL IP arrives (may come after presence)
    connect(&m_radioModel.tunerModel(), &TunerModel::stateChanged, this, [this]() {
        auto* tuner = &m_radioModel.tunerModel();
        if (tuner->isPresent() && !tuner->tgxlIp().isEmpty() && !m_tgxlConn.isConnected()) {
            m_tgxlConn.connectToTgxl(tuner->tgxlIp());
        }
    });

    // Auto-connect to PGXL when detected
    connect(&m_radioModel, &RadioModel::amplifierChanged, this, [this](bool present) {
        if (present && !m_radioModel.ampIp().isEmpty() && !m_pgxlConn.isConnected()) {
            m_pgxlConn.connectToPgxl(m_radioModel.ampIp());
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
        if (kvs.contains("peakfwd") && m_radioModel.hasAmplifier()
                && m_radioModel.ampOperate()) {
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
                m_appletPanel->sMeterWidget()->setTransmitting(true);
            else if (kvs.contains("state") && !kvs.value("state").startsWith("TRANSMIT"))
                m_appletPanel->sMeterWidget()->setTransmitting(false);
            m_appletPanel->sMeterWidget()->setTxMeters(watts, swr);
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
    connect(&m_radioModel, &RadioModel::ampTelemetryUpdated,
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
    // OPERATE button → PGXL standby/operate command via radio amplifier API
    connect(m_appletPanel->ampApplet(), &AmpApplet::operateToggled, this, [this](bool on) {
        if (!m_radioModel.ampHandle().isEmpty())
            m_radioModel.sendCommand(
                QString("amplifier set %1 operate=%2").arg(m_radioModel.ampHandle()).arg(on ? 1 : 0));
    });

    // Switch Fwd Power gauge scale based on radio max power and amplifier presence.
    // All three power gauges (TxApplet, TunerApplet, SMeterWidget) update together.
    // When the PGXL is in STANDBY we fall back to the barefoot scale — only the
    // radio's forward power is reaching the meter, so the 2kW arc would make
    // every reading look tiny and useless.
    auto updatePowerScale = [this]() {
        int maxW = m_radioModel.transmitModel().maxPowerLevel();
        // Aurora (AU-) radios have an integrated 600W PA (Overlord) but
        // max_power_level only reports the exciter limit (100W). Use model
        // name to detect the true PA capability. (#484)
        const QString& model = m_radioModel.model();
        if (model.startsWith("AU-") && maxW <= 100) {
            maxW = 500;
        }
        const bool ampActive = m_radioModel.hasAmplifier()
                            && m_radioModel.ampOperate();
        m_appletPanel->txApplet()->setPowerScale(maxW, ampActive);
        m_appletPanel->tunerApplet()->setPowerScale(maxW, ampActive);
        m_appletPanel->sMeterWidget()->setPowerScale(maxW, ampActive);
        m_appletPanel->healthApplet()->setPowerScale(maxW, ampActive);
    };
    connect(&m_radioModel, &RadioModel::amplifierChanged, this, updatePowerScale);
    connect(&m_radioModel, &RadioModel::ampStateChanged, this, updatePowerScale);

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
        if (m_radioModel.ampOperate())
            setIndicatorHtml(m_pgxlIndicator, m_pgxlStateLabel, "OPERATE", "#00e060");
        else
            setIndicatorHtml(m_pgxlIndicator, m_pgxlStateLabel, "STANDBY", "#404858");
    };
    connect(&m_radioModel, &RadioModel::ampStateChanged, this, [this, updatePgxlStyle]() {
        updatePgxlStyle();
        // Sync the AmpApplet button — the direct PGXL TCP path may not deliver
        // a state update fast enough, leaving the button stuck on the old state.
        // RadioModel is authoritative; use it to keep the button consistent.
        m_appletPanel->ampApplet()->setState(
            m_radioModel.ampOperate() ? QStringLiteral("OPERATE") : QStringLiteral("STANDBY"));
    });

    connect(&m_radioModel, &RadioModel::amplifierChanged, this, [this, updatePgxlStyle](bool present) {
        m_pgxlContainer->setVisible(present);
        m_pgxlSeparator->setVisible(present);
        m_appletPanel->setAmpVisible(present);
        updateStatusBarMinimumWidth();
        if (present) {
            updatePgxlStyle();
            m_appletPanel->ampApplet()->setState(
                m_radioModel.ampOperate() ? QStringLiteral("OPERATE") : QStringLiteral("STANDBY"));
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
        if (m_radioModel.hasAmplifier() && m_radioModel.ampOperate()) {
            m_appletPanel->sMeterWidget()->setTxMeters(fwdPwr, swr);
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
    connect(&m_radioModel.transmitModel(), &TransmitModel::moxChanged,
            m_appletPanel->txApplet(), &TxApplet::setTransmitting);
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

} // namespace AetherSDR
