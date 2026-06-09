// MainWindow_DigitalModes.cpp — digital-mode subsystems of MainWindow.
//
// Part of the #3351 monolith decomposition (Phase 1e). Holds the
// activation/deactivation and routing for every digital-mode surface:
//
//   • RADE digital voice: activateRADE / deactivateRADE / slice-mode watch
//   • Classic FreeDV (FDVU/FDVL): display activation + meter routing +
//     the FreeDV Reporter window and spot reporting
//   • DAX: startDax / stopDax / per-slice channel wiring
//   • AX.25 / AetherModem: decode dialog + KISS TNC startup
//   • RTTY: decoder output routing
//
// Pure code motion from MainWindow.cpp — same class, no header changes.

#include "MainWindow.h"

#include "AppletPanel.h"
#include "Ax25HfPacketDecodeDialog.h"
#include "DaxApplet.h"
#include "PhoneCwApplet.h"
#include "VfoWidget.h"
#include "DaxIqApplet.h"
#include "MainWindowHelpers.h"
#include "PanadapterApplet.h"
#include "PanadapterStack.h"
#include "SpectrumWidget.h"
#ifdef HAVE_RADE
#include "RadeApplet.h"
#include "core/RADEEngine.h"
#endif
#if defined(Q_OS_MAC)
#include "core/VirtualAudioBridge.h"
#elif defined(HAVE_PIPEWIRE)
#include "core/PipeWireAudioBridge.h"
#endif
#include "core/AppSettings.h"
#include "core/LogManager.h"
#include "models/BandPlanManager.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QCoreApplication>
#include <QMessageBox>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace AetherSDR {

void MainWindow::showAx25HfPacketDecodeDialog()
{
    SliceModel* slice = activeSlice();

    // Construct on first open if it didn't already come up via
    // startKissTncOnStartupIfConfigured(). Intentionally NOT using
    // showOrRaisePersistent() here because that template sets
    // WA_DeleteOnClose: closing the window would destroy the dialog and
    // along with it the KISS TCP server, dropping every connected client
    // without warning. The TNC server lifecycle is decoupled from the
    // window's open/close cycle — the dialog stays alive as long as
    // MainWindow does and is just hidden on close.
    if (!m_ax25HfPacketDecodeDialog) {
        auto* dlg = new Ax25HfPacketDecodeDialog(m_audio, &m_radioModel, slice, this);
        dlg->setFramelessMode(
            AppSettings::instance().value("FramelessWindow", "True").toString() == "True");
        m_ax25HfPacketDecodeDialog = dlg;
        m_persistentDialogs.append(QPointer<PersistentDialog>(dlg));
    }
    m_ax25HfPacketDecodeDialog->setAttachedSlice(slice);
#ifdef HAVE_MQTT
    m_ax25HfPacketDecodeDialog->setMqttClient(m_mqttClient);
#endif
    m_ax25HfPacketDecodeDialog->show();
    m_ax25HfPacketDecodeDialog->raise();
    m_ax25HfPacketDecodeDialog->activateWindow();
}

void MainWindow::startKissTncOnStartupIfConfigured()
{
    if (m_ax25HfPacketDecodeDialog)
        return; // already constructed (e.g. user opened the window)
    // One-shot migration from legacy flat keys (AetherModemKissTnc*) into
    // the nested-JSON blob (Constitution Principle V). Safe to call on
    // every startup — no-op once the blob exists.
    TncSettings::migrateLegacy();
    if (!TncSettings::startOnStartup())
        return;

    // Construct the AetherModem window hidden and persistent (no WA_DeleteOnClose)
    // so the KISS TCP server runs from launch and survives the window being
    // closed. The dialog's constructor auto-starts the TNC per the same setting.
    auto* dlg = new Ax25HfPacketDecodeDialog(m_audio, &m_radioModel, activeSlice(), this);
    dlg->setFramelessMode(
        AppSettings::instance().value("FramelessWindow", "True").toString() == "True");
    m_ax25HfPacketDecodeDialog = dlg;
    m_persistentDialogs.append(QPointer<PersistentDialog>(dlg));
#ifdef HAVE_MQTT
    m_ax25HfPacketDecodeDialog->setMqttClient(m_mqttClient);
#endif
}

// External-controller methods (FlexControl, HID encoders / RC-28 / TMate 2 /
// Ulanzi / PowerMate / Shuttle, StreamDeck labels, control-devices snapshot)
// live in MainWindow_Controllers.cpp (#3351 Phase 1a).


void MainWindow::routeRttyDecoderOutput()
{
    PanadapterApplet* target = nullptr;
    if (auto* s = activeSlice(); s && m_panStack && !s->panId().isEmpty())
        target = m_panStack->panadapter(s->panId());
    if (!target) target = m_panApplet;

    if (target == m_rttyDecoderApplet) return;

    if (m_rttyDecoderApplet) {
        disconnect(&m_rttyDecoder, &RttyDecoder::textDecoded,
                   m_rttyDecoderApplet, &PanadapterApplet::appendRttyText);
        disconnect(&m_rttyDecoder, &RttyDecoder::statsUpdated,
                   m_rttyDecoderApplet, &PanadapterApplet::setRttyStats);
        disconnect(m_rttyDecoderApplet, &PanadapterApplet::rttyMarkHzChanged,
                   &m_rttyDecoder, &RttyDecoder::setMarkFreqHz);
        disconnect(m_rttyDecoderApplet, &PanadapterApplet::rttyShiftHzChanged,
                   &m_rttyDecoder, &RttyDecoder::setShiftHz);
        disconnect(m_rttyDecoderApplet, &PanadapterApplet::rttyBaudChanged,
                   &m_rttyDecoder, &RttyDecoder::setBaudRate);
        disconnect(m_rttyDecoderApplet, &PanadapterApplet::rttyReverseChanged,
                   &m_rttyDecoder, &RttyDecoder::setReversePolarity);
        disconnect(m_rttyDecoderApplet, &PanadapterApplet::rttyPanelCloseRequested,
                   &m_rttyDecoder, &RttyDecoder::stop);
    }

    m_rttyDecoderApplet = target;

    if (m_rttyDecoderApplet) {
        connect(&m_rttyDecoder, &RttyDecoder::textDecoded,
                m_rttyDecoderApplet, &PanadapterApplet::appendRttyText);
        connect(&m_rttyDecoder, &RttyDecoder::statsUpdated,
                m_rttyDecoderApplet, &PanadapterApplet::setRttyStats);
        connect(m_rttyDecoderApplet, &PanadapterApplet::rttyMarkHzChanged,
                &m_rttyDecoder, &RttyDecoder::setMarkFreqHz);
        connect(m_rttyDecoderApplet, &PanadapterApplet::rttyShiftHzChanged,
                &m_rttyDecoder, &RttyDecoder::setShiftHz);
        connect(m_rttyDecoderApplet, &PanadapterApplet::rttyBaudChanged,
                &m_rttyDecoder, &RttyDecoder::setBaudRate);
        connect(m_rttyDecoderApplet, &PanadapterApplet::rttyReverseChanged,
                &m_rttyDecoder, &RttyDecoder::setReversePolarity);
        connect(m_rttyDecoderApplet, &PanadapterApplet::rttyPanelCloseRequested,
                &m_rttyDecoder, &RttyDecoder::stop);
    }
}

