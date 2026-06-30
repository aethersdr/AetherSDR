// MainWindow_DspApplets.cpp — client-DSP applet wiring for MainWindow.
//
// Part of the #3351 monolith decomposition (Phase 2d). Holds
// wirePooDooTiles() and wireDspApplets(), extracted verbatim from the
// constructor (two methods because constructor chrome wiring sits
// between them in initialization order — see the call sequence there):
//
//   • PooDoo RX chain status tiles
//   • P/CW applet (mic/ALC/compression meters), PHNE, EQ
//   • Client DSP applet family: EQ / Compressor / Gate / De-esser /
//     Tube / Reverb / AetherDSP / PUDU — TX and RX tiles
//   • TX signal-chain applet + PUDU monitor wiring
//   • RX chain edit + bypass
//
// Window-side wiring (applets ↔ AudioEngine) — not session-scoped.
// Runs once at construction, at the original constructor position.

#include "MainWindow.h"

#include "AetherDspWidget.h"
#include "AppletPanel.h"
#include "AetherialAudioStrip.h"
#include "ClientCompApplet.h"
#include "ClientCompEditor.h"
#include "ClientDeEssApplet.h"
#include "ClientEqApplet.h"
#include "ClientEqEditor.h"
#include "ClientGateApplet.h"
#include "ClientGateEditor.h"
#include "ClientPuduApplet.h"
#include "ClientPuduEditor.h"
#include "ClientReverbApplet.h"
#include "ClientRxDspApplet.h"
#include "ClientTubeApplet.h"
#include "ClientTubeEditor.h"
#include "EqApplet.h"
#include "PhoneApplet.h"
#include "PhoneCwApplet.h"
#include "TitleBar.h"
#include "ClientChainApplet.h"
#include "MainWindowHelpers.h"
#include "core/AppSettings.h"
#include "core/AudioEngine.h"
#include "core/LogManager.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QTimer>

#include <memory>

#include <algorithm>

