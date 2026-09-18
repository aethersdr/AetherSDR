#include "AetherialAudioStrip.h"

#include <QGuiApplication>
#include <QScreen>

#include "FramelessMoveHelper.h"
#include "FramelessResizer.h"
#include "AetherTxSettingsDialog.h"
#include "StagePage.h"
#include "StageTabBar.h"
#include "ClientEqApplet.h"
#include "EditorFramelessTitleBar.h"
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
#include <QMouseEvent>
#include <QMoveEvent>
#include <QResizeEvent>
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
// Edge-to-edge title-bar height. Reserved from the frameless resize edge so a
// title-bar grab starts a move, not a top-edge resize (#4266).
constexpr int kTitleBarHeight = 18;
}

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
    : QWidget(parent, Qt::Window)
    , m_audio(engine)
{
    const QString title = QStringLiteral("AetherTX");
    setWindowTitle(title);
    AetherSDR::ThemeManager::instance().applyStyleSheet(this, "QWidget { background: {{color.background.0}}; color: {{color.text.primary}}; }"
        "QFrame#stripGroupBox { border: 1px solid {{color.background.1}};"
        " border-radius: 4px; background: transparent; }");
    setMinimumSize(1140, 900);
    // The strip's natural content is ~1620 px tall and assumes a 4K
    // (or larger) display.  On 1080p / 1440p, opening at 1620 puts the
    // One stage to a page now, so the window no longer has to be tall enough
    // for a nine-panel grid. Same size the receive window opens at, for the
    // same reason: it sits on the desktop rather than filling it.
    setMinimumSize(600, 400);
    resize(kLaunchSize);

    // Listen at the native-window boundary so edge presses still reach the
    // resize handler when the child-heavy strip content covers every margin.
    // Reserve the edge-to-edge title-bar strip for moves so a title-bar grab
    // isn't stolen by the top-edge resize zone (#4266).
    FramelessResizer::install(this, 6, kTitleBarHeight);

    // Outer layout has zero margins so the title bar can run edge-to-edge
    // across the whole window (matching the applet ContainerTitleBar).
    // The 6 px resize hit zone + 2 px breathing room lives on a nested
    // content layout below the title bar.
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // Custom title bar styled to match the applet ContainerTitleBar:
    // 18 px tall, blue-gradient background, 10 px bold title, trio of
    // window-control buttons at the right.  Built inline rather than
    // via EditorFramelessTitleBar because that widget is hard-wired to
    // the editor look (20 px flat).  The strip wants the chrome family
    // it shares with the docked applet panels.
    {
        m_titleBar = new QWidget(this);
        m_titleBar->setFixedHeight(kTitleBarHeight);
        m_titleBar->setAttribute(Qt::WA_StyledBackground, true);
        AetherSDR::ThemeManager::instance().applyStyleSheet(m_titleBar, "QWidget { background: qlineargradient(x1:0,y1:0,x2:0,y2:1,"
            "stop:0 #5a7494, stop:0.5 #384e68, stop:1 {{color.background.1}}); "
            "border-bottom: 1px solid #0a1a28; }");
        m_titleBar->installEventFilter(this);

        auto* row = new QHBoxLayout(m_titleBar);
        row->setContentsMargins(6, 0, 2, 0);
        row->setSpacing(4);

        // Drag grip on the left — matches ContainerTitleBar.  Decorative
        // only; actual drag is handled on the bar as a whole via the
        // event filter installed below.
        auto* grip = new QLabel(QString::fromUtf8("\xe2\x8b\xae\xe2\x8b\xae"),
                                m_titleBar);
        AetherSDR::ThemeManager::instance().applyStyleSheet(grip, "QLabel { background: transparent; color: {{color.text.secondary}};"
            " font-size: 10px; }");
        row->addWidget(grip);

        // No side suffix any more: this window is the transmit chain, and the
        // receive chain is AetherRX.
        m_titleLbl = new QLabel(title,
                                m_titleBar);
        m_titleLbl->setStyleSheet(
            "QLabel { background: transparent; color: #e0ecf4;"
            " font-size: 10px; font-weight: bold; }");
        row->addWidget(m_titleLbl);
        row->addStretch();

        const QString btnStyle =
            "QPushButton { background: transparent; border: none;"
            " color: #c8d8e8; font-size: 11px; font-weight: bold;"
            " padding: 0px 4px; }"
            "QPushButton:hover { color: #ffffff; }";
        const QString closeBtnStyle =
            "QPushButton { background: transparent; border: none;"
            " color: #c8d8e8; font-size: 11px; font-weight: bold;"
            " padding: 0px 4px; }"
            "QPushButton:hover { color: #ffffff; background: #cc2030; }";

        auto* minBtn = new QPushButton(QString::fromUtf8("\xe2\x80\x94"), m_titleBar);  // —
        minBtn->setFixedSize(16, 16);
        minBtn->setCursor(Qt::ArrowCursor);
        minBtn->setStyleSheet(btnStyle);
        minBtn->setToolTip("Minimize");
        connect(minBtn, &QPushButton::clicked, this, &QWidget::showMinimized);
        row->addWidget(minBtn);

        auto* maxBtn = new QPushButton(QString::fromUtf8("\xe2\x96\xa1"), m_titleBar); // □
        maxBtn->setFixedSize(16, 16);
        maxBtn->setCursor(Qt::ArrowCursor);
        maxBtn->setStyleSheet(btnStyle);
        maxBtn->setToolTip("Maximize");
        connect(maxBtn, &QPushButton::clicked, this, [this]() {
            if (isMaximized()) showNormal(); else showMaximized();
        });
        row->addWidget(maxBtn);

        auto* closeBtn = new QPushButton(QString::fromUtf8("\xc3\x97"), m_titleBar); // ×
        closeBtn->setFixedSize(16, 16);
        closeBtn->setCursor(Qt::ArrowCursor);
        closeBtn->setStyleSheet(closeBtnStyle);
        closeBtn->setToolTip("Close");
        connect(closeBtn, &QPushButton::clicked, this, &QWidget::close);
        row->addWidget(closeBtn);
    }
    root->addWidget(m_titleBar);

    // Content area below the title bar — has the 8 px resize-hit margin
    // so the title bar can stay flush to the window edges above.
    auto* content = new QWidget(this);
    auto* body = new QVBoxLayout(content);
    body->setContentsMargins(8, 0, 8, 8);
    body->setSpacing(8);
    m_bodyLayout = body;
    root->addWidget(content, 1);

    // The transmit chain, one stage to a page, with the column down the left
    // standing in for the horizontal chain strip this window used to carry.
    // Same component AetherRX uses — the two windows are the same idea
    // pointed in opposite directions, so a second copy of the column would
    // have been two places to fix every tab-bar bug.
    //
    // BYPASS, the monitor pair and the preset controls moved into Settings.
    // Neither live control is stranded: ClientChainApplet carries its own
    // BYPASS and its own record/play buttons on the docked panel.
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

    m_tabs->addFooterButton(
        tr("Settings"), QStringLiteral("aetherTxSettingsButton"),
        tr("Profiles, bypass and the transmit monitor."));

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


    // Hide the min / max / close trio on each embedded panel's title bar
    // — the strip owns the window controls, so each panel just needs its
    // name plate.  dynamic_cast (rather than findChild) because
    // EditorFramelessTitleBar has no Q_OBJECT macro; RTTI works fine and
    // we don't need to reach into every Strip*Panel to expose its title
    // bar member.
    //
    // Same pass also rewrites each panel's own QSS so the legacy
    // `#08121d` band colour (inherited from when the panels were
    // duplicated from the floating-editor sources) becomes `#0f0f1a` to
    // match SMeterWidget / applet-panel chrome.  Qt always lets a
    // child's own stylesheet win over a parent's, regardless of selector
    // specificity, so a strip-level override doesn't reach these — the
    // only way to override without editing every Strip*Panel source is
    // to rewrite their stylesheets in place at construction.
    auto recolour = [](QWidget* w) {
        QString s = w->styleSheet();
        if (s.contains("#08121d")) {
            s.replace("#08121d", "#0f0f1a");
            w->setStyleSheet(s);
        }
    };
    for (QWidget* p : { static_cast<QWidget*>(m_tube),
                        static_cast<QWidget*>(m_gate),
                        static_cast<QWidget*>(m_eq),
                        static_cast<QWidget*>(m_comp),
                        static_cast<QWidget*>(m_dess),
                        static_cast<QWidget*>(m_pudu),
                        static_cast<QWidget*>(m_reverb) }) {
        if (!p) continue;
        recolour(p);
        for (QObject* child : p->children()) {
            if (auto* tb = dynamic_cast<EditorFramelessTitleBar*>(child)) {
                tb->setControlsVisible(false);
                recolour(tb);
                break;
            }
        }
    }

    restoreGeometryFromSettings();
    setFramelessMode(
        AppSettings::instance().value("FramelessWindow", "True").toString() == "True");

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
    // Tracked for the life of the dialog, not handed over once: a capture
    // reaching its 30 s cap while the dialog is open has to move the buttons,
    // or Record stays lit and Play stays disabled -- including its accessible
    // description, which would go on saying "nothing has been recorded" after
    // something had been.
    m_settingsDlg = &dlg;
    dlg.setBypassed(m_audio && m_audio->isTxBypassed());
    dlg.setMonitorRecording(m_monRecording);
    dlg.setMonitorHasRecording(m_monHasRecording);
    dlg.setMonitorPlaying(m_monPlaying);
    connect(&dlg, &AetherTxSettingsDialog::profileApplied, this, [this]() {
        refreshAllPanelsFromEngine();
        if (m_tabs) m_tabs->refreshFromHost();
    });
    connect(&dlg, &AetherTxSettingsDialog::bypassToggled,
            this, &AetherialAudioStrip::onBypassToggled);
    connect(&dlg, &AetherTxSettingsDialog::monitorRecordClicked,
            this, &AetherialAudioStrip::monitorRecordClicked);
    connect(&dlg, &AetherTxSettingsDialog::monitorPlayClicked,
            this, &AetherialAudioStrip::monitorPlayClicked);
    dlg.exec();
    m_settingsDlg = nullptr;
}

