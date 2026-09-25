#include "AetherRxDialog.h"
#include "AetherDspWidget.h"
#include "AetherRxSettingsDialog.h"
#include "ClientEqApplet.h"   // ClientEqApplet::Path
#include "EditorFramelessTitleBar.h"
#include "CompactMetrics.h"
#include "ModemChrome.h"
#include "StagePage.h"
#include "StageTabBar.h"
#include "RxStageReorder.h"
#include "StripCompPanel.h"
#include "StripEqPanel.h"
#include "StripGatePanel.h"
#include "StripPuduPanel.h"
#include "StripRxOutputPanel.h"
#include "StripTubePanel.h"
#include "StripWaveformPanel.h"

#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QDrag>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFrame>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QHBoxLayout>
#include <QPushButton>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>
#include "core/AppSettings.h"
#include "core/AudioEngine.h"
#include "core/TxKeyingMarker.h"
#include "core/ClientComp.h"
#include "core/ClientEq.h"
#include "core/ClientGate.h"
#include "core/ClientPudu.h"
#include "core/ClientTube.h"
#include "core/ThemeManager.h"

namespace AetherSDR {

namespace {

// The size this window opens at, every time — see showEvent().
constexpr QSize kLaunchSize(720, 480);

// Stage <-> RxChainStage. Only five of the seven tabs are chain stages; the
// rest have no place in the order and are pinned at the ends.
bool isChainStage(AetherRxDialog::Stage s)
{
    return s == AetherRxDialog::Gate || s == AetherRxDialog::Eq
        || s == AetherRxDialog::Comp || s == AetherRxDialog::Tube
        || s == AetherRxDialog::Voice;
}

AudioEngine::RxChainStage toChainStage(AetherRxDialog::Stage s)
{
    switch (s) {
        case AetherRxDialog::Eq:    return AudioEngine::RxChainStage::Eq;
        case AetherRxDialog::Gate:  return AudioEngine::RxChainStage::Gate;
        case AetherRxDialog::Comp:  return AudioEngine::RxChainStage::Comp;
        case AetherRxDialog::Tube:  return AudioEngine::RxChainStage::Tube;
        case AetherRxDialog::Voice: return AudioEngine::RxChainStage::Pudu;
        default:                    return AudioEngine::RxChainStage::None;
    }
}

AetherRxDialog::Stage fromChainStage(AudioEngine::RxChainStage s)
{
    switch (s) {
        case AudioEngine::RxChainStage::Eq:   return AetherRxDialog::Eq;
        case AudioEngine::RxChainStage::Gate: return AetherRxDialog::Gate;
        case AudioEngine::RxChainStage::Comp: return AetherRxDialog::Comp;
        case AudioEngine::RxChainStage::Tube: return AetherRxDialog::Tube;
        case AudioEngine::RxChainStage::Pudu: return AetherRxDialog::Voice;
        default:                              return AetherRxDialog::StageCount;
    }
}

} // namespace

AetherRxDialog::AetherRxDialog(AudioEngine* audio, QWidget* parent)
    // The geometry key carries a version because the default size changed
    // after the window had shipped once: a stored geometry always wins over
    // resize(), so anyone who had opened it would have kept the old 1420x900
    // for ever and never seen the new default. Bumping the key retires those
    // saved rectangles; the window persists its size again from here.
    : PersistentDialog("AetherRX", "AetherRxDialogGeometry2", parent)
    , m_audio(audio)
{
    theme::setContainer(this, QStringLiteral("dialog/aetherRx"));
    AetherSDR::ThemeManager::instance().applyStyleSheet(this, "QDialog { background: {{color.background.0}}; color: {{color.text.primary}}; }");
    // Opens at a size that sits on the desktop rather than filling it. The EQ
    // and waveform pages are wider than this at their natural size and scroll
    // to fit; that is the trade for a window you can put somewhere. The
    // minimum is what the narrowest page (AetherNR's two-column radio row)
    // needs beside the tab column, and geometry persists, so a window resized
    // once reopens where it was left.
    setMinimumSize(600, 400);
    resize(kLaunchSize);

    auto* body = new QHBoxLayout(bodyWidget());
    body->setContentsMargins(8, 8, 8, 8);
    body->setSpacing(8);

    // -- Left tab column ------------------------------------------------
    m_tabs = new StageTabBar(QStringLiteral("aetherRx"), this);
    StageTabBar::Host host;
    host.isChainStage = [](int id) {
        return isChainStage(static_cast<Stage>(id));
    };
    host.stageEnabled = [this](int id) {
        return stageEnabled(static_cast<Stage>(id));
    };
    host.setStageEnabled = [this](int id, bool on) {
        setStageEnabled(static_cast<Stage>(id), on);
    };
    host.chainOrder = [this]() {
        QVector<int> ids;
        if (!m_audio) return ids;
        for (auto s : m_audio->rxChainStages()) {
            const Stage row = fromChainStage(s);
            if (row < StageCount) ids.append(static_cast<int>(row));
        }
        return ids;
    };
    host.commitChainOrder = [this](const QVector<int>& ids) {
        if (!m_audio) return;
        QVector<AudioEngine::RxChainStage> stages;
        stages.reserve(ids.size());
        for (int id : ids) {
            const auto s = toChainStage(static_cast<Stage>(id));
            if (s != AudioEngine::RxChainStage::None) stages.append(s);
        }
        // Persists on its own — setRxChainStages writes ClientRxChainStages.
        m_audio->setRxChainStages(stages);
    };
    m_tabs->setHost(host);
    body->addWidget(m_tabs);

    m_stack = new QStackedWidget(this);
    body->addWidget(m_stack, 1);

    connect(m_tabs, &StageTabBar::stageSelected,
            m_stack, &QStackedWidget::setCurrentIndex);
    connect(m_tabs, &StageTabBar::footerButtonClicked,
            this, &AetherRxDialog::showSettings);

    // -- AetherNR: the noise-reduction body, method strip and all --------
    m_widget = new AetherDspWidget(audio, this);
    m_widget->setDialogMode(true);
    addStage(Nr, QStringLiteral("AetherNR"), m_widget);

    // -- The RX chain, in the order the signal meets it ------------------
    m_gate = new StripGatePanel(audio, this);
    addStage(Gate, QStringLiteral("Gate"), buildStagePage(m_gate));

    m_eq = new StripEqPanel(audio, this);
    // Straight through: the panel asks, MainWindow decides what a width means
    // for the mode the slice is in.
    connect(m_eq, &StripEqPanel::rxFilterWidthRequested,
            this, &AetherRxDialog::rxFilterWidthRequested);
    connect(m_eq, &StripEqPanel::cutoffsDragRequested,
            this, &AetherRxDialog::cutoffsDragRequested);
    addStage(Eq, QStringLiteral("EQ"), buildStagePage(m_eq));

    m_comp = new StripCompPanel(audio, this);
    addStage(Comp, QStringLiteral("Compressor"), buildStagePage(m_comp));

    m_tube = new StripTubePanel(audio, this);
    addStage(Tube, QStringLiteral("Tube"), buildStagePage(m_tube));

    m_voice = new StripPuduPanel(audio, this);
    addStage(Voice, QStringLiteral("Exciter"), buildStagePage(m_voice));

    // Output and waveform share the last tab: the meter is what you read and
    // the waveform is what you read it against, so splitting them would mean
    // switching tabs to answer one question.
    {
        auto* page = new QWidget;
        auto* col = new QVBoxLayout(page);
        col->setContentsMargins(0, 0, 0, 0);
        col->setSpacing(8);
        m_output = new StripRxOutputPanel(audio, this);
        m_waveform = new StripWaveformPanel(audio, this);
        col->addWidget(m_output, 0, Qt::AlignTop);
        col->addWidget(m_waveform, 1);
        addStage(Output, QStringLiteral("Final Output"), page);
    }

    // REC / PLAY on one row above BYPASS, as in AetherTX -- but recording
    // the receive audio itself, through the same path as the VFO flag's
    // record and play buttons. A click reports the state the button now
    // shows; MainWindow answers with what the recorder actually did (a
    // capture can be refused: PC audio off, unwritable directory), so the
    // button follows the recorder rather than the click.
    const auto pair = m_tabs->addFooterToggleRow({
        {tr("REC"), QStringLiteral("aetherRxRecord"),
         tr("Record the receive audio to a WAV file, as the VFO flag's record "
            "button does. Click again to stop."),
         StageTabBar::Accent::Red},
        {tr("PLAY"), QStringLiteral("aetherRxPlay"),
         tr("Play back the last recording. Click again to stop."),
         StageTabBar::Accent::Green},
    });
    m_recBtn  = pair.at(0);
    m_playBtn = pair.at(1);
    // The short labels fit the row; the spoken names stay whole words
    // (#4896).
    m_recBtn->setAccessibleName(tr("Record"));
    m_playBtn->setAccessibleName(tr("Play"));
    connect(m_recBtn, &QPushButton::clicked, this, [this](bool checked) {
        emit recordToggled(checked);
    });
    connect(m_playBtn, &QPushButton::clicked, this, [this](bool checked) {
        if (m_txPlaybackActive) {
            // PLAY remains enabled to stop an active transmission. A normal
            // click must never start speaker playback while TX owns the file.
            QSignalBlocker block(m_playBtn);
            m_playBtn->setChecked(false);
            emit txPlaybackTriggered();
            return;
        }
        emit playToggled(checked);
    });
    setPlayEnabled(false);

    // Right-click on PLAY: one entry, "TX Playback", which sends the last
    // recording out over the air instead of to the speakers. A keying
    // control, so it carries the marker the automation bridge refuses to
    // invoke without AETHER_AUTOMATION_ALLOW_TX; MainWindow registers the
    // real keying action on it once the radio is known.
    m_txPlaybackAction = new QAction(tr("TX Playback"), this);
    m_txPlaybackAction->setObjectName(QStringLiteral("aetherRxTxPlayback"));
    m_txPlaybackAction->setCheckable(true);
    m_txPlaybackAction->setToolTip(
        tr("Transmit the last recording over the active slice. Select again "
           "to stop."));
    m_txPlaybackAction->setProperty(kTxKeyingProperty, true);
    connect(m_txPlaybackAction, &QAction::triggered, this, [this] {
        // A checkable action flips itself; the host says what really
        // happened through setTxPlaybackActive.
        QSignalBlocker block(m_txPlaybackAction);
        m_txPlaybackAction->setChecked(m_txPlaybackActive);
        emit txPlaybackTriggered();
    });
    m_playBtn->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_playBtn, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        // Reachable whenever there is something to send or something to
        // stop: setPlayEnabled() keeps the button enabled while a transmit
        // playback runs, so this handler can still open the menu to stop it.
        m_txPlaybackAction->setEnabled(m_playEnabled || m_txPlaybackActive);
        QMenu menu(m_playBtn);
        menu.setObjectName(QStringLiteral("aetherRxPlayMenu"));
        menu.addAction(m_txPlaybackAction);
        menu.exec(m_playBtn->mapToGlobal(pos));
    });

    // BYPASS beside the Settings gear, where AetherTX keeps its own. The
    // engine owns the snapshot-and-restore, and the docked chain applet's RX
    // BYPASS drives the same state, so this button follows the engine back
    // rather than remembering anything itself. The engine takes AetherNR
    // down with the chain, so a bypassed receive path is truly unprocessed.
    m_bypassBtn = m_tabs->addFooterToggle(
        tr("BYPASS"), QStringLiteral("aetherRxBypass"),
        tr("Suppress every receive stage at once, AetherNR included, so audio "
           "reaches you unprocessed. Click again to restore what was on."));
    connect(m_bypassBtn, &QPushButton::toggled, this, [this](bool on) {
        if (m_audio) m_audio->setRxBypassed(on);
    });
    if (m_audio) {
        {
            QSignalBlocker block(m_bypassBtn);
            m_bypassBtn->setChecked(m_audio->isRxBypassed());
        }
        connect(m_audio, &AudioEngine::rxBypassChanged, this, [this](bool on) {
            if (!m_bypassBtn) return;
            QSignalBlocker block(m_bypassBtn);
            m_bypassBtn->setChecked(on);
        });
    }

    // The gear leads BYPASS's row.
    m_tabs->addFooterGearButton(
        tr("Settings"), QStringLiteral("aetherRxSettingsButton"),
        tr("Settings: save, load, import and export receive chain profiles."));

    // Pin every panel to its RX engine instance. Without this the EQ canvas
    // has no engine to enumerate bands for and collapses to its "(no EQ
    // connected)" placeholder.
    if (m_gate)     m_gate->showForRx();
    if (m_eq)       m_eq->showForPath(ClientEqApplet::Path::Rx);
    if (m_comp)     m_comp->showForRx();
    if (m_tube)     m_tube->showForRx();
    if (m_voice)    m_voice->showForRx();
    if (m_output)   m_output->showForRx();
    if (m_waveform) m_waveform->showForRx();

    // One panel to a page: the tab already names it, so the plate comes off.
    for (QWidget* panel : {static_cast<QWidget*>(m_gate),
                           static_cast<QWidget*>(m_eq),
                           static_cast<QWidget*>(m_comp),
                           static_cast<QWidget*>(m_tube),
                           static_cast<QWidget*>(m_voice)}) {
        tidyEmbeddedPanel(panel);
    }
    // The Out page stacks two, and their plates are what say which is which.
    tidyEmbeddedPanel(m_output, /*keepTitle=*/true);
    tidyEmbeddedPanel(m_waveform, /*keepTitle=*/true);

    m_tabs->setCurrentStage(Nr);
    m_stack->setCurrentIndex(Nr);

    // The Client* stages are plain classes with no change signal, and the RX
    // chain strip toggles the same flags from outside this window, so the
    // boxes are polled rather than driven. Five times a second is far below
    // what a human notices and nowhere near what six bool reads cost.
    m_tabs->refreshFromHost();
    m_checkTimer = new QTimer(this);
    m_checkTimer->setInterval(200);
    connect(m_checkTimer, &QTimer::timeout, this, [this]() {
        m_tabs->refreshFromHost();
    });

    // Forward every parameter-change signal so existing connections to
    // AetherRxDialog::* keep working unchanged.
    connect(m_widget, &AetherDspWidget::nr2GainMaxChanged,
            this,    &AetherRxDialog::nr2GainMaxChanged);
    connect(m_widget, &AetherDspWidget::nr2GainFloorChanged,
            this,    &AetherRxDialog::nr2GainFloorChanged);
    connect(m_widget, &AetherDspWidget::nr2GainSmoothChanged,
            this,    &AetherRxDialog::nr2GainSmoothChanged);
    connect(m_widget, &AetherDspWidget::nr2QsppChanged,
            this,    &AetherRxDialog::nr2QsppChanged);
    connect(m_widget, &AetherDspWidget::nr2GainMethodChanged,
            this,    &AetherRxDialog::nr2GainMethodChanged);
    connect(m_widget, &AetherDspWidget::nr2NpeMethodChanged,
            this,    &AetherRxDialog::nr2NpeMethodChanged);
    connect(m_widget, &AetherDspWidget::nr2AeFilterChanged,
            this,    &AetherRxDialog::nr2AeFilterChanged);
    connect(m_widget, &AetherDspWidget::nr2Post2SettingsChanged,
            this,    &AetherRxDialog::nr2Post2SettingsChanged);
    connect(m_widget, &AetherDspWidget::mnrStrengthChanged,
            this,    &AetherRxDialog::mnrStrengthChanged);
    connect(m_widget, &AetherDspWidget::rn2DryMixChanged,
            this,    &AetherRxDialog::rn2DryMixChanged);
    connect(m_widget, &AetherDspWidget::dfnrAttenLimitChanged,
            this,    &AetherRxDialog::dfnrAttenLimitChanged);
    connect(m_widget, &AetherDspWidget::dfnrPostFilterBetaChanged,
            this,    &AetherRxDialog::dfnrPostFilterBetaChanged);
    connect(m_widget, &AetherDspWidget::nr4ReductionChanged,
            this,    &AetherRxDialog::nr4ReductionChanged);
    connect(m_widget, &AetherDspWidget::nr4SmoothingChanged,
            this,    &AetherRxDialog::nr4SmoothingChanged);
    connect(m_widget, &AetherDspWidget::nr4WhiteningChanged,
            this,    &AetherRxDialog::nr4WhiteningChanged);
    connect(m_widget, &AetherDspWidget::nr4AdaptiveNoiseChanged,
            this,    &AetherRxDialog::nr4AdaptiveNoiseChanged);
    connect(m_widget, &AetherDspWidget::nr4NoiseMethodChanged,
            this,    &AetherRxDialog::nr4NoiseMethodChanged);
    connect(m_widget, &AetherDspWidget::nr4MaskingDepthChanged,
            this,    &AetherRxDialog::nr4MaskingDepthChanged);
    connect(m_widget, &AetherDspWidget::nr4SuppressionChanged,
            this,    &AetherRxDialog::nr4SuppressionChanged);
}

