#include "AetherialAudioStrip.h"

#include <QGuiApplication>
#include <QScreen>

#include "FramelessResizer.h"
#include "AetherTxSettingsDialog.h"
#include "StagePage.h"
#include "StageTabBar.h"
#include "ClientEqApplet.h"
#include "StripCompPanel.h"
#include "StripDeEssPanel.h"
#include "StripEqPanel.h"
#include "StripGatePanel.h"
#include "StripPuduPanel.h"
#include "StripReverbPanel.h"
#include "StripTubePanel.h"
#include "StripWaveformPanel.h"
#include "StripFinalOutputPanel.h"
#include "core/AppSettings.h"
#include "core/AudioEngine.h"
#include "core/ClientComp.h"
#include "core/ClientDeEss.h"
#include "core/ClientEq.h"
#include "core/ClientGate.h"
#include "core/ClientPudu.h"
#include "core/ClientReverb.h"
#include "core/ClientTube.h"

#include <QButtonGroup>
#include <QByteArray>
#include <QCloseEvent>
#include <QComboBox>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QEvent>
#include <QFrame>
#include <QGridLayout>
#include <QHideEvent>
#include <QShowEvent>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QVBoxLayout>
#include "core/ThemeManager.h"

namespace AetherSDR {

namespace {
// The size this window opens at, every time — see showEvent().
constexpr QSize kLaunchSize(720, 480);
}  // namespace

namespace {

// Stage <-> TxChainStage. Output is the meter page, not a chain stage.
AudioEngine::TxChainStage toChainStage(AetherialAudioStrip::Stage s)
{
    switch (s) {
        case AetherialAudioStrip::Gate:   return AudioEngine::TxChainStage::Gate;
        case AetherialAudioStrip::Eq:     return AudioEngine::TxChainStage::Eq;
        case AetherialAudioStrip::DeEss:  return AudioEngine::TxChainStage::DeEss;
        case AetherialAudioStrip::Comp:   return AudioEngine::TxChainStage::Comp;
        case AetherialAudioStrip::Tube:   return AudioEngine::TxChainStage::Tube;
        case AetherialAudioStrip::Enh:    return AudioEngine::TxChainStage::Enh;
        case AetherialAudioStrip::Reverb: return AudioEngine::TxChainStage::Reverb;
        default:                          return AudioEngine::TxChainStage::None;
    }
}

AetherialAudioStrip::Stage fromChainStage(AudioEngine::TxChainStage s)
{
    switch (s) {
        case AudioEngine::TxChainStage::Gate:   return AetherialAudioStrip::Gate;
        case AudioEngine::TxChainStage::Eq:     return AetherialAudioStrip::Eq;
        case AudioEngine::TxChainStage::DeEss:  return AetherialAudioStrip::DeEss;
        case AudioEngine::TxChainStage::Comp:   return AetherialAudioStrip::Comp;
        case AudioEngine::TxChainStage::Tube:   return AetherialAudioStrip::Tube;
        case AudioEngine::TxChainStage::Enh:    return AetherialAudioStrip::Enh;
        case AudioEngine::TxChainStage::Reverb: return AetherialAudioStrip::Reverb;
        default:                                return AetherialAudioStrip::StageCount;
    }
}

} // namespace

AetherialAudioStrip::AetherialAudioStrip(AudioEngine* engine, QWidget* parent)
    : CanonWindow(QStringLiteral("AetherTX"), parent, Kind::Workspace)
    , m_audio(engine)
{
    const QString title = QStringLiteral("AetherTX");
    theme::setContainer(this, QStringLiteral("canon/aetherTx"));
    // A workspace tool on the style guide's CanonWindow, like AetherRX: the
    // position persists under the key it used before (the saved geometry
    // carries over), and it resizes from every edge.
    setGeometryKey(QStringLiteral("AetherialStripGeometry2"));
    // One stage to a page, so the window does not have to be tall enough for
    // a nine-panel grid. Same size the receive window opens at, for the same
    // reason: it sits on the desktop rather than filling it.
    setMinimumSize(600, 400);
    resize(kLaunchSize);
    FramelessResizer::install(this);
    AetherSDR::ThemeManager::instance().applyStyleSheet(bodyWidget(), canonBodyStyleSheet());

    auto* outer = new QVBoxLayout(bodyWidget());
    outer->setContentsMargins(14, 12, 14, 14);
    outer->setSpacing(10);
    outer->addWidget(makeCanonHeader(title));

    auto* content = new QWidget(bodyWidget());
    auto* body = new QVBoxLayout(content);
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(8);
    outer->addWidget(content, 1);

    m_pcAudioNotice = new QLabel(content);
    m_pcAudioNotice->setObjectName(QStringLiteral("aetherTxPcAudioNotice"));
    m_pcAudioNotice->setAccessibleName(tr("AetherTX audio path guidance"));
    m_pcAudioNotice->setWordWrap(true);
    body->addWidget(m_pcAudioNotice);
    m_pcAudioNotice->hide();

    // The TX chain one stage per page, with the shared tab column on the left
    // (same component as AetherRX). REC/PLAY and BYPASS/Settings sit at the foot
    // of the column, unmodal for mid-QSO use; only the profile library is behind
    // the gear. ClientChainApplet carries its own copies on the docked panel.
    auto* row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);