void MainWindow::refreshRttyDecodeState()
{
    auto* s = activeSlice();
    // Only auto-activate for explicit RTTY mode.  DIGL is a general LSB-data
    // mode used for PSK31, FT8, SSTV, etc. — showing a Baudot decoder on
    // those signals would be confusing.  Users who do FSK on DIGL can open
    // the panel manually via the slice context menu (future work).
    const bool isRtty = s && s->mode() == "RTTY";

    if (m_rttyDecoderApplet)
        m_rttyDecoderApplet->setRttyPanelVisible(isRtty);

    if (!isRtty) {
        if (m_rttyDecoder.isRunning()) m_rttyDecoder.stop();
        return;
    }

    // Can't push params or start without the applet — the panel combos
    // hold the user's choices and we have no fallback source for them.
    if (!m_rttyDecoderApplet) return;

    const int markHz = m_rttyDecoderApplet->rttyMarkHz();
    // markHz == 0 means "Auto": follow the radio's rttyMark setting
    const int effectiveMark = (markHz > 0) ? markHz : s->rttyMark();
    m_rttyDecoder.setMarkFreqHz(effectiveMark);
    m_rttyDecoder.setShiftHz(m_rttyDecoderApplet->rttyShiftHz());
    m_rttyDecoder.setBaudRate(m_rttyDecoderApplet->rttyBaud());
    m_rttyDecoder.setReversePolarity(m_rttyDecoderApplet->rttyReverse());

    if (!m_rttyDecoder.isRunning()) m_rttyDecoder.start();
}