// A stage panel that always fits, by shrinking its knobs and curves rather
// than its labels. No scrollbars, so a control is never half a drag off the
// edge of the page.
QWidget* AetherRxDialog::buildStagePage(QWidget* panel)
{
    return makeStagePage(panel);
}

void AetherRxDialog::addStage(Stage stage, const QString& label, QWidget* page)
{
    // Out is a meter and a waveform, not a stage: no checkbox, and the bar
    // gives it no grip either because it is not in the chain.
    m_tabs->addStage(static_cast<int>(stage), label,
                     /*wantsCheckbox=*/stage != Output);

    const int index = m_stack->addWidget(page);
    Q_ASSERT(index == stage);   // ids double as stack indices
    Q_UNUSED(index);
}

// Opens at kLaunchSize every time, whatever is stored.
//
// Deliberate and temporary: the size this window wants is still being settled,
// and a restored geometry wins over the default in the constructor, so without
// this the first size anyone happened to leave it at is the size they keep.
// The position is still restored and still saved — only the size is pinned.
// To give the size back to the operator, delete this override; the geometry
// key underneath it has been recording all along.
void AetherRxDialog::showEvent(QShowEvent* event)
{
    PersistentDialog::showEvent(event);
    if (size() != kLaunchSize) {
        resize(kLaunchSize);
    }
    m_tabs->refreshFromHost();
    if (m_checkTimer) m_checkTimer->start();
}