    m_tabs = new StageTabBar(QStringLiteral("aetherTx"), content);
    StageTabBar::Host host;
    host.isChainStage = [](int id) {
        return static_cast<Stage>(id) != Output;
    };
    host.stageEnabled = [this](int id) { return stageEnabled(static_cast<Stage>(id)); };
    host.setStageEnabled = [this](int id, bool on) {
        setStageEnabled(static_cast<Stage>(id), on);
    };
    host.chainOrder = [this]() {
        QVector<int> ids;
        if (!m_audio) return ids;
        for (auto s : m_audio->txChainStages()) {
            const Stage row = fromChainStage(s);
            if (row < StageCount) ids.append(static_cast<int>(row));
        }
        return ids;
    };
    host.commitChainOrder = [this](const QVector<int>& ids) {
        if (!m_audio) return;
        QVector<AudioEngine::TxChainStage> stages;
        stages.reserve(ids.size());
        for (int id : ids) {
            const auto s = toChainStage(static_cast<Stage>(id));
            if (s != AudioEngine::TxChainStage::None) stages.append(s);
        }
        // Unlike the receive side, this order really is consumed: the voice
        // processor takes the packed chain every block.
        // Persists on its own — setTxChainStages writes ClientCompTxChainStages.
        m_audio->setTxChainStages(stages);
    };
    m_tabs->setHost(host);
    row->addWidget(m_tabs);

    m_stack = new QStackedWidget(content);
    row->addWidget(m_stack, 1);
    body->addLayout(row, 1);

    connect(m_tabs, &StageTabBar::stageSelected,
            m_stack, &QStackedWidget::setCurrentIndex);
    connect(m_tabs, &StageTabBar::footerButtonClicked,
            this, &AetherialAudioStrip::showSettings);

    m_tube        = new StripTubePanel        (m_audio, this);
    m_gate        = new StripGatePanel        (m_audio, this);
    m_eq          = new StripEqPanel          (m_audio, this);
    m_comp        = new StripCompPanel        (m_audio, this);
    m_dess        = new StripDeEssPanel       (m_audio, this);
    m_pudu        = new StripPuduPanel        (m_audio, this);
    m_reverb      = new StripReverbPanel      (m_audio, this);
    m_waveform    = new StripWaveformPanel    (m_audio, this);
    m_finalOutput = new StripFinalOutputPanel (m_audio, this);