AetherialAudioStrip::~AetherialAudioStrip() = default;

void AetherialAudioStrip::setFramelessMode(bool on)
{
    const QRect geom = geometry();
    const bool wasVisible = isVisible();

    Qt::WindowFlags flags = (windowFlags() & ~Qt::WindowType_Mask) | Qt::Window;
    flags.setFlag(Qt::FramelessWindowHint, on);
    setWindowFlags(flags);
    if (wasVisible) {
        setGeometry(geom);
    }
    if (m_titleBar) {
        m_titleBar->setVisible(on);
    }
    if (m_bodyLayout) {
        m_bodyLayout->setContentsMargins(8, on ? 0 : 8, 8, 8);
    }
    if (wasVisible) {
        show();
    }
}

void AetherialAudioStrip::setTxFilterCutoffs(int lowHz, int highHz)
{
    if (m_eq) m_eq->setTxFilterCutoffs(lowHz, highHz);
}

namespace {
constexpr const char* kMonBtnBase =
    "QPushButton { background: rgba(255,255,255,15); border: none;"
    " border-radius: 10px; font-size: 11px; padding: 0; }"
    "QPushButton:hover:enabled { background: rgba(255,255,255,40); }"
    "QPushButton:disabled { color: #303030;"
    " background: rgba(255,255,255,5); }";
constexpr const char* kRecIdle    = "QPushButton:enabled { color: #804040; }";
constexpr const char* kRecActive  = "QPushButton { color: #ff2020;"
                                    " background: rgba(255,50,50,60); }";
constexpr const char* kPlayIdle   = "QPushButton:enabled { color: #406040; }";
constexpr const char* kPlayActive = "QPushButton { color: #30d050;"
                                    " background: rgba(50,200,80,60); }";
} // namespace