void AetherRxDialog::hideEvent(QHideEvent* event)
{
    PersistentDialog::hideEvent(event);
    if (m_checkTimer) m_checkTimer->stop();
}

bool AetherRxDialog::stageEnabled(Stage stage) const
{
    if (!m_audio) return false;
    switch (stage) {
        case Nr:
            // Client-side NR is exclusive: the stage is on when any one of
            // the seven methods is.
            return m_audio->nr2Enabled()  || m_audio->nr4Enabled()
                || m_audio->mnrEnabled()  || m_audio->dfnrEnabled()
                || m_audio->rn2Enabled()  || m_audio->nvAfxEnabled()
                || m_audio->nnrEnabled();
        case Gate:
            return m_audio->clientGateRx() && m_audio->clientGateRx()->isEnabled();
        case Eq:
            return m_audio->clientEqRx() && m_audio->clientEqRx()->isEnabled();
        case Comp:
            return m_audio->clientCompRx() && m_audio->clientCompRx()->isEnabled();
        case Tube:
            return m_audio->clientTubeRx() && m_audio->clientTubeRx()->isEnabled();
        case Voice:
            return m_audio->clientPuduRx() && m_audio->clientPuduRx()->isEnabled();
        default:
            return false;
    }
}

void AetherRxDialog::setStageEnabled(Stage stage, bool on)
{
    if (!m_audio) return;
    switch (stage) {
        case Nr: {
            // The same gesture the chain strip's DSP tile makes: switching the
            // stage off turns off whichever method is running, and switching it
            // back on restores the last one AetherDspWidget saved.
            if (!on) {
                QMetaObject::invokeMethod(m_audio, [audio = m_audio]() {
                    if (audio->nr2Enabled())    audio->setNr2Enabled(false);
                    if (audio->nr4Enabled())    audio->setNr4Enabled(false);
                    if (audio->mnrEnabled())    audio->setMnrEnabled(false);
                    if (audio->dfnrEnabled())   audio->setDfnrEnabled(false);
                    if (audio->rn2Enabled())    audio->setRn2Enabled(false);
                    if (audio->nvAfxEnabled())  audio->setNvAfxEnabled(false);
                    if (audio->nnrEnabled())    audio->setNnrEnabled(false);
                });
                break;
            }
            const QString name =
                AppSettings::instance().value("LastClientNr", "").toString();
            // Never been on, so there is nothing to restore; the next poll
            // puts the box back where it was.
            if (name.isEmpty()) break;
            if (name == QLatin1String("NR2")) {
                // NR2 needs the wisdom prep before the engine builds
                // SpectralNR — straight through the setter can crash the
                // next feedAudioData (#2275).
                emit nr2EnableWithWisdomRequested();
                break;
            }
            QMetaObject::invokeMethod(m_audio, [audio = m_audio, name]() {
                if (name == QLatin1String("NR4"))       audio->setNr4Enabled(true);
                else if (name == QLatin1String("MNR"))  audio->setMnrEnabled(true);
                else if (name == QLatin1String("DFNR")) audio->setDfnrEnabled(true);
                else if (name == QLatin1String("RN2"))  audio->setRn2Enabled(true);
                else if (name == QLatin1String("BNR"))  audio->setNvAfxEnabled(true);
                else if (name == QLatin1String("NNR"))  audio->setNnrEnabled(true);
            });
            break;
        }
        case Gate:
            if (auto* g = m_audio->clientGateRx()) {
                g->setEnabled(on);
                m_audio->saveClientGateRxSettings();
            }
            break;
        case Eq:
            if (auto* e = m_audio->clientEqRx()) {
                e->setEnabled(on);
                m_audio->saveClientEqSettings();
            }
            break;
        case Comp:
            if (auto* c = m_audio->clientCompRx()) {
                c->setEnabled(on);
                m_audio->saveClientCompRxSettings();
            }
            break;
        case Tube:
            if (auto* t = m_audio->clientTubeRx()) {
                t->setEnabled(on);
                m_audio->saveClientTubeRxSettings();
            }
            break;
        case Voice:
            if (auto* p = m_audio->clientPuduRx()) {
                p->setEnabled(on);
                m_audio->saveClientPuduRxSettings();
            }
            break;
        default:
            break;
    }
}





