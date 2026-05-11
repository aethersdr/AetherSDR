#include "AetherialAudioStrip.h"

#include <QGuiApplication>
#include <QScreen>

#include "AetherDspWidget.h"
#include "FramelessMoveHelper.h"
#include "StripChainWidget.h"
#include "StripRxChainWidget.h"
#include "ClientEqApplet.h"
#include "EditorFramelessTitleBar.h"
#include "StripCompPanel.h"
#include "StripDeEssPanel.h"
#include "StripRxOutputPanel.h"
#include "StripEqPanel.h"
#include "StripGatePanel.h"
#include "StripPuduPanel.h"
#include "StripReverbPanel.h"
#include "StripTubePanel.h"
#include "StripWaveformPanel.h"
#include "StripFinalOutputPanel.h"
#include "core/AppSettings.h"
#include "core/AudioEngine.h"
#include "core/ChannelStripPresets.h"
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
#include <QWindow>

namespace AetherSDR {

namespace {
constexpr int kResizeMargin = 6;
}

AetherialAudioStrip::AetherialAudioStrip(AudioEngine* engine, QWidget* parent)
    : QWidget(parent, Qt::Window)
    , m_audio(engine)
    , m_presets(new ChannelStripPresets(engine, this))
{
    connect(m_presets, &ChannelStripPresets::presetsChanged, this, [this]() {
        rebuildPresetCombo(m_currentPresetName);
    });
    const QString title = QString::fromUtf8("Aetherial Audio \xe2\x80\x94 Channel Strip");
    setWindowTitle(title);
    setStyleSheet(
        "QWidget { background: #0f0f1a; color: #d7e4f2; }"
        "QFrame#stripGroupBox { border: 1px solid #2a3744;"
        " border-radius: 4px; background: transparent; }");
    // Width is hard — the chain row + 50 px tiles need the full
    // 1140 px to read.  Height drops to 900 so the strip fits on a
    // 1080p display (1080 minus taskbar/title chrome ≈ 1000); the
    // panel grid below the chain row scrolls vertically when the
    // window is shorter than the natural content height.
    setMinimumSize(1140, 900);
    // The strip's natural content is ~1620 px tall and assumes a 4K
    // (or larger) display.  On 1080p / 1440p, opening at 1620 puts the
    // bottom edge offscreen with the resize grip unreachable.  Cap to
    // 960 on anything below ~4K so the bottom stays grabbable; users
    // can grow the window manually after open.
    int initialHeight = 1620;
    if (auto* screen = QGuiApplication::primaryScreen()) {
        if (screen->availableGeometry().height() < 2000)
            initialHeight = 960;
    }
    resize(1140, initialHeight);

    // Track mouse without buttons pressed so the resize cursor updates
    // while hovering the bare margin around the embedded grid.
    setMouseTracking(true);

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
        m_titleBar->setFixedHeight(18);
        m_titleBar->setAttribute(Qt::WA_StyledBackground, true);
        m_titleBar->setStyleSheet(
            "QWidget { background: qlineargradient(x1:0,y1:0,x2:0,y2:1,"
            "stop:0 #5a7494, stop:0.5 #384e68, stop:1 #1e2e3e); "
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
        grip->setStyleSheet(
            "QLabel { background: transparent; color: #a0b4c8;"
            " font-size: 10px; }");
        row->addWidget(grip);

        m_titleLbl = new QLabel(title + QString::fromUtf8(" \xe2\x80\x94 TX"),
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

    // Top: horizontal CHAIN strip + record/play buttons inline at the
    // right of the TX endpoint.  The buttons just emit signals; MainWindow
    // wires them to the same PUDU monitor as the docked ClientChainApplet.
    {
        auto* chainRow = new QHBoxLayout;
        chainRow->setContentsMargins(0, 0, 0, 0);
        chainRow->setSpacing(6);

        // TX / RX mode toggles — exclusive group, TX checked by default.
        // Visual parity with the docked chain applet's mode buttons; the
        // strip currently embeds TX panels only, so RX is present as a
        // no-op placeholder until RX-side strip panels arrive.
        const QString modeBtnStyle = QStringLiteral(
            "QPushButton {"
            "  background: #1a2a3a; border: 1px solid #2a4458;"
            "  border-radius: 3px; color: #8aa8c0;"
            "  font-size: 12px; font-weight: bold;"
            "  padding: 2px 10px; min-width: 30px;"
            "}"
            "QPushButton:hover { background: #24384e; }"
            "QPushButton:checked {"
            "  background: #3a2a0e; color: #f2c14e;"
            "  border: 1px solid #f2c14e;"
            "}");

        auto* modeGroup = new QButtonGroup(this);
        modeGroup->setExclusive(true);

        m_txBtn = new QPushButton("TX", content);
        m_txBtn->setCheckable(true);
        m_txBtn->setChecked(true);
        m_txBtn->setFixedHeight(30);
        m_txBtn->setStyleSheet(modeBtnStyle);
        m_txBtn->setToolTip(tr("Show and edit the TX DSP chain"));
        modeGroup->addButton(m_txBtn);
        chainRow->addWidget(m_txBtn);

        m_rxBtn = new QPushButton("RX", content);
        m_rxBtn->setCheckable(true);
        m_rxBtn->setFixedHeight(30);
        m_rxBtn->setStyleSheet(modeBtnStyle);
        m_rxBtn->setToolTip(tr("Show and edit the RX DSP chain"));
        modeGroup->addButton(m_rxBtn);
        chainRow->addWidget(m_rxBtn);

        // Mode toggle — swap chain widget + panel grid + BYPASS target
        // + title-bar suffix when the user clicks RX (or TX).  Mode is
        // persisted so the strip reopens at the user's last choice.
        connect(m_rxBtn, &QPushButton::toggled, this, [this](bool on) {
            m_rxMode = on;
            if (m_chainStack) m_chainStack->setCurrentIndex(on ? 1 : 0);
            if (m_panelStack) m_panelStack->setCurrentIndex(on ? 1 : 0);
            if (m_titleLbl) {
                const QString base =
                    QString::fromUtf8("Aetherial Audio \xe2\x80\x94 Channel Strip");
                m_titleLbl->setText(base + (on
                    ? QString::fromUtf8(" \xe2\x80\x94 RX")
                    : QString::fromUtf8(" \xe2\x80\x94 TX")));
            }
            if (m_bypassBtn && m_audio) {
                QSignalBlocker b(m_bypassBtn);
                m_bypassBtn->setChecked(on
                    ? m_audio->isRxBypassed()
                    : m_audio->isTxBypassed());
            }
            AppSettings::instance().setValue("AetherialStripMode",
                                             on ? "RX" : "TX");
        });

        m_chain = new StripChainWidget(content);
        m_chain->setAudioEngine(m_audio);
        // Forward stage-bypass clicks out as our own signal so MainWindow
        // can refresh the docked Chain applet's chain widget in step.
        connect(m_chain, &StripChainWidget::stageEnabledChanged,
                this,    &AetherialAudioStrip::stageEnabledChanged);

        m_chainRx = new StripRxChainWidget(content);
        m_chainRx->setAudioEngine(m_audio);
        connect(m_chainRx, &StripRxChainWidget::stageEnabledChanged,
                this,      &AetherialAudioStrip::rxStageEnabledChanged);
        connect(m_chainRx, &StripRxChainWidget::dspEditRequested,
                this,      &AetherialAudioStrip::rxDspEditRequested);
        connect(m_chainRx, &StripRxChainWidget::editRequested,
                this,      &AetherialAudioStrip::rxStageEditRequested);

        // QStackedWidget lets the TX/RX toggle swap the visible chain
        // without rebuilding either widget.  Both pages stay alive so
        // their engine bindings keep observing.
        m_chainStack = new QStackedWidget(content);
        m_chainStack->addWidget(m_chain);     // page 0 — TX
        m_chainStack->addWidget(m_chainRx);   // page 1 — RX
        m_chainStack->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        chainRow->addWidget(m_chainStack);

        // BYPASS — sits to the right of the chain's TX endpoint.  Same
        // checkable amber toggle as the docked applet.
        m_bypassBtn = new QPushButton("BYPASS", content);
        m_bypassBtn->setCheckable(true);
        m_bypassBtn->setFixedHeight(30);
        m_bypassBtn->setStyleSheet(
            "QPushButton {"
            "  background: #1a2a3a; border: 1px solid #4a3020; border-radius: 3px;"
            "  color: #c8a070; font-size: 12px; font-weight: bold;"
            "  padding: 2px 10px;"
            "}"
            "QPushButton:hover { background: #3a2818; color: #f2c14e;"
            "                    border: 1px solid #f2c14e; }"
            "QPushButton:checked {"
            "  background: #4a3818; color: #f2c14e; border: 1px solid #f2c14e;"
            "}"
            "QPushButton:checked:hover { background: #5a4a28; }");
        m_bypassBtn->setToolTip(
            tr("Disable every stage in the active chain.  Click again "
               "to restore the stages that were on before."));
        connect(m_bypassBtn, &QPushButton::toggled,
                this, &AetherialAudioStrip::onBypassToggled);
        // Initial visual state from engine, then keep it in lock-step
        // with the docked Chain applet's BYPASS button — both observe
        // the engine's per-mode bypass signal.
        if (m_audio) {
            QSignalBlocker blocker(m_bypassBtn);
            m_bypassBtn->setChecked(m_audio->isTxBypassed());
            // Listen on both mode signals; the visible state reflects
            // whichever mode the strip is currently displaying.
            connect(m_audio, &AudioEngine::txBypassChanged,
                    this, [this](bool on) {
                if (!m_bypassBtn || m_rxMode) return;
                QSignalBlocker b(m_bypassBtn);
                m_bypassBtn->setChecked(on);
                if (m_chain) m_chain->update();
            });
            connect(m_audio, &AudioEngine::rxBypassChanged,
                    this, [this](bool on) {
                if (!m_bypassBtn || !m_rxMode) return;
                QSignalBlocker b(m_bypassBtn);
                m_bypassBtn->setChecked(on);
                if (m_chainRx) m_chainRx->update();
            });
        }
        chainRow->addWidget(m_bypassBtn);

        const QString monBtnBase =
            "QPushButton { background: rgba(255,255,255,15); border: none;"
            " border-radius: 15px; font-size: 28px; padding: 0; }"
            "QPushButton:hover:enabled { background: rgba(255,255,255,40); }"
            "QPushButton:disabled { color: #303030;"
            " background: rgba(255,255,255,5); }";
        const QString recIdle  = monBtnBase + "QPushButton:enabled { color: #804040; }";
        const QString playIdle = monBtnBase + "QPushButton:enabled { color: #406040; }";

        m_monRecBtn = new QPushButton(QString::fromUtf8("\xe2\x8f\xba"), content); // ⏺
        m_monRecBtn->setFixedSize(30, 30);
        m_monRecBtn->setStyleSheet(recIdle);
        m_monRecBtn->setToolTip(
            "Record up to 30 s of post-PooDoo\xe2\x84\xa2 TX audio (MIC must "
            "be set to PC and DAX off).  Click again to stop; playback "
            "starts automatically.");
        connect(m_monRecBtn, &QPushButton::clicked,
                this, &AetherialAudioStrip::monitorRecordClicked);
        chainRow->addWidget(m_monRecBtn);

        m_monPlayBtn = new QPushButton(QString::fromUtf8("\xe2\x96\xb6"), content); // ▶
        m_monPlayBtn->setFixedSize(30, 30);
        m_monPlayBtn->setStyleSheet(playIdle);
        m_monPlayBtn->setToolTip(
            "Play back the captured PooDoo\xe2\x84\xa2 audio.  Click again "
            "to cancel playback.");
        connect(m_monPlayBtn, &QPushButton::clicked,
                this, &AetherialAudioStrip::monitorPlayClicked);
        chainRow->addWidget(m_monPlayBtn);

        // Stretch absorbs surplus row width so the preset combo +
        // Save/Delete cluster right-aligns against the strip's edge.
        chainRow->addStretch();

        // Preset combo — right-aligned at the far end of the chain row.
        // Items: [stored presets...] [---separator---] [Import...] [Export...]
        // Populated lazily by rebuildPresetCombo() below.
        m_presetCombo = new QComboBox(content);
        m_presetCombo->setMinimumWidth(220);
        m_presetCombo->setFixedHeight(30);
        m_presetCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        m_presetCombo->setToolTip(
            "Channel-strip presets — pick a stored preset to load it, or use "
            "Import\xe2\x80\xa6 / Export\xe2\x80\xa6 to share presets via files.");
        m_presetCombo->setStyleSheet(
            "QComboBox { background: #1a2230; color: #d7e4f2;"
            " border: 1px solid #2a3744; border-radius: 4px;"
            " padding: 1px 6px; font-size: 12px; }"
            "QComboBox:hover { border-color: #4a8fcf; }"
            "QComboBox::drop-down { width: 14px; border: none; }"
            "QComboBox QAbstractItemView { background: #1a2230;"
            " color: #d7e4f2; selection-background-color: #2a4a6a;"
            " border: 1px solid #2a3744; }");
        connect(m_presetCombo,
                QOverload<int>::of(&QComboBox::activated),
                this, &AetherialAudioStrip::onPresetComboActivated);
        chainRow->addWidget(m_presetCombo);

        // Save / Delete buttons live to the right of the combo.  They
        // share the rec/play button visual language but get their own
        // hover tint so they're discoverable as preset-management.
        const QString presetBtnBase =
            "QPushButton { background: rgba(255,255,255,15); color: #d7e4f2;"
            " border: none; border-radius: 4px; font-size: 12px;"
            " padding: 0 10px; }"
            "QPushButton:hover:enabled { background: rgba(255,255,255,40); }"
            "QPushButton:disabled { color: #4a5562;"
            " background: rgba(255,255,255,5); }";

        m_presetSaveBtn = new QPushButton(tr("Save"), content);
        m_presetSaveBtn->setFixedHeight(30);
        m_presetSaveBtn->setStyleSheet(presetBtnBase);
        m_presetSaveBtn->setToolTip(
            tr("Save the current channel-strip state as a preset.  If a "
               "preset is selected, its name is prefilled — saving with "
               "the same name overwrites it; changing the name saves a "
               "new preset alongside the original."));
        connect(m_presetSaveBtn, &QPushButton::clicked,
                this, &AetherialAudioStrip::doSavePreset);
        chainRow->addWidget(m_presetSaveBtn);

        m_presetDeleteBtn = new QPushButton(tr("Delete"), content);
        m_presetDeleteBtn->setFixedHeight(30);
        m_presetDeleteBtn->setStyleSheet(presetBtnBase);
        m_presetDeleteBtn->setToolTip(
            tr("Delete the currently selected preset from the local "
               "library.  Disabled when no preset is selected."));
        connect(m_presetDeleteBtn, &QPushButton::clicked,
                this, &AetherialAudioStrip::doDeletePreset);
        chainRow->addWidget(m_presetDeleteBtn);

        // Restore the last-active preset name from AppSettings so the
        // combo opens showing whichever preset was loaded last.  The
        // engine state is already restored by AudioEngine's per-module
        // load helpers — this just brings the UI label back in line.
        const QString lastActive = AppSettings::instance()
            .value("AetherialStripActivePreset", "").toString();
        if (!lastActive.isEmpty() && m_presets
            && m_presets->hasPreset(lastActive)) {
            m_currentPresetName = lastActive;
        }
        rebuildPresetCombo(m_currentPresetName);

        body->addLayout(chainRow);
    }

    // Body: 4 rows × 2 cols, EQ row spans both cols.
    auto* grid = new QGridLayout;
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(8);

    m_tube   = new StripTubePanel  (m_audio, this);
    m_gate   = new StripGatePanel  (m_audio, this);
    m_eq     = new StripEqPanel    (m_audio, this);
    m_comp   = new StripCompPanel  (m_audio, this);
    m_dess   = new StripDeEssPanel (m_audio, this);
    m_pudu        = new StripPuduPanel        (m_audio, this);
    m_reverb      = new StripReverbPanel      (m_audio, this);
    m_waveform    = new StripWaveformPanel    (m_audio, this);
    m_finalOutput = new StripFinalOutputPanel (m_audio, this);

    // Wrap each panel in a frame so the strip reads as a row of distinct
    // stages rather than one continuous panel.  Object name lets the
    // stylesheet target the wrapper without bleeding into child widgets
    // (Qt stylesheet cascade catches every QFrame descendant otherwise).
    auto wrap = [](QWidget* panel, Qt::Alignment align = Qt::Alignment{}) -> QWidget* {
        auto* frame = new QFrame;
        frame->setObjectName("stripGroupBox");
        auto* lay = new QVBoxLayout(frame);
        lay->setContentsMargins(2, 2, 2, 2);
        lay->setSpacing(0);
        lay->addWidget(panel, 0, align);
        return frame;
    };

    int r = 0;
    grid->addWidget(wrap(m_tube),   r,   0);
    grid->addWidget(wrap(m_gate),   r,   1);
    grid->addWidget(wrap(m_eq),     ++r, 0, 1, 2);  // EQ spans both columns
    grid->addWidget(wrap(m_comp),   ++r, 0);
    grid->addWidget(wrap(m_dess),   r,   1);
    // Both bottom-row panels pin to the top of their frames — the row's
    // height is set by the taller cell elsewhere in the grid (e.g. EQ),
    // and without AlignTop these panels would inflate / centre vertically
    // and leave dead space inside the group box.
    grid->addWidget(wrap(m_pudu,   Qt::AlignTop), ++r, 0);
    grid->addWidget(wrap(m_reverb, Qt::AlignTop),   r, 1);
    // Final-output stage on the LEFT, waveform on the RIGHT — the
    // waveform tap reads post-final-limiter, so visually it makes
    // sense for the meter/limiter to sit to the left of the trace.
    grid->addWidget(wrap(m_finalOutput, Qt::AlignTop), ++r, 0);
    grid->addWidget(wrap(m_waveform,    Qt::AlignTop),   r, 1);

    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);
    // Only the EQ row (index 1) absorbs extra vertical height — its
    // canvas scales meaningfully with size.  Tube/Gate/Comp/DeEss/Pudu
    // /Reverb/Waveform/FinalOutput rows stay at content height so the
    // user doesn't see dead space inside the panel borders when the
    // strip window is taller than the minimum.
    grid->setRowStretch(0, 0);
    grid->setRowStretch(1, 1);
    grid->setRowStretch(2, 0);
    grid->setRowStretch(3, 0);
    grid->setRowStretch(4, 0);

    // Wrap the panel grid in a vertical scroll area so users on
    // 1080p / 1440p displays can shrink the window below the natural
    // content height and scroll the DSP panels.  The chain row above
    // (chainRow) stays sticky as it sits in `body` directly above
    // this scroll area.
    auto* gridHost = new QWidget;
    gridHost->setLayout(grid);
    auto* gridScroll = new QScrollArea;
    gridScroll->setWidget(gridHost);
    gridScroll->setWidgetResizable(true);
    gridScroll->setFrameShape(QFrame::NoFrame);
    gridScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    gridScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    gridScroll->setStyleSheet(
        "QScrollArea { background: transparent; border: none; }"
        "QScrollArea > QWidget > QWidget { background: transparent; }"
        "QScrollBar:vertical { background: #0a1018; width: 10px; "
        "border: none; margin: 0; }"
        "QScrollBar::handle:vertical { background: #2a3744; "
        "border-radius: 5px; min-height: 24px; }"
        "QScrollBar::handle:vertical:hover { background: #4a5562; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical "
        "{ height: 0; border: none; background: none; }"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical "
        "{ background: none; }");

    // RX panel grid (#2425) — same `wrap`-into-frame helper as TX, but
    // 4 rows of [ADSP|AGC-T] / [EQ wide] / [AGC-C|DESS] / [TUBE|EVO].
    // ADSP embeds AetherDspWidget directly (already a reusable widget,
    // shared with AetherDspDialog and the docked applet).  DESS has no
    // RX DSP class yet so it ships as a stub label until that lands.
    auto* gridRx = new QGridLayout;
    gridRx->setContentsMargins(0, 0, 0, 0);
    gridRx->setHorizontalSpacing(8);
    gridRx->setVerticalSpacing(8);

    m_adspRx = new AetherDspWidget(m_audio, this);
    // Match Settings → AetherDSP Settings... — 13 px fonts, full-size
    // controls.  The applet-bar path uses setCompactMode(true) instead.
    m_adspRx->setDialogMode(true);
    m_agcT   = new StripGatePanel  (m_audio, this);
    m_eqRx   = new StripEqPanel    (m_audio, this);
    m_agcC   = new StripCompPanel  (m_audio, this);
    m_dessRx = new StripDeEssPanel (m_audio, this);
    m_tubeRx = new StripTubePanel  (m_audio, this);
    m_evo    = new StripPuduPanel  (m_audio, this);

    int rr = 0;
    gridRx->addWidget(wrap(m_adspRx),                  rr,   0);
    gridRx->addWidget(wrap(m_agcT),                    rr,   1);
    gridRx->addWidget(wrap(m_eqRx),                  ++rr,   0, 1, 2);
    gridRx->addWidget(wrap(m_agcC),                  ++rr,   0);
    gridRx->addWidget(wrap(m_dessRx),                  rr,   1);
    gridRx->addWidget(wrap(m_tubeRx, Qt::AlignTop),  ++rr,   0);
    gridRx->addWidget(wrap(m_evo,    Qt::AlignTop),    rr,   1);
    // 5th row — RX-side counterparts of the TX strip's bottom row:
    // output meter on the LEFT, waveform tap on the RIGHT.
    m_outputRx   = new StripRxOutputPanel (m_audio, this);
    m_waveformRx = new StripWaveformPanel (m_audio, this);
    gridRx->addWidget(wrap(m_outputRx,   Qt::AlignTop), ++rr, 0);
    gridRx->addWidget(wrap(m_waveformRx, Qt::AlignTop),   rr, 1);

    gridRx->setColumnStretch(0, 1);
    gridRx->setColumnStretch(1, 1);
    gridRx->setRowStretch(0, 0);
    gridRx->setRowStretch(1, 1);   // EQ row absorbs extra vertical space
    gridRx->setRowStretch(2, 0);
    gridRx->setRowStretch(3, 0);
    gridRx->setRowStretch(4, 0);   // RX output + waveform stay at content height

    auto* gridRxHost = new QWidget;
    gridRxHost->setLayout(gridRx);
    auto* gridRxScroll = new QScrollArea;
    gridRxScroll->setWidget(gridRxHost);
    gridRxScroll->setWidgetResizable(true);
    gridRxScroll->setFrameShape(QFrame::NoFrame);
    gridRxScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    gridRxScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    gridRxScroll->setStyleSheet(gridScroll->styleSheet());

    // Panel stack: page 0 = TX grid (above), page 1 = RX grid.  Mode
    // toggle swaps the page.
    m_panelStack = new QStackedWidget;
    m_panelStack->addWidget(gridScroll);
    m_panelStack->addWidget(gridRxScroll);

    body->addWidget(m_panelStack, 1);

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

    // Pin each RX panel to its RX engine instance.  Same one-shot
    // initialiser pattern as TX above; subsequent visibility is owned
    // by the panel-stack page swap.
    if (m_tubeRx)     m_tubeRx->showForRx();
    if (m_agcT)       m_agcT->showForRx();
    if (m_eqRx)       m_eqRx->showForPath(ClientEqApplet::Path::Rx);
    if (m_agcC)       m_agcC->showForRx();
    if (m_dessRx)     m_dessRx->showForRx();
    if (m_evo)        m_evo->showForRx();
    if (m_outputRx)   m_outputRx->showForRx();
    if (m_waveformRx) m_waveformRx->showForRx();

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
                        static_cast<QWidget*>(m_reverb),
                        // RX panels (#2425) — same chrome cleanup pass.
                        static_cast<QWidget*>(m_agcT),
                        static_cast<QWidget*>(m_eqRx),
                        static_cast<QWidget*>(m_agcC),
                        static_cast<QWidget*>(m_dessRx),
                        static_cast<QWidget*>(m_tubeRx),
                        static_cast<QWidget*>(m_evo),
                        static_cast<QWidget*>(m_outputRx),
                        static_cast<QWidget*>(m_waveformRx) }) {
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

    // Restore last selected mode (#2425).  Default = TX so users on
    // upgrade keep the existing behaviour.  Toggling the button fires
    // the connected lambda which swaps the chain stack, panel stack,
    // BYPASS target, and title-bar suffix.
    {
        const QString m = AppSettings::instance()
            .value("AetherialStripMode", "TX").toString();
        if (m == "RX" && m_rxBtn) {
            m_rxBtn->setChecked(true);
        }
    }
}

AetherialAudioStrip::~AetherialAudioStrip() = default;

void AetherialAudioStrip::setFramelessMode(bool on)
{
    const QRect geom = geometry();
    const bool wasVisible = isVisible();

    Qt::WindowFlags flags = (windowFlags() & ~Qt::WindowType_Mask) | Qt::Window;
    flags.setFlag(Qt::FramelessWindowHint, on);
    setWindowFlags(flags);
    setGeometry(geom);
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
    m_monRecording = on;
    if (m_monRecBtn) {
        m_monRecBtn->setStyleSheet(on ? QString(kMonBtnBase) + kRecActive
                                      : QString(kMonBtnBase) + kRecIdle);
    }
}

void AetherialAudioStrip::setMonitorPlaying(bool on)
{
    m_monPlaying = on;
    if (m_monPlayBtn) {
        m_monPlayBtn->setStyleSheet(on ? QString(kMonBtnBase) + kPlayActive
                                       : QString(kMonBtnBase) + kPlayIdle);
    }
}

void AetherialAudioStrip::setMonitorHasRecording(bool has)
{
    m_monHasRecording = has;
    if (m_monPlayBtn)
        m_monPlayBtn->setEnabled(has || m_monPlaying);
}

void AetherialAudioStrip::setMicInputReady(bool ready)
{
    if (m_chain) m_chain->setMicInputReady(ready);
}

void AetherialAudioStrip::setTxActive(bool active)
{
    if (m_chain) m_chain->setTxActive(active);
}

void AetherialAudioStrip::setRxPcAudioEnabled(bool on)
{
    if (m_chainRx) m_chainRx->setPcAudioEnabled(on);
}

void AetherialAudioStrip::setRxClientDspActive(bool on, const QString& label)
{
    if (m_chainRx) m_chainRx->setClientDspActive(on, label);
}

void AetherialAudioStrip::setRxOutputUnmuted(bool on)
{
    if (m_chainRx) m_chainRx->setOutputUnmuted(on);
}

void AetherialAudioStrip::refreshChainPaint()
{
    if (m_chain) m_chain->update();
    if (m_chainRx) m_chainRx->update();
}

void AetherialAudioStrip::saveGeometryToSettings()
{
    if (m_restoring) return;
    auto& s = AppSettings::instance();
    s.setValue("AetherialStripGeometry", saveGeometry().toBase64());
    s.save();
}

void AetherialAudioStrip::restoreGeometryFromSettings()
{
    auto& s = AppSettings::instance();
    const QByteArray geom = QByteArray::fromBase64(
        s.value("AetherialStripGeometry").toString().toUtf8());
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
    auto& s = AppSettings::instance();
    s.setValue("AetherialStripVisible", "True");
    s.save();
    QWidget::showEvent(ev);
}

void AetherialAudioStrip::hideEvent(QHideEvent* ev)
{
    auto& s = AppSettings::instance();
    s.setValue("AetherialStripVisible", "False");
    s.save();
    QWidget::hideEvent(ev);
}

Qt::Edges AetherialAudioStrip::edgesAt(const QPoint& pos) const
{
    if (!(windowFlags() & Qt::FramelessWindowHint))
        return {};
    if (isMaximized() || isFullScreen())
        return {};

    Qt::Edges edges;
    if (pos.x() <= kResizeMargin)
        edges |= Qt::LeftEdge;
    else if (pos.x() >= width() - kResizeMargin)
        edges |= Qt::RightEdge;
    if (pos.y() <= kResizeMargin)
        edges |= Qt::TopEdge;
    else if (pos.y() >= height() - kResizeMargin)
        edges |= Qt::BottomEdge;
    return edges;
}

void AetherialAudioStrip::updateResizeCursor(const QPoint& pos)
{
    const Qt::Edges edges = edgesAt(pos);
    Qt::CursorShape shape = Qt::ArrowCursor;
    // Two diagonals → SizeFDiag (↘↖); the other two → SizeBDiag (↙↗).
    if ((edges & (Qt::LeftEdge | Qt::TopEdge))     == (Qt::LeftEdge | Qt::TopEdge)
        || (edges & (Qt::RightEdge | Qt::BottomEdge)) == (Qt::RightEdge | Qt::BottomEdge)) {
        shape = Qt::SizeFDiagCursor;
    } else if ((edges & (Qt::RightEdge | Qt::TopEdge))    == (Qt::RightEdge | Qt::TopEdge)
        ||     (edges & (Qt::LeftEdge | Qt::BottomEdge)) == (Qt::LeftEdge | Qt::BottomEdge)) {
        shape = Qt::SizeBDiagCursor;
    } else if (edges & (Qt::LeftEdge | Qt::RightEdge)) {
        shape = Qt::SizeHorCursor;
    } else if (edges & (Qt::TopEdge | Qt::BottomEdge)) {
        shape = Qt::SizeVerCursor;
    }
    setCursor(shape);
}

void AetherialAudioStrip::mouseMoveEvent(QMouseEvent* ev)
{
    if (!(ev->buttons() & Qt::LeftButton))
        updateResizeCursor(ev->pos());
    QWidget::mouseMoveEvent(ev);
}

void AetherialAudioStrip::mousePressEvent(QMouseEvent* ev)
{
    if (ev->button() == Qt::LeftButton) {
        const Qt::Edges edges = edgesAt(ev->pos());
        if (edges) {
            if (auto* h = windowHandle()) {
                h->startSystemResize(edges);
                ev->accept();
                return;
            }
        }
    }
    QWidget::mousePressEvent(ev);
}

void AetherialAudioStrip::leaveEvent(QEvent* ev)
{
    setCursor(Qt::ArrowCursor);
    QWidget::leaveEvent(ev);
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
    // Chain applet route through setTxBypassed / setRxBypassed and
    // observe the matching *BypassChanged signal to stay in lock-step.
    if (m_rxMode) {
        m_audio->setRxBypassed(checked);
        if (m_chainRx) m_chainRx->update();
    } else {
        m_audio->setTxBypassed(checked);
        if (m_chain) m_chain->update();
    }
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

void AetherialAudioStrip::rebuildPresetCombo(const QString& selectName)
{
    if (!m_presetCombo || !m_presets) return;

    m_buildingCombo = true;
    m_presetCombo->clear();

    int selectIdx = -1;
    const auto names = m_presets->presetNames();
    for (const auto& n : names) {
        m_presetCombo->addItem(n);
        const int row = m_presetCombo->count() - 1;
        m_presetCombo->setItemData(row, n, kRolePresetName);
        if (!selectName.isEmpty() && n == selectName) selectIdx = row;
    }

    // Visual separator — use addSeparator() so it renders as a thin
    // line in the popup.  When the popup is closed and the separator
    // is the *current* item, the combo would render an empty cell;
    // we never let that happen because activating the separator is
    // a no-op (handled in onPresetComboActivated).
    if (!names.isEmpty()) m_presetCombo->insertSeparator(m_presetCombo->count());

    m_presetCombo->addItem(QString::fromUtf8("Import\xe2\x80\xa6"));
    m_presetCombo->setItemData(m_presetCombo->count() - 1,
                               kActionImport, kRoleAction);

    m_presetCombo->addItem(QString::fromUtf8("Export Preset\xe2\x80\xa6"));
    m_presetCombo->setItemData(m_presetCombo->count() - 1,
                               kActionExportPreset, kRoleAction);

    m_presetCombo->addItem(QString::fromUtf8("Export Library\xe2\x80\xa6"));
    m_presetCombo->setItemData(m_presetCombo->count() - 1,
                               kActionExportLibrary, kRoleAction);

    if (selectIdx >= 0) {
        m_presetCombo->setCurrentIndex(selectIdx);
        m_currentPresetName = selectName;
    } else if (!m_currentPresetName.isEmpty()) {
        // Active preset was deleted — reset display.
        m_presetCombo->setCurrentIndex(-1);
        m_presetCombo->setEditText(QString());
        m_currentPresetName.clear();
    } else {
        m_presetCombo->setCurrentIndex(-1);
    }

    if (m_presetCombo->currentIndex() < 0 && names.isEmpty()) {
        // No presets stored yet — show placeholder text.
        m_presetCombo->setPlaceholderText(
            QString::fromUtf8("Presets\xe2\x80\xa6"));
    } else if (m_presetCombo->currentIndex() < 0) {
        m_presetCombo->setPlaceholderText(
            QString::fromUtf8("Pick a preset\xe2\x80\xa6"));
    }

    m_buildingCombo = false;
    updatePresetButtonEnable();
}

void AetherialAudioStrip::updatePresetButtonEnable()
{
    // Delete is only meaningful when a stored preset is selected — i.e.
    // m_currentPresetName names a preset that still exists in the
    // library.  Save is always enabled (it can name a new preset or
    // overwrite the active one).
    const bool hasActive = m_presets
        && !m_currentPresetName.isEmpty()
        && m_presets->hasPreset(m_currentPresetName);
    if (m_presetDeleteBtn) m_presetDeleteBtn->setEnabled(hasActive);
}

void AetherialAudioStrip::onPresetComboActivated(int idx)
{
    if (m_buildingCombo || !m_presetCombo || !m_presets || idx < 0) return;

    const auto actionVar = m_presetCombo->itemData(idx, kRoleAction);
    if (actionVar.isValid()) {
        const int action = actionVar.toInt();
        // Reset display to the active preset (if any) so the action
        // label doesn't get stuck in the combo's edit field.
        rebuildPresetCombo(m_currentPresetName);
        if (action == kActionImport)              doImportPreset();
        else if (action == kActionExportPreset)   doExportPreset();
        else if (action == kActionExportLibrary)  doExportLibrary();
        return;
    }

    const auto nameVar = m_presetCombo->itemData(idx, kRolePresetName);
    if (!nameVar.isValid()) return;
    const QString name = nameVar.toString();
    if (m_presets->loadPreset(name)) {
        m_currentPresetName = name;
        AppSettings::instance().setValue("AetherialStripActivePreset", name);
        refreshAllPanelsFromEngine();
    }
    updatePresetButtonEnable();
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
    if (m_chain)  m_chain->update();
}

void AetherialAudioStrip::doImportPreset()
{
    const QString defaultDir = QStandardPaths::writableLocation(
        QStandardPaths::DocumentsLocation);
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Import Channel Strip Preset"),
        defaultDir,
        tr("Channel strip preset (*.json *.aetherpreset);;All files (*)"));
    if (path.isEmpty()) return;

    const QString imported = m_presets->importPresetFromFile(path);
    if (imported.isEmpty()) {
        QMessageBox::warning(this, tr("Import failed"),
            tr("Could not read a valid channel-strip preset from:\n%1")
                .arg(path));
        return;
    }
    // Apply it immediately so the user sees the imported preset live.
    if (m_presets->loadPreset(imported)) {
        m_currentPresetName = imported;
        AppSettings::instance().setValue("AetherialStripActivePreset", imported);
        refreshAllPanelsFromEngine();
    }
    rebuildPresetCombo(m_currentPresetName);
}

void AetherialAudioStrip::doExportPreset()
{
    // Ask the user for a preset name + destination path.  The name
    // gets baked into the file body, so peers who import it see the
    // author's intended name even if the filename is changed later.
    bool ok = false;
    const QString defaultName = m_currentPresetName.isEmpty()
        ? tr("My Preset")
        : m_currentPresetName;
    const QString name = QInputDialog::getText(
        this,
        tr("Export Channel Strip Preset"),
        tr("Preset name (will be saved into the file and added to your "
           "local library):"),
        QLineEdit::Normal, defaultName, &ok);
    if (!ok || name.trimmed().isEmpty()) return;

    const QString defaultDir = QStandardPaths::writableLocation(
        QStandardPaths::DocumentsLocation);
    QString suggested = defaultDir + "/" + name + ".json";
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export Channel Strip Preset"),
        suggested,
        tr("Channel strip preset (*.json);;All files (*)"));
    if (path.isEmpty()) return;