    // In chain order, which is also the order the signal meets them.
    addStage(Gate,    QStringLiteral("Gate"),       makeStagePage(m_gate));
    addStage(Eq,      QStringLiteral("EQ"),         makeStagePage(m_eq));
    addStage(DeEss,   QStringLiteral("De-esser"),   makeStagePage(m_dess));
    addStage(Comp,    QStringLiteral("Compressor"), makeStagePage(m_comp));
    addStage(Tube,    QStringLiteral("Tube"),       makeStagePage(m_tube));
    addStage(Enh,     QStringLiteral("Exciter"),    makeStagePage(m_pudu));
    addStage(Reverb,  QStringLiteral("Reverb"),     makeStagePage(m_reverb));

    // Output and waveform share the last tab: the meter is what you read and
    // the waveform is what you read it against, so splitting them would mean
    // switching tabs to answer one question.
    {
        auto* page = new QWidget;
        auto* col = new QVBoxLayout(page);
        col->setContentsMargins(0, 0, 0, 0);
        col->setSpacing(8);
        col->addWidget(m_finalOutput, 0, Qt::AlignTop);
        col->addWidget(m_waveform, 1);
        addStage(Output, QStringLiteral("Final Output"), page);
    }

    // MIC and TX indicators. The old chain row carried these as coloured
    // endpoint tiles: MIC green means the PC mic is selected and DAX is off,
    // so the processing chain is genuinely in the transmit path rather than
    // being bypassed without saying so; TX lights while we are keying our own
    // slice. Both answer questions the settings in this window cannot -- a
    // perfectly configured chain that nothing is feeding looks identical.
    {
        auto* status = new QWidget;
        auto* row = new QHBoxLayout(status);
        row->setContentsMargins(0, 2, 0, 2);
        row->setSpacing(6);

        const auto addDot = [&](QLabel*& dot, QLabel*& text, const QString& label,
                                const QString& objectName, const QString& tip) {
            dot = new QLabel;
            dot->setObjectName(QStringLiteral("StatusDot"));
            dot->setFixedSize(10, 10);
            dot->setToolTip(tip);
            text = new QLabel(label);
            text->setObjectName(QStringLiteral("SectionLabel"));
            text->setToolTip(tip);
            // One accessible object for the pair: a bare dot announces
            // nothing, and the label alone does not carry the state (#4896).
            dot->setAccessibleName(label);
            dot->setObjectName(objectName);
            row->addWidget(dot);
            row->addWidget(text);
        };

        addDot(m_micDot, m_micLabel, tr("MIC"),
               QStringLiteral("aetherTxMicIndicator"),
               tr("Lit when the PC microphone is selected and DAX is off, so "
                  "this chain is actually in the transmit path."));
        row->addSpacing(10);
        addDot(m_txDot, m_txLabel, tr("TX"),
               QStringLiteral("aetherTxActiveIndicator"),
               tr("Lit while transmitting on your own slice."));
        row->addStretch(1);

        m_tabs->addFooterWidget(status);
        refreshIndicators();
    }