#ifdef HAVE_RADE
void MainWindow::activateRADE(int sliceId)
{
    // Guard against duplicate activation (VfoWidget + RxApplet both selecting RADE)
    if (m_radeSliceId == sliceId && m_radeEngine && m_radeEngine->isActive())
        return;

    // If RADE is already active on a different slice, deactivate it first
    if (m_radeSliceId >= 0 && m_radeSliceId != sliceId)
        deactivateRADE();

    auto* s = m_radioModel.slice(sliceId);
    if (!s) return;

    // Capture TX slice owner before potentially moving the badge, so the
    // failure path can restore it.  -1 means no TX slice existed.
    int prevTxSliceId = sliceId;
    if (!s->isTxSlice()) {
        prevTxSliceId = -1;
        for (auto* sl : m_radioModel.slices()) {
            if (sl && sl->isTxSlice()) { prevTxSliceId = sl->sliceId(); break; }
        }
    }

    // RADE needs to be the TX slice so it can transmit modem audio.
    // Move TX badge to the RADE slice automatically.
    if (!s->isTxSlice())
        s->setTxSlice(true);

    // Set radio mode to DIGU/DIGL (passthrough for OFDM modem).
    // Use band convention from BandDefs to pick sideband — 60m is USB
    // despite being below 10 MHz (#875).
    double freqMhz = s->frequency();
    QString mode = "DIGU";
    for (const auto& band : AetherSDR::kBands) {
        if (freqMhz >= band.lowMhz && freqMhz <= band.highMhz) {
            mode = (QString(band.defaultMode) == "LSB") ? "DIGL" : "DIGU";
            break;
        }
    }
    const QString prevMode       = s->mode();
    const int     prevFilterLow  = s->filterLow();
    const int     prevFilterHigh = s->filterHigh();
    s->setMode(mode);
    if (mode == "DIGL")
        s->setFilterWidth(-2250, -750);
    else
        s->setFilterWidth(750, 2250);

    // Remember which slice and its previous mute state
    m_radeSliceId = sliceId;
    m_radePrevMute = s->audioMute();
    s->setAudioMute(true);

    // Auto-deactivate if the slice mode is changed externally (TCI, profile
    // load, remote SmartSDR client) — those paths bypass the VfoWidget and
    // RxApplet combos and would otherwise leave audio_mute=1 stranded.
    connect(s, &SliceModel::modeChanged,
            this, &MainWindow::onRadeSliceModeChanged,
            Qt::UniqueConnection);

    // Create RADE engine on a worker thread for multi-core utilization
    if (!m_radeEngine) {
        m_radeEngine = new RADEEngine;  // no parent — will be moved to worker thread
        m_radeThread = new QThread(this);
        m_radeThread->setObjectName("RADEEngine");
        m_radeEngine->moveToThread(m_radeThread);
        connect(m_radeThread, &QThread::finished, m_radeEngine, &QObject::deleteLater);
        m_radeThread->start();
    }
    // start() must be invoked on the worker thread
    bool ok = false;
    QMetaObject::invokeMethod(m_radeEngine, [this, &ok]() {
        ok = m_radeEngine->start();
    }, Qt::BlockingQueuedConnection);
    if (!ok) {
        qCWarning(lcRade) << "MainWindow: RADE engine failed to start — restoring slice state";
        deactivateRADE();
        if (auto* sl = m_radioModel.slice(sliceId)) {
            sl->setMode(prevMode);
            sl->setFilterWidth(prevFilterLow, prevFilterHigh);
            if (prevTxSliceId != sliceId) {
                if (prevTxSliceId >= 0) {
                    if (auto* prevTx = m_radioModel.slice(prevTxSliceId)) {
                        prevTx->setTxSlice(true);
                    }
                } else {
                    sl->setTxSlice(false);
                }
            }
        }
        return;
    }
    m_radioModel.setDigitalVoiceTxSlice(sliceId);

    // Encode the operator callsign into the EOO frame so the far end can decode it.
    // Resolution order mirrors startFreeDvReporting: radio-stored callsign if
    // "Use radio" is set and non-empty, otherwise the user's saved FreeDV callsign.
    {
        auto& cs = AppSettings::instance();
        QString callsign;
        if (cs.value("FreeDvUseRadioCallsign", "True").toString() == "True"
                && !m_radioModel.callsign().isEmpty()) {
            callsign = m_radioModel.callsign();
        } else {
            callsign = cs.value("FreeDvMyCallsign", "").toString().trimmed().toUpper();
        }
        if (!callsign.isEmpty()) {
            QMetaObject::invokeMethod(m_radeEngine, [this, callsign]() {
                m_radeEngine->setTxCallsign(callsign);
            }, Qt::QueuedConnection);
        } else {
            qCWarning(lcRade) << "MainWindow: RADE TX EOO callsign not set — configure callsign in SpotHub FreeDV tab";
        }
    }

    // RADE sends VITA-49 modem audio directly (like TCI), so it needs its own
    // dax_tx stream regardless of platform.  On Windows the ExternalDaxRouteOnly
    // path used by updateDaxTxMode() is intentionally blocked by policy (SmartSDR
    // DAX2 owns the audio-device layer), but that policy doesn't apply here —
    // RADE never touches Windows audio devices.  RadeModemTx is always allowed.
    m_radioModel.ensureDaxTxStream(DaxTxRequestReason::RadeModemTx);

    // Only route mic→RADE when the RADE slice IS the TX slice.
    // If another slice is TX (e.g. USB voice), leave its audio path alone.
    m_audio->setRadeMode(s->isTxSlice());

    // TX path: mic -> RADEEngine (worker) -> sendModemTxAudio (main)
    connect(m_audio, &AudioEngine::txRawPcmReady,
            m_radeEngine, &RADEEngine::feedTxAudio, Qt::QueuedConnection);
    connect(m_radeEngine, &RADEEngine::txModemReady,
            m_audio, &AudioEngine::sendModemTxAudio, Qt::QueuedConnection);

    // Phase 3: PTT Orchestration — three-layer intercept to hold TX open until
    // the EOO frame has fully played out on the radio before set_mox=0 is sent.
    //
    // Layer 1 — PttOffHook: catches MOX button (TxApplet) and TciServer callers
    // that go through TransmitModel::requestPttOff(). Fires BEFORE setMox(false),
    // so the radio stays in TX while EOO is generated and sent.
    m_radioModel.transmitModel().setPttOffHook([this]() {
        if (m_radeEooPending) {
            qCDebug(lcRade) << "MainWindow: PttOffHook — EOO already pending, ignoring duplicate";
            return;
        }
        m_radeEooPending = true;
        qCDebug(lcRade) << "MainWindow: PttOffHook — intercepted requestPttOff, deferring for RADE EOO";
        QMetaObject::invokeMethod(m_radeEngine, [this]() {
            m_radeEngine->setEooRequested(true);
        }, Qt::QueuedConnection);
    });

    // Layer 2 — eooFinished: once EOO audio is queued in AudioEngine, close the
    // audio gate (after EOO packets) then wait for the full EOO playout before
    // dropping the carrier. EOO=144ms + silence=60ms + margin=50ms = 254ms.
    connect(m_radeEngine, &RADEEngine::eooFinished, this, [this]() {
        if (!m_radeEooPending) {
            qCDebug(lcRade) << "MainWindow: eooFinished — no pending PTT release (EOO triggered without an intercepted unkey; no carrier to release)";
            return;
        }
        m_radeEooPending = false;
        m_radeTxActive = false;

        // Post setTransmitting(false) AFTER the queued txModemReady(eoo/silence)
        // signals so the audio gate closes only after EOO is in the UDP send buffer.
        if (m_audio)
            QMetaObject::invokeMethod(m_audio, [this]() {
                m_audio->setTransmitting(false);
            }, Qt::QueuedConnection);

        constexpr int kEooPlaybackMs = RADEEngine::kEooFrameMs
                                     + RADEEngine::kEooSilenceTailMs
                                     + RADEEngine::kEooTransportMarginMs;
        qCDebug(lcRade) << "MainWindow: RADE eooFinished — deferring xmit 0 by"
                        << kEooPlaybackMs << "ms for EOO playback";
        QTimer::singleShot(kEooPlaybackMs, this, [this]() {
            qCDebug(lcRade) << "MainWindow: RADE EOO playback timer expired — releasing radio PTT";
            m_radioModel.setTransmit(false);
        });
    });

    // Layer 3 — moxChanged fallback: catches interlock and hardware PTT paths
    // (radio-initiated unkey) that bypass both layers above.
    // tx=true: new over starting — clear pending flag and reset engine EOO state.
    // tx=false + !pending + isTransmitting: unintercepted unkey — request EOO as
    //   best-effort (radio may already be in RX, but at least the app won't hang).
    m_radeMoxFallbackConn = connect(&m_radioModel.transmitModel(), &TransmitModel::moxChanged,
            this, [this](bool tx) {
        if (!m_radeEngine || !m_radeEngine->isActive()) return;
        if (tx) {
            m_radeEooPending = false;
            m_radeTxActive = true;
            QMetaObject::invokeMethod(m_radeEngine, [this]() {
                m_radeEngine->resetTx();
            }, Qt::QueuedConnection);
            qCDebug(lcRade) << "MainWindow: MOX asserted — RADE TX state reset for new over";
        } else if (!m_radeEooPending && m_radeTxActive) {
            qCDebug(lcRade) << "MainWindow: moxChanged(false) fallback — unintercepted PTT release, requesting EOO";
            m_radeEooPending = true;
            QMetaObject::invokeMethod(m_radeEngine, [this]() {
                m_radeEngine->setEooRequested(true);
            }, Qt::QueuedConnection);
        }
    });

    // RX path: DAX RX audio -> RADEEngine (worker) -> decoded speech -> speaker (main)
    // Filter by the RADE slice's DAX channel so other slices' DAX audio is ignored.
    // Look up the channel live so it tracks if the user changes DAX assignment.
    int sid = sliceId;
    connect(m_radioModel.panStream(), &PanadapterStream::daxAudioReady,
            m_radeEngine, [this, sid](int channel, const QByteArray& pcm) {
        auto* s = m_radioModel.slice(sid);
        if (s && channel == s->daxChannel())
            m_radeEngine->feedRxAudio(channel, pcm);
    }, Qt::QueuedConnection);
    connect(m_radeEngine, &RADEEngine::rxSpeechReady,
            m_audio, &AudioEngine::feedDecodedSpeech, Qt::QueuedConnection);

    // If TCI or another path already registered a stream — RADE rides it.
    {
        SliceModel* radeSlice = m_radioModel.slice(sliceId);
        int daxCh = radeSlice ? radeSlice->daxChannel() : 0;
        if (daxCh >= 1 && daxCh <= 4) {
            quint32 existing = m_radioModel.panStream()->daxStreamIdForChannel(daxCh);
            if (existing) {
                // TCI or another path already registered a stream — RADE rides it.
                qCDebug(lcRade) << "MainWindow: RADE reusing existing dax_rx ch" << daxCh
                                << "stream" << Qt::hex << existing;
            } else {
                m_radeDaxStreamConn = connect(
                    &m_radioModel, &RadioModel::statusReceived,
                    this, [this, daxCh](const QString& obj, const QMap<QString,QString>& kvs) {
                        if (!obj.startsWith("stream ")) return;
                        if (kvs.value("type") != "dax_rx") return;
                        quint32 streamId = obj.mid(7).toUInt(nullptr, 16);
                        int ch = kvs.value("dax_channel").toInt();
                        if (streamId && ch == daxCh) {
                            m_radioModel.panStream()->registerDaxStream(streamId, ch);
                            m_radeDaxStreamId = streamId;
                            disconnect(m_radeDaxStreamConn);
                            qCDebug(lcRade) << "MainWindow: RADE registered dax_rx ch" << ch
                                            << "stream" << Qt::hex << streamId;
                        }
                    });
                m_radioModel.sendCommand(
                    QString("stream create type=dax_rx dax_channel=%1").arg(daxCh));
            }
        } else {
            qWarning() << "MainWindow: RADE slice" << sliceId
                       << "has no DAX channel assigned — RX audio will not flow";
        }
    }

    // Restore client-side mic gain for RADE. The radio's mic input is unused in
    // RADE mode so PcMicGain applies regardless of the current mic_selection.
    {
        int gain = AppSettings::instance().value("PcMicGain", 100).toInt();
        m_audio->setPcMicGain(gain);
    }
    m_appletPanel->phoneCwApplet()->setRadeActive(true);

    // Start mic capture if not already running
    if (!m_audio->isTxStreaming()) {
        audioStartTx(m_radioModel.radioAddress(), 4991);
    }

    // RADE status indicator in VFO widget.
    // Use spectrumForSlice() to find the correct pan — in multi-pan layouts
    // spectrum() returns the *active* pan, which may not be the RADE slice's pan.
    if (auto* sw = spectrumForSlice(m_radioModel.slice(sliceId))) {
        if (auto* vfo = sw->vfoWidget(sliceId)) {
            vfo->setRadeActive(true);
            // Show initial unsynchronised state immediately — syncChanged only fires
            // from feedRxAudio() which requires DAX audio to be flowing first.
            vfo->setRadeSynced(false);
            connect(m_radeEngine, &RADEEngine::syncChanged,
                    vfo, &VfoWidget::setRadeSynced);
            connect(m_radeEngine, &RADEEngine::snrChanged,
                    vfo, &VfoWidget::setRadeSnr);
            connect(m_radeEngine, &RADEEngine::freqOffsetChanged,
                    vfo, &VfoWidget::setRadeFreqOffset);
            connect(m_radeEngine, &RADEEngine::eooCallsignReceived,
                    vfo, &VfoWidget::setRadeCallsign, Qt::QueuedConnection);
        }
    }

    if (auto* applet = m_appletPanel->radeApplet()) {
        applet->setRadeActive(true);
        applet->setRadeSynced(false);
        connect(m_radeEngine, &RADEEngine::syncChanged,
                applet, &RadeApplet::setRadeSynced);
        connect(m_radeEngine, &RADEEngine::snrChanged,
                applet, &RadeApplet::setRadeSnr);
        connect(m_radeEngine, &RADEEngine::freqOffsetChanged,
                applet, &RadeApplet::setRadeFreqOffset);
        connect(m_radeEngine, &RADEEngine::eooCallsignReceived,
                applet, &RadeApplet::setRadeCallsign, Qt::QueuedConnection);
    }

    // Store far-end callsign received in EOO frame for display / future use.
    connect(m_radeEngine, &RADEEngine::eooCallsignReceived,
            this, [this](const QString& callsign) {
                m_lastRadeRxCallsign = callsign;
                qCDebug(lcRade) << "RADE EOO callsign received:" << callsign;
            });

    // FreeDV Reporter: station reporting when the user has opted in.
    if (AppSettings::instance().value("FreeDvAutoReport", "False").toString() == "True")
        startFreeDvReporting(sliceId);

    qInfo() << "MainWindow: RADE mode activated on slice" << sliceId;
}