void AetherialAudioStrip::setMonitorRecording(bool on)
{
    // Held for the next time Settings opens, and forwarded if it is open now.
    m_monRecording = on;
    if (m_settingsDlg) m_settingsDlg->setMonitorRecording(on);
}

void AetherialAudioStrip::setMonitorPlaying(bool on)
{
    m_monPlaying = on;
    if (m_settingsDlg) m_settingsDlg->setMonitorPlaying(on);
}

void AetherialAudioStrip::setMonitorHasRecording(bool has)
{
    m_monHasRecording = has;
    if (m_settingsDlg) m_settingsDlg->setMonitorHasRecording(has);
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

void AetherialAudioStrip::saveGeometryToSettings()
{
    if (m_restoring) return;
    auto& s = AppSettings::instance();
    s.setValue("AetherialStripGeometry2", saveGeometry().toBase64());
    s.save();
}

void AetherialAudioStrip::restoreGeometryFromSettings()
{
    auto& s = AppSettings::instance();
    const QByteArray geom = QByteArray::fromBase64(
        s.value("AetherialStripGeometry2").toString().toUtf8());
    if (geom.isEmpty()) return;

    m_restoring = true;
    restoreGeometry(geom);
    m_restoring = false;
}

void AetherialAudioStrip::closeEvent(QCloseEvent* ev)
{
    saveGeometryToSettings();
    auto& s = AppSettings::instance();
    s.setValue("AetherialStripVisible", "False");
    s.save();
    QWidget::closeEvent(ev);
}

void AetherialAudioStrip::moveEvent(QMoveEvent* ev)
{
    saveGeometryToSettings();
    QWidget::moveEvent(ev);
}

void AetherialAudioStrip::resizeEvent(QResizeEvent* ev)
{
    saveGeometryToSettings();
    QWidget::resizeEvent(ev);
}

void AetherialAudioStrip::showEvent(QShowEvent* ev)
{
    // Opens at kLaunchSize every time, whatever is stored — the same
    // deliberate, temporary pin the receive window carries while the size is
    // still being settled. Position is still restored and still saved.
    if (size() != kLaunchSize) resize(kLaunchSize);
    if (m_tabs) m_tabs->refreshFromHost();
    if (m_checkTimer) m_checkTimer->start();
    auto& s = AppSettings::instance();
    s.setValue("AetherialStripVisible", "True");
    s.save();
    QWidget::showEvent(ev);
}

void AetherialAudioStrip::hideEvent(QHideEvent* ev)
{
    if (m_checkTimer) m_checkTimer->stop();
    auto& s = AppSettings::instance();
    s.setValue("AetherialStripVisible", "False");
    s.save();
    QWidget::hideEvent(ev);
}

bool AetherialAudioStrip::eventFilter(QObject* obj, QEvent* ev)
{
    if (obj == m_titleBar && ev->type() == QEvent::MouseMove) {
        return FramelessMoveHelper::move(m_titleBar, static_cast<QMouseEvent*>(ev));
    }
    if (obj == m_titleBar && ev->type() == QEvent::MouseButtonRelease) {
        return FramelessMoveHelper::finish(m_titleBar, static_cast<QMouseEvent*>(ev));
    }

    // Drag-to-move via the custom title bar.  The trio buttons are
    // independent QPushButtons that consume the press themselves, so
    // this only fires on the bare title-bar background between the
    // title text and the trio.
    if (obj == m_titleBar && ev->type() == QEvent::MouseButtonPress) {
        auto* me = static_cast<QMouseEvent*>(ev);
        return FramelessMoveHelper::start(m_titleBar, me);
    }
    if (obj == m_titleBar && ev->type() == QEvent::MouseButtonDblClick) {
        if (isMaximized()) showNormal();
        else               showMaximized();
        ev->accept();
        return true;
    }
    return QWidget::eventFilter(obj, ev);
}

// ──────────────────────────────────────────────────────────────────
// Master bypass
// ──────────────────────────────────────────────────────────────────

void AetherialAudioStrip::onBypassToggled(bool checked)
{
    if (!m_audio) return;
    // Engine owns the bypass snapshots — both this widget and the docked
    // Chain applet route through setTxBypassed and observe
    // txBypassChanged to stay in lock-step. RX bypass belongs to AetherRX.
    m_audio->setTxBypassed(checked);
}

// ──────────────────────────────────────────────────────────────────
// Preset combo box
// ──────────────────────────────────────────────────────────────────
//
// Items laid out as:
//   0..N-1      stored preset names (alphabetic)
//   N           "──────────"  (disabled separator)
//   N+1         "Import\xe2\x80\xa6"
//   N+2         "Export\xe2\x80\xa6"
//
// We deliberately use index sentinels (UserRole) rather than text
// matching so localising the action labels later is safe.

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
