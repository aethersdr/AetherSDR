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
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QFileDialog>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace AetherSDR {

namespace {

constexpr double kIncrementalTriggerEdgeMarginFrac = 0.05;
constexpr double kIncrementalSettleEdgeMarginFrac = 0.06;
constexpr double kRevealComfortEdgeMarginFrac = 0.18;
constexpr double kSpectrumClickEdgeMarginFrac = 0.05;

constexpr double kMemoryRevealTargetToleranceMhz = 0.000001;

bool memoryRevealTargetMatches(double actualMhz, double targetMhz)
{
    return targetMhz <= 0.0
        || std::abs(actualMhz - targetMhz) <= kMemoryRevealTargetToleranceMhz;
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

            // Restore DEXP (downward expander) — radio does not persist across sessions
            bool dexpSaved = settings.value("DexpEnabled", "False").toString() == "True";
            int dexpLevel = settings.value("DexpLevel", "0").toInt();
            if (dexpSaved) {
                m_radioModel.transmitModel().setDexp(true);
                if (dexpLevel > 0) {
                    m_radioModel.transmitModel().setDexpLevel(dexpLevel);
                }
            }

            refreshCwDecodeState();
            refreshRttyDecodeState();
        });
    }

    // Restore per-slice DAX channel from last session (#1221).
    // Deferred so the radio's initial slice status has arrived first.
    {
        const int sliceIdx = m_radioModel.slices().indexOf(s);
        if (sliceIdx >= 0) {
            const QString key = QString("DaxChannel_Slice%1").arg(QChar('A' + sliceIdx));
            int savedDax = AppSettings::instance().value(key, "0").toInt();
            if (savedDax > 0) {
                QTimer::singleShot(300, this, [s, savedDax]() {
                    if (s) { s->setDaxChannel(savedDax); }
                });
            }
        }
    }

    // Re-claim TX assignment after profile load or slice recreation (#145).
    // The radio sets tx=1 on the slice but tx_client_handle may be 0x00000000
    // if the slice was destroyed and recreated (e.g. by profile global load).
    if (s->isTxSlice())
        m_radioModel.sendCommand(QString("slice set %1 tx=1").arg(s->sliceId()));

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
        m_radioModel.transmitModel().setDax(isDigital);
        m_audio->setDaxTxMode(isDigital);
        if (isDigital)
            m_radioModel.ensureDaxTxStream(DaxTxRequestReason::HostedDaxBridge);
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
    // Squelch threshold line on the spectrum overlay — only update for the
    // active slice so that inactive slices sharing a pan don't overwrite it.
    connect(s, &SliceModel::squelchChanged, this, [this, s](bool on, int level) {
        if (s != activeSlice()) return;
        if (auto* sw = spectrumForSlice(s))
            sw->setSquelchLine(on, level);
    });
    if (auto* sw = spectrumForSlice(s))
        sw->setSquelchLine(s->squelchOn(), s->squelchLevel());

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
            vfo, [vfo, sid](int sliceIndex, float dbm) {
        if (sliceIndex == sid)
            vfo->setSignalLevel(dbm);
    });
    // Feed ESC meter per-slice — signal strength after ESC processing
    connect(&m_radioModel.meterModel(), &MeterModel::escLevelChanged,
            vfo, [vfo, sid](int sliceIndex, float dbm) {
        if (sliceIndex == sid)
            vfo->setEscLevel(dbm);
    });
    connect(&m_radioModel, &RadioModel::antListChanged,
            vfo, &VfoWidget::setAntennaList);

    // Reset band-stack auto-save dwell timer on every active-slice tune
    connect(s, &SliceModel::frequencyChanged, this, [this, s]() {
        if (s->sliceId() != m_activeSliceId) return;
        if (!m_bsAutoSaveTimer) return;
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
}

void MainWindow::onSliceRemoved(int id)
{
    if (m_applyingLayout) return;

    qDebug() << "MainWindow: slice removed" << id;

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
}


void MainWindow::wirePanadapter(PanadapterApplet* applet)
{
    auto* sw = applet->spectrumWidget();
    auto* menu = sw->overlayMenu();

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

    // Wire band plan manager to this spectrum widget
    sw->setBandPlanManager(m_bandPlanMgr);

    // Set panadapter bandwidth zoom limits based on radio model
    sw->setBandwidthLimits(m_radioModel.minPanBandwidthMhz(),
                           m_radioModel.maxPanBandwidthMhz());

    // Set panId on the overlay menu so +RX routes to the correct pan
    menu->setPanId(applet->panId());
    menu->setMemories(m_radioModel.memories());
    menu->setRadioModel(&m_radioModel);
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
            const int xpix = panXpixelsFor(sw);
            const int ypix = panYpixelsFor(sw);
            m_radioModel.sendCommand(
                QString("display pan set %1 xpixels=%2 ypixels=%3")
                    .arg(pan->panId()).arg(xpix).arg(ypix));
            if (pan->panStreamId()) {
                m_radioModel.panStream()->setYPixels(pan->panStreamId(), ypix);
                sw->prepareForFftScaleChange();
            }
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
        m_radioModel.sendCommand(QString("display pan remove %1").arg(panId));
    });

    // ── User drag actions from spectrum → radio (per-pan) ──────────────────
    connect(sw, &SpectrumWidget::frequencyRangeChangeRequested,
            this, [this, applet](double center, double bandwidth) {
        applyPanRangeRequest(applet->panId(), center, bandwidth, "explicit-pan-range");
    });
    connect(sw, &SpectrumWidget::bandwidthChangeRequested,
            this, [this, applet](double bw) {
        m_radioModel.sendCommand(
            QString("display pan set %1 bandwidth=%2").arg(applet->panId()).arg(bw, 0, 'f', 6));
    });
    connect(sw, &SpectrumWidget::centerChangeRequested,
            this, [this, applet](double center) {
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
            this, [pendingDbm, setStreamDbmRange, sendDbmRangeCommand](float minDbm, float maxDbm) {
        pendingDbm->active = true;
        pendingDbm->minDbm = minDbm;
        pendingDbm->maxDbm = maxDbm;
        pendingDbm->requestedMs = QDateTime::currentMSecsSinceEpoch();
        setStreamDbmRange(minDbm, maxDbm, true);
        sendDbmRangeCommand(minDbm, maxDbm);
    });
    connect(sw, &SpectrumWidget::dbmRangeDragFinished,
            this, [pendingDbm, setStreamDbmRange, sendDbmRangeCommand](float minDbm, float maxDbm) {
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
            sw, &SpectrumWidget::setAutoSquelchEnable);
    connect(m_appletPanel->rxApplet(), &RxApplet::squelchStateChanged,
            sw, &SpectrumWidget::setSquelchLine);
    // Spectrum widget → radio + back to overlay: apply level and immediately
    // update the squelch line on sw so the yellow line never depends on the
    // RxApplet → squelchStateChanged → setSquelchLine round-trip, which can
    // miss the first emission if wirePanadapter runs before setSlice.
    connect(sw, &SpectrumWidget::autoSquelchLevelSuggested,
            this, [this, sw](int level) {
        if (auto* s = activeSlice()) s->setSquelch(true, level);
        sw->setSquelchLine(true, level);
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
        sw->setWfColorGain(v);
        auto* pan = m_radioModel.panadapter(applet->panId());
        if (pan && !pan->waterfallId().isEmpty())
            m_radioModel.sendCommand(
                QString("display panafall set %1 color_gain=%2").arg(pan->waterfallId()).arg(v));
    });
    connect(menu, &SpectrumOverlayMenu::wfBlackLevelChanged,
            this, [this, applet, sw](int v) {
        sw->setWfBlackLevel(v);
        auto* pan = m_radioModel.panadapter(applet->panId());
        if (pan && !pan->waterfallId().isEmpty())
            m_radioModel.sendCommand(
                QString("display panafall set %1 black_level=%2").arg(pan->waterfallId()).arg(v));
    });
    connect(menu, &SpectrumOverlayMenu::wfAutoBlackChanged,
            this, [this, sw](bool on) {
        sw->setWfAutoBlack(on);
    });
    connect(menu, &SpectrumOverlayMenu::wfAutoBlackOffsetChanged,
            this, [sw](int offset) {
        sw->setWfAutoBlackOffset(offset);
    });
    const auto applyWaterfallLineDuration = [this, applet, sw](int ms) {
        const int clampedMs = std::clamp(ms, 1, 100);
        sw->setWfLineDuration(clampedMs);
        auto* pan = m_radioModel.panadapter(applet->panId());
        if (pan && !pan->waterfallId().isEmpty())
            m_radioModel.sendCommand(
                QString("display panafall set %1 line_duration=%2").arg(pan->waterfallId()).arg(clampedMs));
    };
    connect(menu, &SpectrumOverlayMenu::wfLineDurationChanged,
            this, applyWaterfallLineDuration);
    connect(sw, &SpectrumWidget::waterfallLineDurationChangeRequested,
            this, applyWaterfallLineDuration);
    // NB Waterfall Blanker (#277)
    connect(menu, &SpectrumOverlayMenu::wfBlankerEnabledChanged,
            sw, &SpectrumWidget::setWfBlankerEnabled);
    connect(menu, &SpectrumOverlayMenu::wfBlankerThresholdChanged,
            sw, &SpectrumWidget::setWfBlankerThreshold);
    connect(menu, &SpectrumOverlayMenu::backgroundImageRequested,
            this, [this, sw] {
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
        sw->setFftWeightedAvg(false);
        sw->setFftHeatMap(true);
        sw->setWfColorScheme(0);
        sw->setWfColorGain(50);
        sw->setWfBlackLevel(15);
        sw->setWfAutoBlack(true);
        sw->setWfAutoBlackOffset(50);
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
        s.setValue(sw->settingsKey("DisplayFftWeightedAvg"),      "False");
        s.setValue(sw->settingsKey("DisplayFftHeatMap"),          "True");
        s.setValue(sw->settingsKey("DisplayWfColorScheme"),       "0");
        s.setValue(sw->settingsKey("DisplayWfColorGain"),         "50");
        s.setValue(sw->settingsKey("DisplayWfBlackLevel"),        "15");
        s.setValue(sw->settingsKey("DisplayWfAutoBlack"),         "True");
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
                                  50, 15, true, 50, 100, 75, false, true, 0);
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
            if (m_tciServer && s && !s->isLocked() && !m_swrSweep.running)
                m_tciServer->notifySpotClicked(spotIndex, s);
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
                              &m_tgxlConn, &m_pgxlConn, &m_antennaGenius);
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
    // same marker) extend the trigger on *both* sides; single-flag slices
    // extend only on the side the flag currently renders on.  Non-flagged
    // and compact-mode slices fall through to the original slice-frequency
    // comparison (both offsets stay 0.0).
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
                } else if (vfo->onLeft()) {
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

    // Note: w->setSlice(s) is called at the end of this method (line ~1895)

    // Per-slice signals — these are specific to the slice this widget represents
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
    connect(w, &VfoWidget::playToggled, this, [this, w, sliceId](bool on) {
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

    // AetherDSP button on the per-slice DSP tab — same entry point as the
    // Settings menu action and the RX chain double-click; reuses the
    // existing modeless m_dspDialog when one is already open.
    connect(w, &VfoWidget::aetherDspRequested, this, [this] {
        ensureAetherDspDialog();
    });

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
    // with the radio-side pan value from ALL sources (VFO panel, RX applet,
    // MIDI, radio echo-back on connect).  Connecting SliceModel::audioPanChanged
    // covers every path that calls setAudioPan(); only the active slice's value
    // matters because AudioEngine plays one slice at a time.
    connect(s, &SliceModel::audioPanChanged, this, [this, sliceId](int v) {
        if (sliceId == m_activeSliceId)
            m_audio->setRxPan(v);
    });
    connect(s, &SliceModel::rxAntennaChanged, this, [this, sliceId](const QString&) {
        if (auto* sl = m_radioModel.slice(sliceId)) {
            if (auto* sw = spectrumForSlice(sl)) {
                sw->reacquireNoiseFloorLock();
            }
        }
    });

    // Wire slice data into widget
    w->setRadioModel(&m_radioModel);
    w->setSlice(s);
    w->setAntennaList(m_radioModel.antennaList());
    w->setTransmitModel(&m_radioModel.transmitModel());
}

// wireActiveVfoSignals removed — NR2/RN2/RADE are now wired permanently
// in wireVfoWidget() so connections survive focus switches (#227).


} // namespace AetherSDR