    // The transmit monitor pair, side by side on one row. Checkable so the
    // lit state can show, but a click only asks: MainWindow owns the monitor and calls
    // setMonitorRecording / setMonitorPlaying back with what actually
    // happened, which is when the button lights. Record red, Play green,
    // the colours the docked applet's pair already use.
    const auto pair = m_tabs->addFooterToggleRow({
        {tr("REC"), QStringLiteral("aetherTxMonitorRecord"),
         tr("Record up to 30 s of processed transmit audio (MIC must be set "
            "to PC and DAX off). Click again to stop; playback starts by "
            "itself."),
         StageTabBar::Accent::Red},
        {tr("PLAY"), QStringLiteral("aetherTxMonitorPlay"),
         tr("Play back the captured audio. Click again to cancel."),
         StageTabBar::Accent::Green},
    });
    m_monRecBtn  = pair.at(0);
    m_monPlayBtn = pair.at(1);
    // The short labels fit the row; the spoken names stay whole words
    // (#4896).
    m_monRecBtn->setAccessibleName(tr("Record"));
    m_monPlayBtn->setAccessibleName(tr("Play"));
    // A checkable button flips itself on click. Put it back and let the
    // monitor's own started/stopped signals light it, so a capture the
    // monitor declined (already playing, say) never shows as running.
    const auto askOnly = [](QPushButton* b) {
        QSignalBlocker block(b);
        b->setChecked(!b->isChecked());
    };
    connect(m_monRecBtn, &QPushButton::clicked, this, [this, askOnly]() {
        askOnly(m_monRecBtn);
        emit monitorRecordClicked();
    });
    connect(m_monPlayBtn, &QPushButton::clicked, this, [this, askOnly]() {
        askOnly(m_monPlayBtn);
        emit monitorPlayClicked();
    });
    // Nothing recorded yet, and why PLAY is greyed out has to reach the
    // accessible channel too, not just the tooltip (#4896).
    setMonitorHasRecording(false);

    m_bypassBtn = m_tabs->addFooterToggle(
        tr("BYPASS"), QStringLiteral("aetherTxBypass"),
        tr("Suppress every voice stage at once, so the microphone reaches the "
           "radio unprocessed. Click again to restore the stages that were on."));
    connect(m_bypassBtn, &QPushButton::toggled,
            this, &AetherialAudioStrip::onBypassToggled);
    if (m_audio) {
        // Engine-owned state: mirror it here so a click on the docked chain
        // applet's BYPASS, or a bridge command, lights this button too.
        {
            QSignalBlocker block(m_bypassBtn);
            m_bypassBtn->setChecked(m_audio->isTxBypassed());
        }
        connect(m_audio, &AudioEngine::txBypassChanged, this, [this](bool on) {
            if (!m_bypassBtn) return;
            QSignalBlocker block(m_bypassBtn);
            m_bypassBtn->setChecked(on);
        });
    }

    // The gear leads BYPASS's row.
    m_tabs->addFooterGearButton(
        tr("Settings"), QStringLiteral("aetherTxSettingsButton"),
        tr("Settings: save, load, import and export transmit chain profiles."));

    // Pin every panel to its TX engine instance, then show the first stage.
    if (m_gate)        m_gate->showForTx();
    if (m_eq)          m_eq->showForPath(ClientEqApplet::Path::Tx);
    if (m_dess)        m_dess->showForTx();
    if (m_comp)        m_comp->showForTx();
    if (m_tube)        m_tube->showForTx();
    if (m_pudu)        m_pudu->showForTx();
    if (m_reverb)      m_reverb->showForTx();
    if (m_finalOutput) m_finalOutput->showForTx();
    if (m_waveform)    m_waveform->showForTx();

    // One panel to a page: the tab already names it, so the plate comes off.
    for (QWidget* panel : {static_cast<QWidget*>(m_gate),
                           static_cast<QWidget*>(m_eq),
                           static_cast<QWidget*>(m_dess),
                           static_cast<QWidget*>(m_comp),
                           static_cast<QWidget*>(m_tube),
                           static_cast<QWidget*>(m_pudu),
                           static_cast<QWidget*>(m_reverb)}) {
        tidyEmbeddedPanel(panel);
    }
    // The Final Output page stacks two, and their plates say which is which.
    tidyEmbeddedPanel(m_finalOutput, /*keepTitle=*/true);
    tidyEmbeddedPanel(m_waveform, /*keepTitle=*/true);

    m_tabs->setCurrentStage(Gate);
    m_stack->setCurrentIndex(Gate);

    // The Client* stages are plain classes with no change signal, and the
    // docked chain applet writes the same flags, so the boxes are polled
    // while the window is visible rather than driven.
    m_tabs->refreshFromHost();
    m_checkTimer = new QTimer(this);
    m_checkTimer->setInterval(200);
    connect(m_checkTimer, &QTimer::timeout, this, [this]() {
        if (m_tabs) m_tabs->refreshFromHost();
    });