void MainWindow::deactivateRADE()
{
    // Capture slice ID before any field mutations below clear it.
    const int radeSliceId = m_radeSliceId;

    // Restore audio mute state on the RADE slice
    if (m_radeSliceId >= 0) {
        if (auto* s = m_radioModel.slice(m_radeSliceId)) {
            disconnect(s, &SliceModel::modeChanged,
                       this, &MainWindow::onRadeSliceModeChanged);
            s->setAudioMute(m_radePrevMute);
        }
        // Clear RADE status label and disconnect VFO signal connections before resetting sliceId.
        // Do this here (with m_radeSliceId still valid) rather than in the m_radeEngine block
        // below where the slice ID has already been cleared.
        if (auto* sw = spectrumForSlice(m_radioModel.slice(m_radeSliceId))) {
            if (auto* vfo = sw->vfoWidget(m_radeSliceId)) {
                vfo->setRadeActive(false);
                if (m_radeEngine) {
                    disconnect(m_radeEngine, &RADEEngine::syncChanged,         vfo, nullptr);
                    disconnect(m_radeEngine, &RADEEngine::snrChanged,           vfo, nullptr);
                    disconnect(m_radeEngine, &RADEEngine::freqOffsetChanged,    vfo, nullptr);
                    disconnect(m_radeEngine, &RADEEngine::eooCallsignReceived,  vfo, nullptr);
                }
            }
        }
        m_radeSliceId = -1;
    }

    m_radioModel.transmitModel().clearPttOffHook();
    disconnect(m_radeMoxFallbackConn);
    m_radeEooPending = false;
    m_radeTxActive = false;

    m_audio->setRadeMode(false);
    m_radioModel.setDigitalVoiceTxSlice(-1);
    m_audio->clearTxAccumulators();  // flush stale RADE modem data
    m_appletPanel->phoneCwApplet()->setRadeActive(false);

    if (auto* applet = m_appletPanel->radeApplet()) {
        applet->setRadeActive(false);
        if (m_radeEngine) {
            disconnect(m_radeEngine, &RADEEngine::syncChanged,        applet, nullptr);
            disconnect(m_radeEngine, &RADEEngine::snrChanged,         applet, nullptr);
            disconnect(m_radeEngine, &RADEEngine::freqOffsetChanged,  applet, nullptr);
            disconnect(m_radeEngine, &RADEEngine::eooCallsignReceived, applet, nullptr);
        }
    }

    // For hardware mics, reset to full gain — the radio controls hardware levels.
    // PC mic keeps its PcMicGain so SSB sessions are unaffected.
    if (m_radioModel.transmitModel().micSelection() != "PC") {
        m_audio->setPcMicGain(100);
    }
    m_lastRadeRxCallsign.clear();

    if (m_radeEngine) {
        disconnect(m_audio, &AudioEngine::txRawPcmReady,
                   m_radeEngine, nullptr);
        disconnect(m_radeEngine, &RADEEngine::txModemReady,
                   m_audio, nullptr);
        disconnect(m_radioModel.panStream(), &PanadapterStream::daxAudioReady,
                   m_radeEngine, nullptr);
        disconnect(m_radeEngine, &RADEEngine::rxSpeechReady,
                   m_audio, nullptr);
        disconnect(m_radeEngine, &RADEEngine::eooFinished,
                   this, nullptr);
        disconnect(m_radeEngine, &RADEEngine::eooCallsignReceived,
                   this, nullptr);
        if (m_radeDaxStreamId) {
            // Only send stream remove if TCI has no active clients. If TCI is
            // connected it may have borrowed this stream — removing it would
            // silently kill TCI audio. Leave TCI responsible for cleanup in
            // that case. TODO: replace with proper ref-counting in PanadapterStream
            // so any creator/borrower can safely release independently (#stream-lifecycle).
            bool tciActive = tciServer() && tciServer()->clientCount() > 0;
            bool daxBridgeActive = false;
#if defined(Q_OS_MAC) || defined(HAVE_PIPEWIRE)
            daxBridgeActive = (m_daxBridge != nullptr);
#endif
            if (!tciActive && !daxBridgeActive) {
                m_radioModel.panStream()->unregisterDaxStream(m_radeDaxStreamId);
                if (m_radioModel.isConnected()) {
                    m_radioModel.sendCommand(
                        QString("stream remove 0x%1")
                            .arg(m_radeDaxStreamId, 8, 16, QChar('0')));
                }
            }
            m_radeDaxStreamId = 0;
        }
        disconnect(m_radeDaxStreamConn);
        // Stop on the worker thread, then shut down the thread
        QMetaObject::invokeMethod(m_radeEngine, &RADEEngine::stop,
                                  Qt::BlockingQueuedConnection);
        if (m_radeThread) {
            m_radeThread->quit();
            m_radeThread->wait(2000);
            m_radeThread->deleteLater();
            m_radeThread = nullptr;
        }
        m_radeEngine = nullptr;  // deleteLater handles actual deletion
    }

    stopFreeDvReporting(radeSliceId);

    qInfo() << "MainWindow: RADE mode deactivated";
}