void AetherRxDialog::showSettings()
{
    // Modeless would let the operator load a profile while a stage page is
    // mid-drag; modal keeps the chain still while it is being rewritten.
    AetherRxSettingsDialog dlg(m_audio, this);
    connect(&dlg, &AetherRxSettingsDialog::profileApplied, this, [this]() {
        // A profile can reorder the chain and flip every enable, so the whole
        // window re-reads the engine rather than waiting for the poll.
        syncFromEngine();
        m_tabs->refreshFromHost();
    });
    dlg.exec();
}

void AetherRxDialog::setRecordOn(bool on)
{
    if (!m_recBtn) return;
    QSignalBlocker block(m_recBtn);
    m_recBtn->setChecked(on);
}

void AetherRxDialog::setPlayOn(bool on)
{
    if (!m_playBtn) return;
    QSignalBlocker block(m_playBtn);
    m_playBtn->setChecked(on);
}

void AetherRxDialog::setPlayEnabled(bool enabled)
{
    m_playEnabled = enabled;
    if (!m_playBtn) return;
    // A disabled widget gets no context-menu event, so the menu that stops
    // a transmit playback would vanish with it. While one is transmitting
    // the button stays enabled whatever the host says; the host's answer is
    // kept and applied once the transmit ends.
    const bool effective = enabled || m_txPlaybackActive;
    m_playBtn->setEnabled(effective);
    // Why it is greyed out has to reach the accessible channel too, not
    // just the tooltip (#4896).
    m_playBtn->setAccessibleDescription(
        effective ? tr("Play back the last recording. Click again to stop.")
                  : tr("Unavailable until something has been recorded. "
                       "Use REC first."));
}