    // Wire each panel to its TX-side engine.  The panels' showForTx() /
    // showForPath() methods do this — they also call show() / raise() /
    // activateWindow() which are no-ops on embedded children.  Without
    // this, the EQ icon row + param row collapse to zero height because
    // they have no engine to enumerate bands for, and the canvas displays
    // the "(no EQ connected)" placeholder.
    m_tube->showForTx();
    m_gate->showForTx();
    m_eq->showForPath(ClientEqApplet::Path::Tx);
    m_comp->showForTx();
    m_dess->showForTx();
    m_pudu->showForTx();
    m_reverb->showForTx();
    m_waveform->showForTx();
    m_finalOutput->showForTx();

    // Forward the EQ panel's cutoff-drag signal to the strip's public
    // signal so MainWindow can wire it the same way it wires the
    // floating ClientEqEditor.
    connect(m_eq, &StripEqPanel::cutoffsDragRequested,
            this, &AetherialAudioStrip::cutoffsDragRequested);
}


void AetherialAudioStrip::addStage(Stage stage, const QString& label, QWidget* page)
{
    // Final Output is a meter and a waveform, not a stage: no checkbox, and
    // the bar gives it no grip because it is not in the chain.
    m_tabs->addStage(static_cast<int>(stage), label,
                     /*wantsCheckbox=*/stage != Output);
    const int index = m_stack->addWidget(page);
    Q_ASSERT(index == stage);   // ids double as stack indices
    Q_UNUSED(index);
}

bool AetherialAudioStrip::stageEnabled(Stage stage) const
{
    if (!m_audio) return false;
    switch (stage) {
        case Gate:   return m_audio->clientGateTx()  && m_audio->clientGateTx()->isEnabled();
        case Eq:     return m_audio->clientEqTx()    && m_audio->clientEqTx()->isEnabled();
        case DeEss:  return m_audio->clientDeEssTx() && m_audio->clientDeEssTx()->isEnabled();
        case Comp:   return m_audio->clientCompTx()  && m_audio->clientCompTx()->isEnabled();
        case Tube:   return m_audio->clientTubeTx()  && m_audio->clientTubeTx()->isEnabled();
        case Enh:    return m_audio->clientPuduTx()  && m_audio->clientPuduTx()->isEnabled();
        case Reverb: return m_audio->clientReverbTx() && m_audio->clientReverbTx()->isEnabled();
        default:     return false;
    }
}

void AetherialAudioStrip::setStageEnabled(Stage stage, bool on)
{
    if (!m_audio) return;
    switch (stage) {
        case Gate:
            if (auto* g = m_audio->clientGateTx()) {
                g->setEnabled(on); m_audio->saveClientGateSettings();
            }
            break;
        case Eq:
            if (auto* e = m_audio->clientEqTx()) {
                e->setEnabled(on); m_audio->saveClientEqSettings();
            }
            break;
        case DeEss:
            if (auto* d = m_audio->clientDeEssTx()) {
                d->setEnabled(on); m_audio->saveClientDeEssSettings();
            }
            break;
        case Comp:
            if (auto* c = m_audio->clientCompTx()) {
                c->setEnabled(on); m_audio->saveClientCompSettings();
            }
            break;
        case Tube:
            if (auto* t = m_audio->clientTubeTx()) {
                t->setEnabled(on); m_audio->saveClientTubeSettings();
            }
            break;
        case Enh:
            if (auto* p = m_audio->clientPuduTx()) {
                p->setEnabled(on); m_audio->saveClientPuduSettings();
            }
            break;
        case Reverb:
            if (auto* r = m_audio->clientReverbTx()) {
                r->setEnabled(on); m_audio->saveClientReverbSettings();
            }
            break;
        default:
            break;
    }
    emit stageEnabledChanged(toChainStage(stage), on);
}