    // Save into the local library first so the new preset shows up
    // in the dropdown straight away, *then* write it out for sharing
    // (the on-disk file mirrors what Import would round-trip back).
    if (!m_presets->savePresetFromCurrent(name)) {
        QMessageBox::warning(this, tr("Export failed"),
            tr("Could not save preset to local library."));
        return;
    }
    if (!m_presets->exportPresetToFile(name, path)) {
        QMessageBox::warning(this, tr("Export failed"),
            tr("Could not write preset file:\n%1").arg(path));
        return;
    }
    m_currentPresetName = name;
    AppSettings::instance().setValue("AetherialStripActivePreset", name);
    rebuildPresetCombo(m_currentPresetName);
}

void AetherialAudioStrip::doExportLibrary()
{
    if (!m_presets) return;
    if (m_presets->presetNames().isEmpty()) {
        QMessageBox::information(this, tr("Export Library"),
            tr("Your local preset library is empty — save at least one "
               "preset before exporting."));
        return;
    }

    const QString defaultDir = QStandardPaths::writableLocation(
        QStandardPaths::DocumentsLocation);
    const QString suggested = defaultDir + "/AetherSDR-ChannelStrip-Library.json";
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export Channel Strip Library"),
        suggested,
        tr("Channel strip preset library (*.json);;All files (*)"));
    if (path.isEmpty()) return;

    if (!m_presets->exportLibraryToFile(path)) {
        QMessageBox::warning(this, tr("Export failed"),
            tr("Could not write library file:\n%1").arg(path));
    }
}