void MainWindow::onRadeSliceModeChanged(const QString& mode)
{
    // Fired when the RADE slice's mode changes via any path — TCI, profile
    // load, remote SmartSDR client. RADE requires DIGU or DIGL; deactivate
    // if the mode leaves that family so audio_mute is always restored.
    if (mode != "DIGU" && mode != "DIGL")
        deactivateRADE();
}

void MainWindow::activateFdvDisplay(int sliceId)
{
    if (m_fdvDisplaySliceId == sliceId)
        return;

    if (m_fdvDisplaySliceId >= 0)
        deactivateFdvDisplay();

    m_fdvDisplaySliceId = sliceId;

    auto* s = m_radioModel.slice(sliceId);
    if (!s) return;

    if (auto* sw = spectrumForSlice(s)) {
        if (auto* vfo = sw->vfoWidget(sliceId)) {
            vfo->setRadeActive(true, QStringLiteral("FreeDV"));
            vfo->setRadeSynced(false);
        }
    }

#ifdef HAVE_RADE
    if (auto* applet = m_appletPanel->radeApplet()) {
        applet->setRadeActive(true, QStringLiteral("FreeDV"));
        applet->setRadeSynced(false);
    }
#endif

    m_fdvSnrMeterIndex = m_radioModel.meterModel()
                             .findMeter("EXT_WVF", "FreeDV_SNR");

    connect(&m_radioModel.meterModel(), &MeterModel::meterUpdated,
            this, &MainWindow::onFdvMeterUpdated,
            Qt::UniqueConnection);
    connect(&m_radioModel, &RadioModel::metersChanged,
            this, &MainWindow::onFdvMetersChanged,
            Qt::UniqueConnection);

    m_fdvSynced = false;
    qCInfo(lcGui) << "MainWindow: FreeDV display activated on slice" << sliceId;
}

void MainWindow::deactivateFdvDisplay()
{
    if (m_fdvDisplaySliceId < 0)
        return;

    if (auto* s = m_radioModel.slice(m_fdvDisplaySliceId)) {
        if (auto* sw = spectrumForSlice(s)) {
            if (auto* vfo = sw->vfoWidget(m_fdvDisplaySliceId))
                vfo->setRadeActive(false);
        }
    }

#ifdef HAVE_RADE
    if (auto* applet = m_appletPanel->radeApplet())
        applet->setRadeActive(false);
#endif

    disconnect(&m_radioModel.meterModel(), &MeterModel::meterUpdated,
               this, &MainWindow::onFdvMeterUpdated);
    disconnect(&m_radioModel, &RadioModel::metersChanged,
               this, &MainWindow::onFdvMetersChanged);

    m_fdvDisplaySliceId = -1;
    m_fdvSnrMeterIndex  = -1;
    m_fdvSynced         = false;
    qCInfo(lcGui) << "MainWindow: FreeDV display deactivated";
}

void MainWindow::onFdvMeterUpdated(int index, float value)
{
    if (index != m_fdvSnrMeterIndex || m_fdvDisplaySliceId < 0)
        return;

    auto* s = m_radioModel.slice(m_fdvDisplaySliceId);
    if (!s) return;
    auto* sw = spectrumForSlice(s);
    if (!sw) return;
    auto* vfo = sw->vfoWidget(m_fdvDisplaySliceId);
    if (!vfo) return;

    const bool synced = (value > -98.9f);
    if (synced != m_fdvSynced) {
        m_fdvSynced = synced;
        vfo->setRadeSynced(synced);
#ifdef HAVE_RADE
        if (auto* applet = m_appletPanel->radeApplet())
            applet->setRadeSynced(synced);
#endif
    }
    if (synced) {
        vfo->setRadeSnr(value);
#ifdef HAVE_RADE
        if (auto* applet = m_appletPanel->radeApplet())
            applet->setRadeSnr(value);
#endif
    }
}

void MainWindow::onFdvMetersChanged()
{
    m_fdvSnrMeterIndex = m_radioModel.meterModel()
                             .findMeter("EXT_WVF", "FreeDV_SNR");
}

void MainWindow::startFreeDvReporting(int sliceId)
{
#ifndef HAVE_WEBSOCKETS
    // RADE without WebSockets: reporter client doesn't exist, no-op. (#2204)
    Q_UNUSED(sliceId);
#else
    if (!m_freedvClient) return;

    auto& cs = AppSettings::instance();

    // Callsign: prefer radio-stored value if "Use radio" is set and it's populated.
    QString callsign;
    if (cs.value("FreeDvUseRadioCallsign", "True").toString() == "True"
            && !m_radioModel.callsign().isEmpty()) {
        callsign = m_radioModel.callsign();
    } else {
        callsign = cs.value("FreeDvMyCallsign", "").toString().trimmed().toUpper();
    }

    // Grid: GPS (if hardware present, locked, and user prefers it), else user-saved.
    QString grid;
    if (cs.value("FreeDvUseGpsGrid", "True").toString() == "True"
            && m_radioModel.hasGpsHardware()
            && !m_radioModel.gpsGrid().isEmpty()) {
        grid = m_radioModel.gpsGrid();
    } else {
        grid = cs.value("FreeDvMyGrid", "").toString().trimmed().toUpper();
    }

    // Refuse to broadcast placeholder data to the public FreeDV Reporter
    // map — the dialog already validates this when the user toggles the
    // checkbox, but RADE auto-activation can hit this path without going
    // through the toggle, so guard here too.
    if (callsign.isEmpty() || grid.isEmpty()) {
        qCWarning(lcDxCluster)
            << "FreeDvReporting: refusing to enable — callsign or grid empty"
            << "(callsign='" << callsign << "', grid='" << grid << "')";
        return;
    }

    const QString message = cs.value("FreeDvMyMessage", "").toString();
    const QString swVer   = QString("AetherSDR %1").arg(QCoreApplication::applicationVersion());
    const double  freqMhz = m_radioModel.slice(sliceId)
                            ? m_radioModel.slice(sliceId)->frequency() : 0.0;

    // Auto-start the WebSocket connection if not already running — reporting is
    // independent of the user's FreeDV spot subscription.
    if (!m_freedvClient->isConnected())
        QMetaObject::invokeMethod(m_freedvClient, [this] { m_freedvClient->startConnection(); });

    QMetaObject::invokeMethod(m_freedvClient,
        [this, callsign, grid, message, swVer, freqMhz] {
            m_freedvClient->enableReporting(callsign, grid, message, swVer, freqMhz);
        });

    // TX report: evaluate tune/ATU guard on the main thread so we never report
    // a tune or ATU cycle as a voice transmission.
    m_freedvMoxConn = connect(&m_radioModel.transmitModel(), &TransmitModel::moxChanged,
            this, [this](bool tx) {
                const TransmitModel& txm = m_radioModel.transmitModel();
                if (txm.isTuning() || txm.atuStatus() == ATUStatus::InProgress)
                    return;
                QMetaObject::invokeMethod(m_freedvClient, [this, tx] {
                    m_freedvClient->reportTxState(tx);
                });
            });

    if (m_radeEngine) {
        connect(m_radeEngine, &RADEEngine::snrChanged,
                m_freedvClient, &FreeDvClient::updateRxSnr, Qt::QueuedConnection);
        connect(m_radeEngine, &RADEEngine::syncChanged,
                m_freedvClient, &FreeDvClient::updateRxSynced, Qt::QueuedConnection);
        connect(m_radeEngine, &RADEEngine::eooCallsignReceived,
                m_freedvClient, &FreeDvClient::updateRxCallsign, Qt::QueuedConnection);
    }
    if (SliceModel* radeSlice = m_radioModel.slice(sliceId)) {
        connect(radeSlice, &SliceModel::frequencyChanged,
                m_freedvClient, &FreeDvClient::reportFreqChange, Qt::QueuedConnection);
    }
#endif  // HAVE_WEBSOCKETS
}