void AetherialAudioStrip::showSettings()
{
    // Modal, like the receive window's: a profile load rewrites the whole
    // chain, and letting that happen mid-drag on a stage page would be a
    // good way to lose track of what changed.
    AetherTxSettingsDialog dlg(m_audio, this);
    connect(&dlg, &AetherTxSettingsDialog::profileApplied, this, [this]() {
        refreshAllPanelsFromEngine();
        if (m_tabs) m_tabs->refreshFromHost();
    });
    dlg.exec();
}

void AetherialAudioStrip::closeSettingsIfOpen()
{
    if (AetherTxSettingsDialog* dlg = findChild<AetherTxSettingsDialog*>()) {
        dlg->reject();
    }
}

AetherialAudioStrip::~AetherialAudioStrip() = default;

void AetherialAudioStrip::setAudioPathNotice(const QString& text, bool warning)
{
    if (!m_pcAudioNotice) return;
    m_pcAudioNotice->setText(text);
    m_pcAudioNotice->setAccessibleDescription(text);
    ThemeManager::instance().applyStyleSheet(m_pcAudioNotice, warning
        ? "QLabel { background: {{color.background.warning}}; "
          "color: {{color.accent.warning}}; border: 1px solid {{color.accent.warning}}; "
          "border-radius: 4px; padding: 7px; font-size: 11px; }"
        : "QLabel { background: {{color.background.1}}; "
          "color: {{color.text.primary}}; border: 1px solid {{color.border.strong}}; "
          "border-radius: 4px; padding: 7px; font-size: 11px; }");
    m_pcAudioNotice->setVisible(!text.isEmpty());
}

void AetherialAudioStrip::setTxFilterCutoffs(int lowHz, int highHz)
{
    if (m_eq) m_eq->setTxFilterCutoffs(lowHz, highHz);
}

void AetherialAudioStrip::setMonitorRecording(bool on)
{
    // Driven by MainWindow, which owns the monitor: the button lights when a
    // capture is actually running, not when it was asked for. Blocked so the
    // readback cannot look like a second click.
    if (!m_monRecBtn) return;
    QSignalBlocker block(m_monRecBtn);
    m_monRecBtn->setChecked(on);
}

void AetherialAudioStrip::setMonitorPlaying(bool on)
{
    if (!m_monPlayBtn) return;
    QSignalBlocker block(m_monPlayBtn);
    m_monPlayBtn->setChecked(on);
}

void AetherialAudioStrip::setMonitorHasRecording(bool has)
{
    if (!m_monPlayBtn) return;
    m_monPlayBtn->setEnabled(has);
    // A screen-reader user meets a disabled button with no explanation
    // otherwise (#4896).
    m_monPlayBtn->setAccessibleDescription(
        has ? tr("Play back the captured audio. Click again to cancel.")
            : tr("Unavailable until something has been recorded. "
                 "Use REC first."));
}

void AetherialAudioStrip::setMicInputReady(bool ready)
{
    if (m_micReady == ready) return;
    m_micReady = ready;
    refreshIndicators();
}

void AetherialAudioStrip::setTxActive(bool active)
{
    if (m_txActive == active) return;
    m_txActive = active;
    refreshIndicators();
}

void AetherialAudioStrip::refreshChainPaint()
{
    // Engine state is the source of truth; this just pulls the column's
    // checkboxes and order back from it after something else moved them.
    if (m_tabs) m_tabs->refreshFromHost();
}