void AetherRxDialog::setTxPlaybackActive(bool on)
{
    m_txPlaybackActive = on;
    if (m_txPlaybackAction) {
        QSignalBlocker block(m_txPlaybackAction);
        m_txPlaybackAction->setChecked(on);
    }
    setPlayEnabled(m_playEnabled);
}

void AetherRxDialog::syncFromEngine()
{
    if (m_widget) m_widget->syncFromEngine();
}

void AetherRxDialog::selectTab(const QString& name)
{
    // Both spellings resolve. These stages were AGC-G, AGC-C, AetherVoice and
    // Out until the tabs were renamed, and callers elsewhere — plus anything
    // driving this window through the automation bridge — still say those.
    static const struct { Stage stage; const char* label; } kStages[] = {
        {Nr, "AetherNR"},   {Gate, "Gate"},        {Eq, "EQ"},
        {Comp, "Compressor"}, {Tube, "Tube"},      {Voice, "Exciter"},
        {Output, "Final Output"},
        {Gate, "AGC-G"},    {Comp, "AGC-C"},      {Voice, "AetherVoice"},
        {Output, "Out"},
    };
    for (const auto& entry : kStages) {
        if (name.compare(QLatin1String(entry.label), Qt::CaseInsensitive) != 0) {
            continue;
        }
        m_tabs->setCurrentStage(entry.stage);
        if (m_stack) m_stack->setCurrentIndex(entry.stage);
        return;
    }

    // Not a stage name, so it is a noise-reduction method: show the NR tab and
    // let the body pick the method. Every caller that predates this window
    // passes one of these.
    m_tabs->setCurrentStage(Nr);
    if (m_stack) m_stack->setCurrentIndex(Nr);
    if (m_widget) m_widget->selectTab(name);
}

} // namespace AetherSDR