void MainWindow::stopFreeDvReporting(int sliceId)
{
#ifndef HAVE_WEBSOCKETS
    Q_UNUSED(sliceId);
#else
    if (!m_freedvClient) return;

    disconnect(m_freedvMoxConn);
    if (m_radeEngine) {
        disconnect(m_radeEngine, &RADEEngine::snrChanged,           m_freedvClient, nullptr);
        disconnect(m_radeEngine, &RADEEngine::syncChanged,          m_freedvClient, nullptr);
        disconnect(m_radeEngine, &RADEEngine::eooCallsignReceived,  m_freedvClient, nullptr);
    }
    if (auto* radeSlice = m_radioModel.slice(sliceId))
        disconnect(radeSlice, &SliceModel::frequencyChanged, m_freedvClient, nullptr);

    QMetaObject::invokeMethod(m_freedvClient, [this] { m_freedvClient->disableReporting(); });
#endif
}

#endif  // HAVE_RADE

#ifdef HAVE_WEBSOCKETS
void MainWindow::showFreeDvReporter()
{
    if (!m_freedvReporterDialog) {
        m_freedvReporterDialog = new FreeDvReporterDialog(this);
        connect(m_freedvClient, &FreeDvClient::stationsCleared,
                m_freedvReporterDialog, &FreeDvReporterDialog::onStationsCleared,
                Qt::QueuedConnection);
        connect(m_freedvClient, &FreeDvClient::stationUpdated,
                m_freedvReporterDialog, &FreeDvReporterDialog::onStationUpdated,
                Qt::QueuedConnection);
        connect(m_freedvClient, &FreeDvClient::stationRemoved,
                m_freedvReporterDialog, &FreeDvReporterDialog::onStationRemoved,
                Qt::QueuedConnection);
        if (auto* s = activeSlice())
            m_freedvReporterDialog->setActiveSlice(s);
        // Seed with current state — bulk_update fires at connect time, before the
        // dialog exists. Without this, the table fills slowly from live events only.
        for (const auto& [sid, info] : m_freedvClient->stations().asKeyValueRange())
            m_freedvReporterDialog->onStationUpdated(sid, info);
    }
    // Resolve GPS-aware grid every open — same logic as startFreeDvReporting()
    // so km/Hdg columns work when GPS grid is active and never written to AppSettings.
    {
        auto& cs = AppSettings::instance();
        QString grid;
        if (cs.value("FreeDvUseGpsGrid", "True").toString() == "True"
                && m_radioModel.hasGpsHardware()
                && !m_radioModel.gpsGrid().isEmpty()) {
            grid = m_radioModel.gpsGrid();
        } else {
            grid = cs.value("FreeDvMyGrid", "").toString().trimmed().toUpper();
        }
        if (grid.isEmpty())
            grid = m_freedvClient->myGrid();
        m_freedvReporterDialog->setMyGrid(grid);
    }
    m_freedvReporterDialog->show();
    m_freedvReporterDialog->raise();
    m_freedvReporterDialog->activateWindow();
}
#endif