void AetherialAudioStrip::refreshIndicators()
{
    const auto paint = [](QLabel* dot, QLabel* text, bool on, const QString& colour) {
        if (!dot || !text) return;
        ThemeManager::instance().applyStyleSheet(dot, QStringLiteral(
            "QLabel { background: %1; border-radius: 5px; border: 1px solid %2; }")
            .arg(on ? colour : QStringLiteral("{{color.background.1}}"),
                 on ? colour : QStringLiteral("{{color.border.strong}}")));
        text->setEnabled(on);
        // The state has to reach the accessible channel, not just the
        // colour -- the whole point of the dot is that it is a state (#4896).
        dot->setAccessibleDescription(
            on ? QObject::tr("active") : QObject::tr("inactive"));
    };
    paint(m_micDot, m_micLabel, m_micReady,
          QStringLiteral("{{color.accent.success}}"));
    paint(m_txDot,  m_txLabel,  m_txActive,
          QStringLiteral("{{color.accent.danger}}"));
}

void AetherialAudioStrip::closeEvent(QCloseEvent* ev)
{
    // CanonWindow::closeEvent() saves the geometry and flushes the settings.
    AppSettings::instance().setValue("AetherialStripVisible", "False");
    CanonWindow::closeEvent(ev);
}

void AetherialAudioStrip::showEvent(QShowEvent* ev)
{
    // Opens at kLaunchSize every time, whatever is stored — the same
    // deliberate, temporary pin the receive window carries while the size is
    // still being settled. Position is still restored and still saved. After
    // the base, which restores the saved geometry, size and all.
    CanonWindow::showEvent(ev);
    if (size() != kLaunchSize) resize(kLaunchSize);
    if (m_tabs) m_tabs->refreshFromHost();
    if (m_checkTimer) m_checkTimer->start();
    auto& s = AppSettings::instance();
    s.setValue("AetherialStripVisible", "True");
    s.save();
}

void AetherialAudioStrip::hideEvent(QHideEvent* ev)
{
    if (m_checkTimer) m_checkTimer->stop();
    auto& s = AppSettings::instance();
    s.setValue("AetherialStripVisible", "False");
    s.save();
    CanonWindow::hideEvent(ev);
}

// ──────────────────────────────────────────────────────────────────
// Master bypass
// ──────────────────────────────────────────────────────────────────

void AetherialAudioStrip::onBypassToggled(bool checked)
{
    if (!m_audio) return;
    // Engine owns the bypass snapshots — both this window and the docked
    // Chain applet route through setTxBypassed and observe txBypassChanged
    // to stay in lock-step, so the click lands here and the button follows
    // the engine back. RX bypass belongs to AetherRX.
    m_audio->setTxBypassed(checked);
}

// Preset combo items:
//   0..N-1      stored preset names (alphabetic)
//   N           "──────────"  (disabled separator)
//   N+1         "Import\xe2\x80\xa6"
//   N+2         "Export\xe2\x80\xa6"
// Actions are identified by UserRole sentinels, not text, so labels can be
// localised.

namespace {
constexpr int kRolePresetName       = Qt::UserRole + 1;
constexpr int kRoleAction           = Qt::UserRole + 2;
constexpr int kActionImport         = 1;
constexpr int kActionExportPreset   = 2;
constexpr int kActionExportLibrary  = 3;
}




void AetherialAudioStrip::refreshAllPanelsFromEngine()
{
    // Preset loader writes new values straight into the engine; nudge
    // every panel so its widgets pick them up (knobs, param-row text,
    // canvas curves, family combo).  Without this, only the live
    // visualisations that re-read engine state on paint update — text
    // labels stay stale.
    if (m_eq)     m_eq->refreshFromEngine();
    if (m_tube)   m_tube->syncControlsFromEngine();
    if (m_gate)   m_gate->syncControlsFromEngine();
    if (m_comp)   m_comp->syncControlsFromEngine();
    if (m_dess)   m_dess->syncControlsFromEngine();
    if (m_pudu)   m_pudu->syncControlsFromEngine();
    if (m_reverb) m_reverb->syncControlsFromEngine();
    if (m_waveform)    m_waveform->syncControlsFromEngine();
    if (m_finalOutput) m_finalOutput->syncControlsFromEngine();
    if (m_tabs)   m_tabs->refreshFromHost();
}






} // namespace AetherSDR