void AetherialAudioStrip::doSavePreset()
{
    if (!m_presets) return;

    bool ok = false;
    const QString prefill = m_currentPresetName;  // empty if nothing active
    const QString entered = QInputDialog::getText(
        this,
        tr("Save Channel Strip Preset"),
        tr("Preset name:"),
        QLineEdit::Normal,
        prefill,
        &ok);
    if (!ok) return;
    const QString name = entered.trimmed();
    if (name.isEmpty()) return;

    // Same name as the currently-active preset → overwrite confirmation.
    // Different name OR no preset was active → save straight through (no
    // prompt) even if the new name happens to collide with another stored
    // preset, per spec ("If they change the name then it should be stored
    // as a new preset, no confirmation required").
    if (!prefill.isEmpty() && name == prefill && m_presets->hasPreset(name)) {
        const auto reply = QMessageBox::question(
            this,
            tr("Overwrite Preset?"),
            tr("A preset named \"%1\" already exists.  Overwrite it with "
               "the current channel-strip settings?").arg(name),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (reply != QMessageBox::Yes) return;
    }

    if (!m_presets->savePresetFromCurrent(name)) {
        QMessageBox::warning(this, tr("Save failed"),
            tr("Could not write preset to local library."));
        return;
    }
    m_currentPresetName = name;
    AppSettings::instance().setValue("AetherialStripActivePreset", name);
    rebuildPresetCombo(m_currentPresetName);
}

void AetherialAudioStrip::doDeletePreset()
{
    if (!m_presets || m_currentPresetName.isEmpty()) return;
    if (!m_presets->hasPreset(m_currentPresetName)) return;

    const auto reply = QMessageBox::question(
        this,
        tr("Delete Preset?"),
        tr("Delete the preset \"%1\" from your local library?  This "
           "cannot be undone.").arg(m_currentPresetName),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    const QString deleted = m_currentPresetName;
    if (!m_presets->deletePreset(deleted)) {
        QMessageBox::warning(this, tr("Delete failed"),
            tr("Could not remove preset from local library."));
        return;
    }
    m_currentPresetName.clear();
    AppSettings::instance().remove("AetherialStripActivePreset");
    rebuildPresetCombo();
}

} // namespace AetherSDR