#if defined(Q_OS_MAC) || defined(HAVE_PIPEWIRE)
bool MainWindow::startDax()
{
    if (m_daxBridge) return true;

#ifdef Q_OS_MAC
    // Only start if the macOS HAL driver bundle is installed.
    if (!macDaxDriverInstalled()) {
        qWarning() << "MainWindow: DAX HAL plugin not installed";
        QMessageBox::warning(this, "DAX Audio Driver Missing",
            "The AetherSDR DAX audio driver is not installed on this Mac.\n\n"
            "Install the DAX Virtual Audio Driver from the AetherSDR DMG package, "
            "then enable DAX again.");
        return false;
    }
#endif

    m_daxBridge = new DaxBridge(this);
    if (!m_daxBridge->open()) {
        qWarning() << "MainWindow: failed to open DAX audio bridge";
        QMessageBox::warning(this, "DAX Audio Bridge Error",
            "AetherSDR could not open the DAX audio bridge.\n\n"
            "If the DAX driver was just installed, quit and relaunch AetherSDR and try again.");
        delete m_daxBridge;
        m_daxBridge = nullptr;
        return false;
    }

    // Listen for DAX stream status messages to register them in PanadapterStream.
    // The radio sends "stream 0xNNNNNNNN type=dax_rx dax_channel=N" status lines
    // in response to our stream create commands.
    connect(&m_radioModel, &RadioModel::statusReceived,
            m_daxBridge, [this](const QString& obj, const QMap<QString,QString>& kvs) {
        if (!obj.startsWith("stream ")) return;
        const QStringList parts = obj.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (parts.size() < 2) return;
        bool ok = false;
        quint32 streamId = parts[1].toUInt(&ok, 0);
        if (!ok) return;
        const bool removed = parts.contains(QStringLiteral("removed")) || kvs.contains(QStringLiteral("removed"));
        if (removed) {
            m_radioModel.panStream()->unregisterDaxStream(streamId);
            qCDebug(lcDax) << "MainWindow: unregistered removed DAX RX stream"
                           << "0x" + QString::number(streamId, 16);
            return;
        }
        QString type = kvs.value("type");
        if (type == "dax_rx") {
            if (!streamStatusBelongsToUs(kvs, m_radioModel.ourClientHandle())) {
                qCDebug(lcDax) << "MainWindow: ignoring DAX RX stream for another client"
                                << "stream=0x" + QString::number(streamId, 16)
                                << "owner=" << kvs.value("client_handle");
                return;
            }
            int ch = kvs.value("dax_channel").toInt();
            if (streamId && ch >= 1 && ch <= 4) {
                m_radioModel.panStream()->registerDaxStream(streamId, ch);
                qCDebug(lcDax) << "MainWindow: registered DAX RX ch" << ch
                                << "stream=0x" + QString::number(streamId, 16);
            }
        }
    });

    // Create DAX RX streams only for channels with slices assigned.
    // FlexLib creates streams on demand, not all 4 unconditionally.
    // Creating unused streams causes the radio to round-robin audio
    // across all of them, starving the active channels.
    m_daxSliceLastCh.clear();
    for (auto* s : m_radioModel.slices()) {
        int ch = s->daxChannel();
        m_daxSliceLastCh[s->sliceId()] = ch;
        if (ch >= 1 && ch <= 4) {
            m_radioModel.sendCommand(
                QString("stream create type=dax_rx dax_channel=%1").arg(ch));
        }
    }

    // #2895: the one-shot loop above only covers slices that ALREADY have a
    // DAX channel at bridge startup (typically just slice 0 / DAX 1). When the
    // user later assigns DAX 2-4 to another slice via the UI, SliceModel only
    // sends `slice set <id> dax=<ch>` — it never sends `stream create
    // type=dax_rx`, so the radio never registers a DAX client (dax_clients
    // stays 0) and sends silence. React to per-slice channel changes here and
    // create/remove the DAX RX stream on demand, mirroring the TCI path
    // (TciServer::ensureDaxForTci, #1331/#1439) and FlexLib
    // RequestDAXRXAudioStream(channel).
    for (auto* s : m_radioModel.slices()) {
        wireDaxSlice(s);
    }
    m_daxSliceConns.append(connect(&m_radioModel, &RadioModel::sliceAdded,
                                   this, [this](SliceModel* s) {
        if (!m_daxBridge || !s) return;
        // Let onDaxChannelChanged() see a 0 -> channel transition for slices
        // restored with DAX already assigned.
        m_daxSliceLastCh[s->sliceId()] = 0;
        wireDaxSlice(s);
        // A slice can arrive already carrying a DAX channel (radio profile
        // restore); make sure its stream exists too.
        if (s->daxChannel() >= 1 && s->daxChannel() <= 4) {
            onDaxChannelChanged(s, s->daxChannel());
        }
    }));

    // Wire DAX RX: PanadapterStream routes registered DAX streams here
    connect(m_radioModel.panStream(), &PanadapterStream::daxAudioReady,
            m_daxBridge, &DaxBridge::feedDaxAudio);

    // ── DAX IQ stream status + VITA routing ─────────────────────────────
    connect(&m_radioModel, &RadioModel::statusReceived,
            this, [this](const QString& obj, const QMap<QString,QString>& kvs) {
        if (!obj.startsWith("stream ")) return;
        const QStringList parts = obj.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (parts.size() < 2) return;
        bool ok = false;
        quint32 streamId = parts[1].toUInt(&ok, 0);
        if (!ok) return;
        const bool removed = parts.contains(QStringLiteral("removed")) || kvs.contains(QStringLiteral("removed"));
        if (removed) {
            m_radioModel.panStream()->unregisterIqStream(streamId);
            m_radioModel.daxIqModel().handleStreamRemoved(streamId);
            qCDebug(lcDax) << "MainWindow: unregistered removed DAX IQ stream"
                           << "0x" + QString::number(streamId, 16);
            return;
        }
        QString type = kvs.value("type");
        if (type == "dax_iq") {
            if (!streamStatusBelongsToUs(kvs, m_radioModel.ourClientHandle())) {
                qCDebug(lcDax) << "MainWindow: ignoring DAX IQ stream for another client"
                                << "stream=0x" + QString::number(streamId, 16)
                                << "owner=" << kvs.value("client_handle");
                return;
            }
            qCDebug(lcDax) << "MainWindow: DAX IQ stream status" << obj
                           << "keys=" << kvs.keys()
                           << "ch=" << kvs.value("daxiq_channel")
                           << "ip=" << kvs.value("ip");
            m_radioModel.daxIqModel().applyStreamStatus(streamId, kvs);
            int ch = kvs.value("daxiq_channel").toInt();
            if (streamId && ch >= 1 && ch <= 4)
                m_radioModel.panStream()->registerIqStream(streamId, ch);
        }
    });

    // Route IQ VITA-49 packets to DaxIqModel worker thread
    connect(m_radioModel.panStream(), &PanadapterStream::iqDataReady,
            &m_radioModel.daxIqModel(), &DaxIqModel::feedRawIqPacket);

    // Wire DAX IQ level meters to DAX IQ applet
    connect(&m_radioModel.daxIqModel(), &DaxIqModel::iqLevelReady,
            m_appletPanel->daxIqApplet(), &DaxIqApplet::setDaxIqLevel);

    // Wire DAX IQ enable/disable/rate from DAX IQ applet to DaxIqModel
    connect(m_appletPanel->daxIqApplet(), &DaxIqApplet::iqEnableRequested,
            &m_radioModel.daxIqModel(), &DaxIqModel::createStream);
    connect(m_appletPanel->daxIqApplet(), &DaxIqApplet::iqDisableRequested,
            &m_radioModel.daxIqModel(), &DaxIqModel::removeStream);
    connect(m_appletPanel->daxIqApplet(), &DaxIqApplet::iqRateChanged,
            &m_radioModel.daxIqModel(), &DaxIqModel::setSampleRate);

    // Wire DAX level meters
    connect(m_daxBridge, &DaxBridge::daxRxLevel,
            m_appletPanel->daxApplet(), &DaxApplet::setDaxRxLevel);
    connect(m_daxBridge, &DaxBridge::daxTxLevel,
            m_appletPanel->daxApplet(), &DaxApplet::setDaxTxLevel);

    // Wire DAX gain sliders
    connect(m_appletPanel->daxApplet(), &DaxApplet::daxRxGainChanged,
            m_daxBridge, &DaxBridge::setChannelGain);
    connect(m_appletPanel->daxApplet(), &DaxApplet::daxTxGainChanged,
            m_daxBridge, &DaxBridge::setTxGain);

    // Apply saved gains to the bridge
    auto& ss = AppSettings::instance();
    for (int i = 1; i <= 4; ++i)
        m_daxBridge->setChannelGain(i, ss.value(QStringLiteral("DaxRxGain%1").arg(i), "0.5").toString().toFloat());
    m_daxBridge->setTxGain(ss.value("DaxTxGain", "0.5").toString().toFloat());

    // Wire DAX TX: apps → bridge → AudioEngine → VITA-49.
    // AudioEngine chooses packet format/routing based on DaxTxLowLatency.
    connect(m_daxBridge, &DaxBridge::txAudioReady,
            this, [this](const QByteArray& pcm) {
        if (m_audio->isRadeMode()) return;
        if (!m_audio->isDaxTxMode()) return;
        QMetaObject::invokeMethod(m_audio, [this, pcm]() { m_audio->feedDaxTxAudio(pcm); });
    });

    // Save current mic selection before forcing PC audio source.
    m_savedMicSelection = m_radioModel.transmitModel().micSelection();

    // Default to the radio-native DAX route (dax=1, int16 mono).  RADE
    // mode overrides this to the low-latency route via setRadeMode()
    // when the user enters RADE — see AudioEngine::setRadeMode().
    m_audio->setDaxTxUseRadioRoute(true);
    m_radioModel.ensureDaxTxStream(DaxTxRequestReason::HostedDaxBridge);
    m_radioModel.sendCommand("transmit set mic_selection=PC");
    // Don't force dax=1 here — radio-side DAX flag follows mode changes
    // via updateDaxTxMode(). Bridge up ≠ DAX TX active. (#534)

    qInfo() << "MainWindow: starting DAX audio bridge";
    return true;
}