namespace AetherSDR {

void MainWindow::wirePooDooTiles()
{
    // ── PooDoo RX chain status tiles (Phase 0 chassis) ─────────────────────
    // RADIO tile = PC Audio enabled (standard SSB / remote_audio_rx stream).
    // DSP tile   = any client-side NR active (NR4 / DFNR / BNR).
    // SPEAK tile = AudioEngine output unmuted.
    if (auto* chain = m_appletPanel->clientChainApplet()) {
        // RADIO — driven by the existing PC Audio toggle in TitleBar.
        // Also forward to the strip's RX chain so its RADIO tile
        // tracks the same state.
        connect(m_titleBar, &TitleBar::pcAudioToggled, this,
                [this, chain](bool on) {
            chain->setRxPcAudioEnabled(on);
            if (m_aetherialStrip) m_aetherialStrip->setRxPcAudioEnabled(on);
        });

        // DSP — aggregate of every client-side NR module.  These are
        // mutually exclusive in AudioEngine::processRx (chained
        // if/else), so at most one is active at a time.  The tile
        // greens whenever any is on, and its label rotates to the
        // active module's short name (NR2 / RN2 / NR4 / DFNR / MNR /
        // BNR).  Shared state lives in a struct held by shared_ptr
        // captured into each lambda so lifetime ends with the last
        // connected slot.
        struct DspState {
            bool nr2{false}, rn2{false}, nr4{false},
                 dfnr{false}, mnr{false}, bnr{false};
        };
        auto dspState = std::make_shared<DspState>();
        auto pushDsp = [this, chain, dspState]() {
            // Priority order picks the most "specific" / most-recent
            // module if more than one is somehow on at once.  Same
            // order as the audio-thread dispatcher so the displayed
            // label matches what's actually processing.
            QString label;
            if      (dspState->bnr)  label = "BNR";
            else if (dspState->mnr)  label = "MNR";
            else if (dspState->dfnr) label = "DFNR";
            else if (dspState->nr4)  label = "NR4";
            else if (dspState->rn2)  label = "RN2";
            else if (dspState->nr2)  label = "NR2";
            const bool anyOn = !label.isEmpty();
            chain->setRxClientDspActive(anyOn, label);
            if (m_aetherialStrip)
                m_aetherialStrip->setRxClientDspActive(anyOn, label);
        };
        connect(m_audio, &AudioEngine::nr2EnabledChanged, chain,
                [dspState, pushDsp](bool on) { dspState->nr2 = on; pushDsp(); });
        connect(m_audio, &AudioEngine::rn2EnabledChanged, chain,
                [dspState, pushDsp](bool on) { dspState->rn2 = on; pushDsp(); });
        connect(m_audio, &AudioEngine::nr4EnabledChanged, chain,
                [dspState, pushDsp](bool on) { dspState->nr4 = on; pushDsp(); });
        connect(m_audio, &AudioEngine::dfnrEnabledChanged, chain,
                [dspState, pushDsp](bool on) { dspState->dfnr = on; pushDsp(); });
        connect(m_audio, &AudioEngine::mnrEnabledChanged, chain,
                [dspState, pushDsp](bool on) { dspState->mnr = on; pushDsp(); });
        connect(m_audio, &AudioEngine::nvAfxEnabledChanged, chain,
                [dspState, pushDsp](bool on) { dspState->bnr = on; pushDsp(); });

        // SPEAK — AudioEngine emits mutedChanged on every setMuted() flip.
        connect(m_audio, &AudioEngine::mutedChanged, this,
                [this, chain](bool muted) {
            chain->setRxOutputUnmuted(!muted);
            if (m_aetherialStrip) m_aetherialStrip->setRxOutputUnmuted(!muted);
        });

        // Seed initial state — settings and engine values are already
        // loaded by the time we reach this wiring code.  We pull from
        // the engine (not signals) so an already-on NR module shows up
        // immediately instead of waiting for the next toggle.
        const bool pcOn = AppSettings::instance()
            .value("PcAudioEnabled", "True").toString() == "True";
        chain->setRxPcAudioEnabled(pcOn);
        if (m_aetherialStrip) m_aetherialStrip->setRxPcAudioEnabled(pcOn);
        dspState->nr2  = m_audio->nr2Enabled();
        dspState->rn2  = m_audio->rn2Enabled();
        dspState->nr4  = m_audio->nr4Enabled();
        dspState->dfnr = m_audio->dfnrEnabled();
        dspState->mnr  = m_audio->mnrEnabled();
        dspState->bnr  = m_audio->nvAfxEnabled();   // BNR == local AFX denoiser
        pushDsp();
        chain->setRxOutputUnmuted(!m_audio->isMuted());
        if (m_aetherialStrip)
            m_aetherialStrip->setRxOutputUnmuted(!m_audio->isMuted());
    }
}

void MainWindow::wireDspApplets()
{
    // ── P/CW applet: mic meters + ALC meter + model ────────────────────────
    // Suppress radio CODEC meters when mic_selection=PC (they just show noise).
    // Client-side metering handles PC mic display below.
    // Compression gauge: full 20fps meter rate, gated on actual radio TX + PROC.
    {
        connect(&m_radioModel.meterModel(), &MeterModel::micMetersChanged,
                this, [this](float micLevel, float compLevel, float micPeak, float compPeak) {
            // Mic level: hardware mic uses radio meters, PC uses client-side
            if (m_radioModel.transmitModel().micSelection() != "PC")
                m_appletPanel->phoneCwApplet()->updateMeters(micLevel, compLevel, micPeak, 0.0f);

            // Compression has no useful meaning in RX; FLEX-8000 radios can
            // publish quiescent TX-chain meters there that look fully pegged.
            {
                const auto& tx = m_radioModel.transmitModel();
                const bool showCompression =
                    m_radioModel.isRadioTransmitting() && tx.speechProcessorEnable();
                m_appletPanel->phoneCwApplet()->updateCompression(
                    showCompression ? compPeak : 0.0f);
            }
        });
    }
    connect(&m_radioModel.meterModel(), &MeterModel::swAlcChanged,
            this, [this](float alc) {
        // FLEX-8000 TX-chain meters can publish quiescent RX values near 0 dBFS.
        // Only show SW ALC while the radio interlock says RF is actually keyed.
        m_appletPanel->phoneCwApplet()->updateAlc(
            m_radioModel.isRadioTransmitting() ? alc : -20.0f);
    });
    // Client-side PC mic metering — radio CODEC meters only see hardware mics.
    // Apply VU-style ballistics: fast attack, slow decay (~20 dB/sec).
    {
        auto heldLevel = std::make_shared<float>(-150.0f);
        auto heldPeak  = std::make_shared<float>(-150.0f);
        connect(m_audio, &AudioEngine::pcMicLevelChanged,
                this, [this, heldLevel, heldPeak](float peakDb, float avgDb) {
            if (m_radioModel.transmitModel().micSelection() != "PC" && !m_audio->isRadeMode()) return;
            constexpr float kDecayPerUpdate = 1.0f;  // ~20 dB/sec at 20 updates/sec
            // Level: fast attack, slow decay
            if (avgDb > *heldLevel)
                *heldLevel = avgDb;
            else
                *heldLevel = qMax(avgDb, *heldLevel - kDecayPerUpdate);
            // Peak: fast attack, slower decay
            if (peakDb > *heldPeak)
                *heldPeak = peakDb;
            else
                *heldPeak = qMax(*heldLevel, *heldPeak - kDecayPerUpdate * 0.5f);
            m_appletPanel->phoneCwApplet()->updateMeters(*heldLevel, 0.0f, *heldPeak, 0.0f);
        });
    }
    m_appletPanel->phoneCwApplet()->setTransmitModel(&m_radioModel.transmitModel());



    // ── PHNE applet: VOX + CW controls ──────────────────────────────────────
    m_appletPanel->phoneApplet()->setTransmitModel(&m_radioModel.transmitModel());

    // ── EQ applet: graphic equalizer ─────────────────────────────────────────
    m_appletPanel->eqApplet()->setEqualizerModel(&m_radioModel.equalizerModel());

    // ── Client EQ applets: dedicated TX and RX tiles (Phase 7.1) ───────────
    m_appletPanel->clientEqTxApplet()->setAudioEngine(m_audio);
    m_appletPanel->clientEqRxApplet()->setAudioEngine(m_audio);

    auto wireEqEditOpen = [this](ClientEqApplet* applet) {
        connect(applet, &ClientEqApplet::editRequested, this,
                [this](ClientEqApplet::Path path) {
            ensureClientEqEditor()->showForPath(path);
        });
    };
    wireEqEditOpen(m_appletPanel->clientEqTxApplet());
    wireEqEditOpen(m_appletPanel->clientEqRxApplet());

    // Push TX low/high filter cutoffs to the EQ canvases as dashed yellow
    // guide lines.  Subscribes to the *dedicated* txFilterCutoffChanged
    // signal — NOT the omnibus phoneStateChanged which fires on every
    // VOX/CW/mic-boost/dexp/etc. transmit-status update and would
    // cascade unnecessary repaints into the audio path during TX.
    auto pushTxFilterCutoffsToEq = [this](int lo, int hi) {
        if (m_appletPanel && m_appletPanel->clientEqTxApplet())
            m_appletPanel->clientEqTxApplet()->setTxFilterCutoffs(lo, hi);
        if (m_clientEqEditor)
            m_clientEqEditor->setTxFilterCutoffs(lo, hi);
        if (m_aetherialStrip)
            m_aetherialStrip->setTxFilterCutoffs(lo, hi);
    };
    {
        const auto& tx = m_radioModel.transmitModel();
        pushTxFilterCutoffsToEq(tx.txFilterLow(), tx.txFilterHigh());
    }
    connect(&m_radioModel.transmitModel(), &TransmitModel::txFilterCutoffChanged,
            this, pushTxFilterCutoffsToEq);

    // RX filter passband guide lines on the RX EQ canvas — fed by the
    // currently-active RX slice.  filterLow / filterHigh on a slice are
    // *offsets* (e.g. -3000..0 for LSB, 0..3000 for USB, -3000..3000 for
    // AM); the EQ canvas plots in absolute audio-frequency.  Convert:
    //   audio_high = max(|lo|, |hi|)
    //   audio_low  = (lo and hi same sign / one zero) ? min(|lo|, |hi|) : 0
    // Then push to the docked RX-bound applet + floating editor (if open).
    // setActiveSlice() and SliceModel::filterChanged both call this lambda
    // so the guides track both slice swaps and live filter drags.
    pushRxFilterCutoffsToEq();
    connect(&m_radioModel, &RadioModel::sliceAdded, this, [this](SliceModel* s) {
        if (!s) return;
        connect(s, &SliceModel::filterChanged, this,
                [this, s](int /*lo*/, int /*hi*/) {
            if (s->sliceId() == m_activeSliceId)
                pushRxFilterCutoffsToEq();
        });
    });

    // ── Client Compressor applets: TX (#1661) + RX (Phase 7.3) ─────────────
    m_appletPanel->clientCompTxApplet()->setAudioEngine(m_audio);
    m_appletPanel->clientCompRxApplet()->setAudioEngine(m_audio);
    connect(m_appletPanel->clientCompTxApplet(), &ClientCompApplet::editRequested,
            this, [this]() { ensureClientCompEditor()->showForTx(); });
    connect(m_appletPanel->clientCompRxApplet(), &ClientCompApplet::editRequested,
            this, [this]() { ensureClientCompEditor()->showForRx(); });

    // ── Client Gate applets: TX (#1661 Phase 2) + RX (Phase 7.2) ───────────
    m_appletPanel->clientGateTxApplet()->setAudioEngine(m_audio);
    m_appletPanel->clientGateRxApplet()->setAudioEngine(m_audio);
    connect(m_appletPanel->clientGateTxApplet(), &ClientGateApplet::editRequested,
            this, [this]() { ensureClientGateEditor()->showForTx(); });
    connect(m_appletPanel->clientGateRxApplet(), &ClientGateApplet::editRequested,
            this, [this]() { ensureClientGateEditor()->showForRx(); });

    // ── Client De-esser applet: TX sidechain-filtered dynamics (#1661 Phase 3) ─
    m_appletPanel->clientDeEssApplet()->setAudioEngine(m_audio);

    // ── Client Tube applets: TX (#1661) + RX (Phase 7.4) ───────────────────
    m_appletPanel->clientTubeTxApplet()->setAudioEngine(m_audio);
    m_appletPanel->clientTubeRxApplet()->setAudioEngine(m_audio);
    connect(m_appletPanel->clientTubeTxApplet(), &ClientTubeApplet::editRequested,
            this, [this]() { ensureClientTubeEditor()->showForTx(); });
    connect(m_appletPanel->clientTubeRxApplet(), &ClientTubeApplet::editRequested,
            this, [this]() { ensureClientTubeEditor()->showForRx(); });

    // ── Client Reverb applet: TX reverb (Freeverb) ─
    m_appletPanel->clientReverbApplet()->setAudioEngine(m_audio);

    // ── RX-side AetherDSP applet — same controls as the Settings menu
    // dialog, embedded as a docked tile in PooDoo Audio (RX).  Parameter
    // changes route through the same per-signal wiring used by the dialog,
    // factored into wireAetherDspWidget() to keep dialog + applet in sync.
    if (auto* a = m_appletPanel->clientRxDspApplet()) {
        a->setAudioEngine(m_audio);
        if (auto* w = a->widget())
            wireAetherDspWidget(w);
    }

    // ── Client PUDU applets: TX (#1661 Phase 5) + RX (Phase 7.5) ───────────
    m_appletPanel->clientPuduTxApplet()->setAudioEngine(m_audio);
    m_appletPanel->clientPuduRxApplet()->setAudioEngine(m_audio);
    connect(m_appletPanel->clientPuduTxApplet(), &ClientPuduApplet::editRequested,
            this, [this]() { ensureClientPuduEditor()->showForTx(); });
    connect(m_appletPanel->clientPuduRxApplet(), &ClientPuduApplet::editRequested,
            this, [this]() { ensureClientPuduEditor()->showForRx(); });

    // ── TX signal-chain applet (#1661) ──────────────────────────────────────
    // Visual strip showing MIC → stages → TX with per-stage bypass +
    // drag-drop reorder.  Clicking a stage opens its floating editor.
    //
    // TX-stage applet visibility is independent of bypass state — the
    // chain widget click and the editor Bypass buttons toggle the DSP
    // and refresh the applet's bypass indicator, but do not show or
    // hide the tile.  Users control applet visibility via the applet
    // header ✕ and toolbar toggles (persisted as Applet_<ID>).
    m_appletPanel->clientChainApplet()->setAudioEngine(m_audio);
    // PooDoo TX/RX tab → AppletPanel side filter.  Hides the inactive
    // side's per-stage applet tiles whenever the user flips the chain
    // tab.  Seed the initial side from the saved tab so the first
    // paint is correct.
    connect(m_appletPanel->clientChainApplet(),
            &ClientChainApplet::chainModeChanged,
            this, [this](ClientChainApplet::ChainMode mode) {
        if (!m_appletPanel) return;
        m_appletPanel->setPooDooActiveSide(
            mode == ClientChainApplet::ChainMode::Tx
                ? AppletPanel::PooDooSide::Tx
                : AppletPanel::PooDooSide::Rx);
    });
    // Side-filter seed is moved further down — must run AFTER
    // setTxDspChainOrder, otherwise that helper's insertChildWidget
    // calls re-show every TX container we just hid.

    // Seed the PooDoo MIC-ready + TX-pulse indicators from current
    // state — subsequent changes flow through the TransmitModel
    // signal connections above (micStateChanged + moxChanged).
    {
        const auto& tx = m_radioModel.transmitModel();
        const bool ready = (tx.micSelection() == "PC") && !tx.daxOn();
        m_appletPanel->clientChainApplet()->setMicInputReady(ready);
        m_appletPanel->clientChainApplet()->setTxActive(
            ready && tx.isTransmitting());
        if (m_aetherialStrip) {
            m_aetherialStrip->setMicInputReady(ready);
            m_aetherialStrip->setTxActive(ready && tx.isTransmitting());
        }
    }
    // Pulse the TX endpoint red when we're transmitting AND PooDoo
    // is actually in the signal path (MIC=PC and DAX off).  Otherwise
    // the pulse would lie about what's being processed.
    connect(&m_radioModel.transmitModel(), &TransmitModel::moxChanged,
            this, [this](bool txActive) {
        if (!m_appletPanel || !m_appletPanel->clientChainApplet()) return;
        const auto& tx = m_radioModel.transmitModel();
        const bool ready = (tx.micSelection() == "PC") && !tx.daxOn();
        m_appletPanel->clientChainApplet()->setTxActive(ready && txActive);
        if (m_aetherialStrip)
            m_aetherialStrip->setTxActive(ready && txActive);
    });

    // ── PUDU monitor wiring ─────────────────────────────────────
    auto* chainApplet = m_appletPanel->clientChainApplet();
    chainApplet->setMonitorHasRecording(m_finalMonitor->hasRecording());

    // Easter-egg nub on the chain applet → toggle the Aetherial Audio
    // Channel Strip.  Stubbed in step 1 of the strip plan (#2301);
    // step 4 lazy-creates the strip window and toggles visibility.
    connect(chainApplet, &ClientChainApplet::aetherialStripToggleRequested,
            this, &MainWindow::toggleAetherialStrip);

    // User-click → start/stop based on current monitor state.  The
    // monitor's own signals drive the button visuals back.
    connect(chainApplet, &ClientChainApplet::monitorRecordClicked,
            this, [this]() {
        if (m_finalMonitor->isRecording()) {
            m_finalMonitor->stopRecording();
        } else {
            // Don't record while playing — button shouldn't be
            // enabled in that state, but guard anyway.
            if (m_finalMonitor->isPlaying()) m_finalMonitor->stopPlayback();
            m_finalMonitor->startRecording();
        }
    });
    connect(chainApplet, &ClientChainApplet::monitorPlayClicked,
            this, [this]() {
        if (m_finalMonitor->isPlaying()) {
            m_finalMonitor->stopPlayback();
        } else {
            m_finalMonitor->startPlayback();
        }
    });

    // Monitor state → UI updates.  RX audio gating is handled
    // separately via the muteRxRequested wiring above.  State is
    // forwarded both to the docked ClientChainApplet AND to the
    // AetherialAudioStrip's mirrored buttons (when the strip exists).
    connect(m_finalMonitor, &ClientPuduMonitor::recordingStarted,
            this, [this]() {
        if (m_appletPanel && m_appletPanel->clientChainApplet())
            m_appletPanel->clientChainApplet()->setMonitorRecording(true);
        if (m_aetherialStrip)
            m_aetherialStrip->setMonitorRecording(true);
    });
    connect(m_finalMonitor, &ClientPuduMonitor::recordingStopped,
            this, [this](int /*durationMs*/) {
        if (m_appletPanel && m_appletPanel->clientChainApplet()) {
            auto* a = m_appletPanel->clientChainApplet();
            a->setMonitorRecording(false);
            a->setMonitorHasRecording(true);
        }
        if (m_aetherialStrip) {
            m_aetherialStrip->setMonitorRecording(false);
            m_aetherialStrip->setMonitorHasRecording(true);
        }
        // Auto-start playback — the mute stays installed across the
        // transition because the monitor only emits muteRxRequested
        // (false) at stopPlayback().
        m_finalMonitor->startPlayback();
    });
    connect(m_finalMonitor, &ClientPuduMonitor::playbackStarted,
            this, [this]() {
        if (m_appletPanel && m_appletPanel->clientChainApplet())
            m_appletPanel->clientChainApplet()->setMonitorPlaying(true);
        if (m_aetherialStrip)
            m_aetherialStrip->setMonitorPlaying(true);
    });
    connect(m_finalMonitor, &ClientPuduMonitor::playbackStopped,
            this, [this]() {
        if (m_appletPanel && m_appletPanel->clientChainApplet())
            m_appletPanel->clientChainApplet()->setMonitorPlaying(false);
        if (m_aetherialStrip)
            m_aetherialStrip->setMonitorPlaying(false);
    });
    // TX chain applet visibility is independent of bypass state — the
    // user controls show/hide via the applet header ✕ and toolbar
    // toggles, persisted via Applet_<ID>.  Bypassing a stage just
    // shows the applet as bypassed; it doesn't hide the tile.

    // Initial applet-stack order mirrors the persisted chain order
    // for both sides, and stays in sync on every subsequent drag-
    // reorder.
    if (m_audio) {
        m_appletPanel->setTxDspChainOrder(m_audio->txChainStages());
        m_appletPanel->setRxDspChainOrder(m_audio->rxChainStages());
    }
    auto reapplyPooDooSide = [this]() {
        if (!m_appletPanel) return;
        const QString savedTab = AppSettings::instance()
            .value("PooDooAudioActiveTab", "TX").toString();
        m_appletPanel->setPooDooActiveSide(
            savedTab == "RX" ? AppletPanel::PooDooSide::Rx
                             : AppletPanel::PooDooSide::Tx);
    };
    connect(m_appletPanel->clientChainApplet(),
            &ClientChainApplet::chainReordered,
            this, [this, reapplyPooDooSide]() {
        if (!m_appletPanel || !m_audio) return;
        m_appletPanel->setTxDspChainOrder(m_audio->txChainStages());
        // setTxDspChainOrder re-shows every reinserted child, so re-
        // apply the side filter to keep the inactive side hidden.
        reapplyPooDooSide();
    });
    connect(m_appletPanel->clientChainApplet(),
            &ClientChainApplet::rxChainReordered,
            this, [this, reapplyPooDooSide]() {
        if (!m_appletPanel || !m_audio) return;
        m_appletPanel->setRxDspChainOrder(m_audio->rxChainStages());
        reapplyPooDooSide();
    });

    // PooDoo TX/RX side-filter seed — runs AFTER setTxDspChainOrder
    // because that helper's insertChildWidget unconditionally shows
    // each child it reinserts, undoing any earlier hide.  Putting the
    // seed here ensures the inactive side stays hidden on first paint.
    {
        const QString savedTab = AppSettings::instance()
            .value("PooDooAudioActiveTab", "TX").toString();
        m_appletPanel->setPooDooActiveSide(
            savedTab == "RX" ? AppletPanel::PooDooSide::Rx
                             : AppletPanel::PooDooSide::Tx);
    }

    connect(m_appletPanel->clientChainApplet(),
            &ClientChainApplet::stageEnabledChanged,
            this, &MainWindow::onTxChainStageEnabledChanged);

    // ── RX chain edit + bypass ──────────────────────────────────────────────
    // Phase 1 routes RX EQ double-clicks to the existing ClientEqEditor in
    // RX path mode.  Click-bypass lands on the engine via the chain widget
    // itself; we just refresh the CEQ applet's Enable toggle here so it
    // stays in sync.
    connect(m_appletPanel->clientChainApplet(),
            &ClientChainApplet::rxEditRequested,
            this, [this](AudioEngine::RxChainStage stage) {
        switch (stage) {
            case AudioEngine::RxChainStage::Eq:
                ensureClientEqEditor()->showForPath(ClientEqApplet::Path::Rx);
                break;
            case AudioEngine::RxChainStage::Gate:
                ensureClientGateEditor()->showForRx();
                break;
            case AudioEngine::RxChainStage::Comp:
                ensureClientCompEditor()->showForRx();
                break;
            case AudioEngine::RxChainStage::Tube:
                ensureClientTubeEditor()->showForRx();
                break;
            case AudioEngine::RxChainStage::Pudu:
                ensureClientPuduEditor()->showForRx();
                break;
            default:
                break;
        }
    });
    connect(m_appletPanel->clientChainApplet(),
            &ClientChainApplet::rxStageEnabledChanged,
            this, [this](AudioEngine::RxChainStage stage, bool /*enabled*/) {
        // Keep the shared per-stage applet's Enable toggle in lock-
        // step with the click-bypass that just fired on the chain
        // widget.  Each stage routes to its own applet refresh.
        if (!m_appletPanel) return;
        switch (stage) {
            case AudioEngine::RxChainStage::Eq:
                if (m_appletPanel->clientEqRxApplet())
                    m_appletPanel->clientEqRxApplet()->refreshEnableFromEngine();
                break;
            case AudioEngine::RxChainStage::Gate:
                if (m_appletPanel->clientGateRxApplet())
                    m_appletPanel->clientGateRxApplet()->refreshEnableFromEngine();
                break;
            case AudioEngine::RxChainStage::Comp:
                if (m_appletPanel->clientCompRxApplet())
                    m_appletPanel->clientCompRxApplet()->refreshEnableFromEngine();
                break;
            case AudioEngine::RxChainStage::Tube:
                if (m_appletPanel->clientTubeRxApplet())
                    m_appletPanel->clientTubeRxApplet()->refreshEnableFromEngine();
                break;
            case AudioEngine::RxChainStage::Pudu:
                if (m_appletPanel->clientPuduRxApplet())
                    m_appletPanel->clientPuduRxApplet()->refreshEnableFromEngine();
                break;
            default:
                break;
        }
    });
}

} // namespace AetherSDR