void MainWindow::stopDax()
{
    if (!m_daxBridge) return;

    m_audio->setDaxTxMode(false);
    m_audio->clearTxAccumulators();

    // #2895: drop the per-slice daxChannelChanged / sliceAdded reactions wired
    // in startDax() so they don't fire against a torn-down bridge.
    for (const auto& c : m_daxSliceConns) {
        disconnect(c);
    }
    m_daxSliceConns.clear();
    m_daxSliceLastCh.clear();

    disconnect(m_radioModel.panStream(), &PanadapterStream::daxAudioReady,
               m_daxBridge, nullptr);
    disconnect(m_daxBridge, &DaxBridge::txAudioReady,
               this, nullptr);

    // Remove DAX RX streams from the radio — but only channels no other
    // consumer still needs. TCI borrows bridge-created streams (#1331/#1439)
    // and RADE may hold one; unconditionally removing every registered stream
    // here silenced WSJT-X / RADE the instant the DAX bridge (or mute) was
    // toggled off — the #3363 / #2886 failure. Mirror the ownership guard in
    // onDaxChannelChanged(); full refcounting is tracked in #3305.
    auto* ps = m_radioModel.panStream();
    for (int ch = 1; ch <= 4; ++ch) {
        const quint32 id = ps->daxStreamIdForChannel(ch);
        if (id == 0) continue;
        const bool tciUsing = tciServer() && tciServer()->ownsDaxChannel(ch);
#ifdef HAVE_RADE
        const bool radeUsing = (id == m_radeDaxStreamId);
#else
        const bool radeUsing = false;
#endif
        if (tciUsing || radeUsing) {
            qCInfo(lcDax) << "MainWindow: keeping DAX RX stream"
                          << "0x" + QString::number(id, 16) << "for channel" << ch
                          << "— still used by" << (tciUsing ? "TCI" : "RADE") << "(#3363)";
            continue;
        }
        m_radioModel.sendCommand(QString("stream remove 0x%1").arg(id, 0, 16));
        ps->unregisterDaxStream(id);
    }

    // Restore original mic selection
    if (!m_savedMicSelection.isEmpty() && m_savedMicSelection != "PC")
        m_radioModel.sendCommand(QString("transmit set mic_selection=%1").arg(m_savedMicSelection));

    m_daxBridge->close();
    delete m_daxBridge;
    m_daxBridge = nullptr;
    qInfo() << "MainWindow: stopping DAX audio bridge";
}

// #2895: connect one slice's daxChannelChanged so a DAX channel (re)assigned
// after the bridge is already up still gets a radio-side DAX RX stream.
void MainWindow::wireDaxSlice(SliceModel* slice)
{
    if (!slice) return;
    m_daxSliceConns.append(connect(slice, &SliceModel::daxChannelChanged,
                                   this, [this, slice](int newCh) {
        if (!m_daxBridge) return;  // bridge torn down — ignore late signals
        onDaxChannelChanged(slice, newCh);
    }));
}

// Handle a slice's DAX channel transitioning. daxChannelChanged only fires on
// an actual value change (SliceModel guards equality), and arrives from both
// the local UI setter and the radio status echo — both are safe here because
// the stream create is made idempotent by the daxStreamIdForChannel() guard.
void MainWindow::onDaxChannelChanged(SliceModel* slice, int newCh)
{
    if (!slice || !m_daxBridge) return;
    auto* ps = m_radioModel.panStream();
    if (!ps) return;

    const int sliceId = slice->sliceId();
    const int oldCh = m_daxSliceLastCh.value(sliceId, 0);
    if (oldCh == newCh) return;
    m_daxSliceLastCh[sliceId] = newCh;

    // 0 -> 1..4 (or 1..4 -> different 1..4): ensure the new channel has a
    // radio-registered DAX RX stream. The stream status echo will register the
    // stream id in PanadapterStream via the dax_rx handler wired in startDax().
    if (newCh >= 1 && newCh <= 4) {
        if (ps->daxStreamIdForChannel(newCh) == 0) {
            m_radioModel.sendCommand(
                QString("stream create type=dax_rx dax_channel=%1").arg(newCh));
            qCInfo(lcDax) << "MainWindow: creating DAX RX stream for channel"
                          << newCh << "(slice" << sliceId << ", #2895)";
        }
        // Re-assert slice -> DAX mapping so the radio registers our stream as a
        // client. Without this dax_clients stays 0 and the radio sends silence
        // (the #1439 workaround, mirrored from TciServer::ensureDaxForTci).
        m_radioModel.sendCommand(
            QString("slice set %1 dax=%2").arg(sliceId).arg(newCh));
    }

    // <old>:1..4 -> now released or moved: remove the old channel's stream if
    // no other slice still references it.
    if (oldCh >= 1 && oldCh <= 4) {
        bool stillUsed = false;
        for (auto* s : m_radioModel.slices()) {
            if (s && s != slice && s->daxChannel() == oldCh) {
                stillUsed = true;
                break;
            }
        }
        if (!stillUsed) {
            const quint32 id = ps->daxStreamIdForChannel(oldCh);
            // Ownership guard: the PanadapterStream DAX map is shared across
            // every DAX consumer — the bridge, TCI (which borrows
            // bridge-created streams, #1331/#1439), and RADE. Removing a stream
            // another consumer is still using would silence WSJT-X or RADE
            // audio — the mirror image of #3270. Only tear down a stream that
            // no other consumer needs. (#2895)
            const bool tciUsing = tciServer() && tciServer()->ownsDaxChannel(oldCh);
#ifdef HAVE_RADE
            const bool radeUsing = (id != 0 && id == m_radeDaxStreamId);
#else
            const bool radeUsing = false;
#endif
            if (id != 0 && !tciUsing && !radeUsing) {
                m_radioModel.sendCommand(
                    QString("stream remove 0x%1").arg(id, 0, 16));
                ps->unregisterDaxStream(id);
                qCInfo(lcDax) << "MainWindow: removed DAX RX stream"
                              << "0x" + QString::number(id, 16)
                              << "for channel" << oldCh << "(#2895)";
            } else if (id != 0) {
                qCInfo(lcDax) << "MainWindow: keeping DAX RX stream"
                              << "0x" + QString::number(id, 16)
                              << "for channel" << oldCh
                              << "— still used by" << (tciUsing ? "TCI" : "RADE")
                              << "(#2895)";
            }
        }
    }
}
#endif

// registerMidiParams() lives in MainWindow_Controllers.cpp (#3351 Phase 1a).

} // namespace AetherSDR
