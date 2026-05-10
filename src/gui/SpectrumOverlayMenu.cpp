#include "SpectrumOverlayMenu.h"
#include "DspParamPopup.h"
#include "MemoryBrowsePanel.h"
#include "SpectrumWidget.h"
#include "GuardedSlider.h"
#include "ComboStyle.h"
#include "models/SliceModel.h"
#include "models/BandDefs.h"
#include "core/AppSettings.h"

#include <QPushButton>
#include <QComboBox>
#include <QSlider>
#include <QLabel>
#include <QGridLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSignalBlocker>
#include <QEvent>
#include <QFrame>
#include <QColorDialog>

namespace AetherSDR {

static constexpr int BTN_W = 68;
static constexpr int BTN_H = 22;

// Band button size (slightly smaller for the grid)
static constexpr int BAND_BTN_W = 48;
static constexpr int BAND_BTN_H = 26;

static const QString kPanelStyle =
    "QWidget { background: rgba(15, 15, 26, 220); "
    "border: 1px solid #304050; border-radius: 3px; }";

static const QString kLabelStyle =
    "QLabel { background: transparent; border: none; "
    "color: #8aa8c0; font-size: 10px; font-weight: bold; }";

static const QString kSliderStyle =
    "QSlider::groove:horizontal { background: #1a2a3a; height: 4px; "
    "border-radius: 2px; }"
    "QSlider::handle:horizontal { background: #c8d8e8; width: 10px; "
    "margin: -4px 0; border-radius: 5px; }";

static const QString kMenuBtnNormal =
    "QPushButton { background: rgba(20, 30, 45, 240); "
    "border: 1px solid rgba(255, 255, 255, 40); border-radius: 2px; "
    "color: #c8d8e8; font-size: 11px; font-weight: bold; }"
    "QPushButton:hover { background: rgba(0, 112, 192, 180); "
    "border: 1px solid #0090e0; }";

static const QString kMenuBtnActive =
    "QPushButton { background: rgba(0, 112, 192, 180); "
    "border: 1px solid #0090e0; border-radius: 2px; "
    "color: #ffffff; font-size: 11px; font-weight: bold; }";

static QPushButton* makeMenuBtn(const QString& text, QWidget* parent)
{
    auto* btn = new QPushButton(text, parent);
    btn->setFixedSize(BTN_W, BTN_H);
    btn->setStyleSheet(kMenuBtnNormal);
    return btn;
}

// Band button grid uses shared BandDefs + special entries.
// Index into this array for the grid layout below.
struct BandGridEntry {
    const char* label;
    const char* bandName;  // key for BandSettings (e.g. "20m")
    double freqMhz;
    const char* mode;
};

static constexpr BandGridEntry BAND_GRID[] = {
    {"160",  "160m",  1.900,  "LSB"},   // 0
    {"80",   "80m",   3.800,  "LSB"},   // 1
    {"60",   "60m",   5.357,  "USB"},   // 2
    {"40",   "40m",   7.200,  "LSB"},   // 3
    {"30",   "30m",  10.125,  "DIGU"},  // 4
    {"20",   "20m",  14.225,  "USB"},   // 5
    {"17",   "17m",  18.130,  "USB"},   // 6
    {"15",   "15m",  21.300,  "USB"},   // 7
    {"12",   "12m",  24.950,  "USB"},   // 8
    {"10",   "10m",  28.400,  "USB"},   // 9
    {"6",    "6m",   50.150,  "USB"},   // 10
    {"WWV",  "WWV",  10.000,  "AM"},    // 11
    {"GEN",  "GEN",   0.500,  "AM"},    // 12
    {"2200", "2200m", 0.1375, "CW"},    // 13
    {"630",  "630m",  0.475,  "CW"},    // 14
    {"XVTR", "",      0.0,    ""},      // 15
};

SpectrumOverlayMenu::SpectrumOverlayMenu(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_TransparentForMouseEvents, false);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAutoFillBackground(false);

    // Toggle button (arrow)
    m_toggleBtn = new QPushButton(this);
    m_toggleBtn->setFixedSize(BTN_W, BTN_H);
    m_toggleBtn->setStyleSheet(
        "QPushButton { background: rgba(20, 30, 45, 240); "
        "border: 1px solid rgba(255, 255, 255, 40); border-radius: 2px; "
        "color: #c8d8e8; font-size: 13px; font-weight: bold; }"
        "QPushButton:hover { background: rgba(0, 112, 192, 180); "
        "border: 1px solid #0090e0; }");
    connect(m_toggleBtn, &QPushButton::clicked, this, &SpectrumOverlayMenu::toggle);

    // Menu buttons — Band, ANT, DSP handled specially (sub-panels)
    struct BtnDef { QString text; int specialIdx; void (SpectrumOverlayMenu::*sig)(); };
    const BtnDef defs[] = {
        {"+RX",      -1, nullptr},   // 0 — handled separately (signal has panId arg)
        {"+TNF",     -1, &SpectrumOverlayMenu::addTnfClicked},  // 1
        {"Band",      0, nullptr},   // 2 — toggleBandPanel
        {"ANT",       1, nullptr},   // 3 — toggleAntPanel
        {"Display",   4, nullptr},   // 4 — toggleDisplayPanel
        {"Memory",    5, nullptr},   // 6 — toggleMemoryPanel
        // MEM+ moved into MemoryBrowsePanel (bottom button — doesn't scroll).
        {"DAX",       3, nullptr},   // 6 — toggleDaxPanel
    };

    for (const auto& def : defs) {
        auto* btn = makeMenuBtn(def.text, this);
        if (def.specialIdx == 0)
            connect(btn, &QPushButton::clicked, this, &SpectrumOverlayMenu::toggleBandPanel);
        else if (def.specialIdx == 1)
            connect(btn, &QPushButton::clicked, this, &SpectrumOverlayMenu::toggleAntPanel);
        else if (def.specialIdx == 3)
            connect(btn, &QPushButton::clicked, this, &SpectrumOverlayMenu::toggleDaxPanel);
        else if (def.specialIdx == 4)
            connect(btn, &QPushButton::clicked, this, &SpectrumOverlayMenu::toggleDisplayPanel);
        else if (def.specialIdx == 5)
            connect(btn, &QPushButton::clicked, this, &SpectrumOverlayMenu::toggleMemoryPanel);
        else if (def.text == "+RX")
            connect(btn, &QPushButton::clicked, this, [this]() { emit addRxClicked(m_panId); });
        else if (def.sig)
            connect(btn, &QPushButton::clicked, this, def.sig);
        m_menuBtns.append(btn);
    }

    // Menu button tooltips
    if (m_menuBtns.size() >= 7) {
        m_menuBtns[kBtnAddRx]->setToolTip("Add a new receive slice on this panadapter.");
        m_menuBtns[kBtnAddTnf]->setToolTip("Add a tracking notch filter at the current frequency.");
        m_menuBtns[kBtnBand]->setToolTip("Open band selector.");
        m_menuBtns[kBtnAnt]->setToolTip("Open antenna and RF gain controls.");
        m_menuBtns[kBtnDisplay]->setToolTip("Open panadapter and waterfall display settings.");
        m_menuBtns[kBtnMemoryBrowse]->setToolTip("Browse saved memories for quick recall.");
        m_menuBtns[kBtnDax]->setToolTip("Open DAX audio routing channel selector.");
    }

    buildBandPanel();
    buildAntPanel();
    buildDaxPanel();
    buildDisplayPanel();
    buildMemoryPanel();

    // Prevent mouse/wheel events from falling through panels to the spectrum
    for (auto* panel : {m_bandPanel, m_antPanel, m_daxPanel, m_displayPanel,
                        static_cast<QWidget*>(m_memoryPanel)})
        if (panel) panel->installEventFilter(this);

    updateLayout();
}

void SpectrumOverlayMenu::raiseAll()
{
    raise();
    if (m_bandPanel)    m_bandPanel->raise();
    if (m_xvtrPanel)    m_xvtrPanel->raise();
    if (m_antPanel)     m_antPanel->raise();
    if (m_daxPanel)     m_daxPanel->raise();
    if (m_displayPanel) m_displayPanel->raise();
    if (m_memoryPanel)  m_memoryPanel->raise();
}

void SpectrumOverlayMenu::setMemories(const QMap<int, MemoryEntry>& memories)
{
    if (m_memoryPanel)
        m_memoryPanel->setMemories(memories);
}

// ── Band sub-panel ────────────────────────────────────────────────────────────

void SpectrumOverlayMenu::buildBandPanel()
{
    m_bandPanel = new QWidget(parentWidget());
    m_bandPanel->setStyleSheet("QWidget { background: rgba(15, 15, 26, 220); "
                                "border: 1px solid #304050; border-radius: 3px; }");
    m_bandPanel->hide();

    auto* grid = new QGridLayout(m_bandPanel);
    grid->setContentsMargins(2, 2, 2, 2);
    grid->setSpacing(2);

    const QString bandBtnStyle =
        "QPushButton { background: rgba(30, 40, 55, 220); "
        "border: 1px solid #304050; border-radius: 3px; "
        "color: #c8d8e8; font-size: 11px; font-weight: bold; }"
        "QPushButton:hover { background: rgba(0, 112, 192, 180); "
        "border: 1px solid #0090e0; }";

    constexpr int layout[][3] = {
        {0, 1, 2},      // 160, 80, 60
        {3, 4, 5},      // 40, 30, 20
        {6, 7, 8},      // 17, 15, 12
        {9, 10, -1},    // 10, 6
        {11, 12, -1},   // WWV, GEN
        {13, 14, 15},   // 2200, 630, XVTR
    };

    for (int row = 0; row < 6; ++row) {
        for (int col = 0; col < 3; ++col) {
            int idx = layout[row][col];
            if (idx < 0) continue;

            auto* btn = new QPushButton(BAND_GRID[idx].label, m_bandPanel);
            btn->setFixedSize(BAND_BTN_W, BAND_BTN_H);
            btn->setStyleSheet(bandBtnStyle);

            QString bandName = QString::fromLatin1(BAND_GRID[idx].bandName);
            double freq = BAND_GRID[idx].freqMhz;
            QString mode = QString::fromLatin1(BAND_GRID[idx].mode);
            if (idx == 15) {
                // XVTR button → open Radio Setup XVTR tab (#571)
                connect(btn, &QPushButton::clicked, this, [this]() {
                    hideAllSubPanels();
                    emit xvtrSetupRequested();
                });
            } else if (bandName.isEmpty()) {
                btn->setEnabled(false);
            } else {
                connect(btn, &QPushButton::clicked, this, [this, bandName, freq, mode]() {
                    hideAllSubPanels();
                    emit bandSelected(bandName, freq, mode);
                });
            }

            grid->addWidget(btn, row, col);
        }
    }

    m_bandPanel->adjustSize();
}

// ── ANT sub-panel ─────────────────────────────────────────────────────────────

void SpectrumOverlayMenu::buildAntPanel()
{
    m_antPanel = new QWidget(parentWidget());
    m_antPanel->setStyleSheet(kPanelStyle);
    m_antPanel->hide();

    auto* vbox = new QVBoxLayout(m_antPanel);
    vbox->setContentsMargins(6, 6, 6, 6);
    vbox->setSpacing(4);

    constexpr int kLabelW = 48;
    constexpr int kValueW = 20;

    // RX ANT row
    auto* antRow = new QHBoxLayout;
    antRow->setSpacing(4);
    auto* antLabel = new QLabel("RX ANT:");
    antLabel->setStyleSheet(kLabelStyle);
    antLabel->setFixedWidth(kLabelW);
    antRow->addWidget(antLabel);
    m_rxAntCmb = new GuardedComboBox;
    AetherSDR::applyComboStyle(m_rxAntCmb);
    antRow->addWidget(m_rxAntCmb, 1);
    vbox->addLayout(antRow);

    connect(m_rxAntCmb, &QComboBox::currentTextChanged,
            this, [this](const QString& ant) {
        if (!m_updatingFromModel && m_slice && !ant.isEmpty())
            m_slice->setRxAntenna(ant);
    });

    // RF Gain row
    auto* gainRow = new QHBoxLayout;
    gainRow->setSpacing(4);
    auto* gainLabel = new QLabel("RF Gain:");
    gainLabel->setStyleSheet(kLabelStyle);
    gainLabel->setFixedWidth(kLabelW);
    gainRow->addWidget(gainLabel);
    m_rfGainSlider = new GuardedSlider(Qt::Horizontal);
    m_rfGainSlider->setRange(-8, 32);
    m_rfGainSlider->setSingleStep(8);
    m_rfGainSlider->setPageStep(8);
    m_rfGainSlider->setTickInterval(8);
    m_rfGainSlider->setTickPosition(QSlider::TicksBelow);
    m_rfGainSlider->setStyleSheet(kSliderStyle);
    m_rfGainSlider->setToolTip("RF Gain: −8 to +32 dB (8 dB steps)\n"
                               "Step size is determined by radio hardware.");
    gainRow->addWidget(m_rfGainSlider, 1);
    m_rfGainLabel = new QLabel("0 dB");
    m_rfGainLabel->setStyleSheet(kLabelStyle);
    m_rfGainLabel->setFixedWidth(36);
    m_rfGainLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    gainRow->addWidget(m_rfGainLabel);
    vbox->addLayout(gainRow);

    connect(m_rfGainSlider, &QSlider::valueChanged, this, [this](int v) {
        // Snap to nearest multiple of step size
        int step = m_rfGainSlider->singleStep();
        if (step < 1) step = 1;
        int snapped = qRound(static_cast<double>(v) / step) * step;
        if (snapped != v) {
            QSignalBlocker sb(m_rfGainSlider);
            m_rfGainSlider->setValue(snapped);
        }
        m_rfGainLabel->setText(QString("%1 dB").arg(snapped));
        // Only emit when the snapped value actually differs from the last
        // emitted one. Mouse drags within a single step fire valueChanged
        // with many unsnapped values that all round to the same snapped
        // value — without this guard we'd spam rfgain commands to the
        // radio on every drag tick (#1498).
        if (!m_updatingFromModel && snapped != m_lastEmittedRfGain) {
            m_lastEmittedRfGain = snapped;
            emit rfGainChanged(snapped);
            if (m_slice)
                m_slice->setRfGain(static_cast<float>(snapped));
        }
    });

    // WNB row: toggle button + level slider
    auto* wnbRow = new QHBoxLayout;
    wnbRow->setSpacing(4);
    m_wnbBtn = new QPushButton("WNB");
    m_wnbBtn->setCheckable(true);
    m_wnbBtn->setFixedSize(48, 22);
    m_wnbBtn->setStyleSheet(
        "QPushButton { background: #1a2a3a; border: 1px solid #304050; "
        "border-radius: 2px; color: #c8d8e8; font-size: 11px; font-weight: bold; }"
        "QPushButton:checked { background: #0070c0; color: #ffffff; "
        "border: 1px solid #0090e0; }"
        "QPushButton:hover { border: 1px solid #0090e0; }");
    wnbRow->addWidget(m_wnbBtn);
    m_wnbSlider = new GuardedSlider(Qt::Horizontal);
    m_wnbSlider->setRange(0, 100);
    m_wnbSlider->setValue(50);
    m_wnbSlider->setStyleSheet(kSliderStyle);
    wnbRow->addWidget(m_wnbSlider, 1);
    m_wnbLabel = new QLabel("50");
    m_wnbLabel->setStyleSheet(kLabelStyle);
    m_wnbLabel->setFixedWidth(kValueW);
    m_wnbLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    wnbRow->addWidget(m_wnbLabel);
    vbox->addLayout(wnbRow);

    connect(m_wnbBtn, &QPushButton::toggled, this, &SpectrumOverlayMenu::wnbToggled);
    connect(m_wnbSlider, &QSlider::valueChanged, this, [this](int v) {
        m_wnbLabel->setText(QString::number(v));
        emit wnbLevelChanged(v);
    });

    const QString sweepBtnStyle =
        "QPushButton { background: rgba(38, 34, 24, 235); "
        "border: 1px solid #705820; border-radius: 2px; "
        "color: #ffd070; font-size: 10px; font-weight: bold; padding: 1px 4px; }"
        "QPushButton:hover { background: rgba(120, 80, 20, 210); "
        "border: 1px solid #ffc040; color: #ffffff; }"
        "QPushButton:disabled { color: #706858; border-color: #403828; }";
    auto* sweepRow = new QHBoxLayout;
    sweepRow->setSpacing(4);
    m_swrStartBtn = new QPushButton("Start Sweep");
    m_swrStartBtn->setMinimumHeight(22);
    m_swrStartBtn->setStyleSheet(sweepBtnStyle);
    m_swrClearBtn = new QPushButton("Clear Sweep");
    m_swrClearBtn->setMinimumHeight(22);
    m_swrClearBtn->setStyleSheet(sweepBtnStyle);
    sweepRow->addWidget(m_swrStartBtn, 1);
    sweepRow->addWidget(m_swrClearBtn, 1);
    vbox->addLayout(sweepRow);

    auto* sweepPowerRow = new QHBoxLayout;
    sweepPowerRow->setSpacing(4);
    auto* sweepPowerTitle = new QLabel("PWR");
    sweepPowerTitle->setStyleSheet(kLabelStyle);
    sweepPowerTitle->setFixedWidth(28);
    sweepPowerRow->addWidget(sweepPowerTitle);
    m_swrPowerSlider = new GuardedSlider(Qt::Horizontal);
    m_swrPowerSlider->setRange(1, 10);
    m_swrPowerSlider->setValue(1);
    m_swrPowerSlider->setStyleSheet(kSliderStyle);
    sweepPowerRow->addWidget(m_swrPowerSlider, 1);
    m_swrPowerLabel = new QLabel("1 W");
    m_swrPowerLabel->setStyleSheet(kLabelStyle);
    m_swrPowerLabel->setFixedWidth(kValueW);
    m_swrPowerLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    sweepPowerRow->addWidget(m_swrPowerLabel);
    vbox->addLayout(sweepPowerRow);

    connect(m_swrStartBtn, &QPushButton::clicked, this, [this]() {
        const int sliceId = m_slice ? m_slice->sliceId() : -1;
        const int watts = m_swrPowerSlider ? m_swrPowerSlider->value() : 1;
        hideAllSubPanels();
        emit swrSweepStartRequested(sliceId, watts);
    });
    connect(m_swrClearBtn, &QPushButton::clicked, this, [this]() {
        hideAllSubPanels();
        emit swrSweepClearRequested();
    });
    connect(m_swrPowerSlider, &QSlider::valueChanged, this, [this](int watts) {
        watts = qBound(1, watts, 10);
        if (m_swrPowerLabel)
            m_swrPowerLabel->setText(QString("%1 W").arg(watts));
        emit swrSweepPowerChanged(watts);
    });

    // ANT panel tooltips
    m_rxAntCmb->setToolTip("Select the receive antenna port for this slice.");
    m_rfGainSlider->setToolTip("Adjusts receiver IF gain. Lower values reduce strong-signal overload.");
    m_wnbBtn->setToolTip("Wideband noise blanker \u2014 suppresses correlated impulse noise across the full panadapter bandwidth.");
    m_wnbSlider->setToolTip("Adjusts WNB threshold. Higher values blank more aggressively.");
    m_swrStartBtn->setToolTip("Run a low-power tune sweep across the current TX band and plot SWR on the panadapter.");
    m_swrClearBtn->setToolTip("Clear the displayed SWR sweep trace.");
    m_swrPowerSlider->setToolTip("Sets the low-power tune carrier used for SWR sweeps.");

    m_antPanel->setFixedWidth(180);
    m_antPanel->adjustSize();
}

void SpectrumOverlayMenu::setAntennaList(const QStringList& ants)
{
    m_antList = ants;
    QSignalBlocker sb(m_rxAntCmb);
    QString cur = m_rxAntCmb->currentText();
    m_rxAntCmb->clear();
    m_rxAntCmb->addItems(ants);
    if (ants.contains(cur))
        m_rxAntCmb->setCurrentText(cur);
}

void SpectrumOverlayMenu::setSlice(SliceModel* slice)
{
    if (m_slice)
        m_slice->disconnect(this);
    m_slice = slice;
    if (!m_slice) return;

    connect(m_slice, &SliceModel::rxAntennaChanged, this, [this](const QString& ant) {
        m_updatingFromModel = true;
        m_rxAntCmb->setCurrentText(ant);
        m_updatingFromModel = false;
    });

    connect(m_slice, &SliceModel::rfGainChanged, this, [this](float gain) {
        m_updatingFromModel = true;
        QSignalBlocker sb(m_rfGainSlider);
        m_rfGainSlider->setValue(static_cast<int>(gain));
        m_rfGainLabel->setText(QString("%1 dB").arg(static_cast<int>(gain)));
        m_updatingFromModel = false;
    });

    // DSP toggle/level wiring lived here when the overlay carried a DSP
    // sub-panel.  Slice DSP now lives on VfoWidget (radio-side) and the
    // AetherDSP applet (client-side) — those widgets own their own slice
    // bindings.

    syncAntPanel();
    syncDaxPanel();
}

void SpectrumOverlayMenu::syncAntPanel()
{
    if (!m_slice) return;
    m_updatingFromModel = true;
    m_rxAntCmb->setCurrentText(m_slice->rxAntenna());
    {
        QSignalBlocker sb(m_rfGainSlider);
        m_rfGainSlider->setValue(static_cast<int>(m_slice->rfGain()));
    }
    m_rfGainLabel->setText(QString("%1 dB").arg(static_cast<int>(m_slice->rfGain())));
    m_updatingFromModel = false;
}

// ── DAX sub-panel ─────────────────────────────────────────────────────────────

void SpectrumOverlayMenu::buildDaxPanel()
{
    m_daxPanel = new QWidget(parentWidget());
    m_daxPanel->setStyleSheet(kPanelStyle);
    m_daxPanel->hide();

    auto* vb = new QVBoxLayout(m_daxPanel);
    vb->setContentsMargins(6, 6, 6, 6);
    vb->setSpacing(4);

    auto* iqRow = new QHBoxLayout;
    iqRow->setSpacing(4);
    auto* iqLbl = new QLabel("IQ Ch");
    iqLbl->setStyleSheet(kLabelStyle);
    iqRow->addWidget(iqLbl);
    m_daxIqCmb = new GuardedComboBox;
    m_daxIqCmb->addItems({"None", "1", "2", "3", "4"});
    AetherSDR::applyComboStyle(m_daxIqCmb);
    iqRow->addWidget(m_daxIqCmb, 1);
    vb->addLayout(iqRow);

    connect(m_daxIqCmb, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int idx) {
        if (!m_updatingFromModel)
            emit daxIqChannelChanged(idx);
    });

    m_daxPanel->setFixedWidth(140);
    m_daxPanel->adjustSize();
}

void SpectrumOverlayMenu::syncDaxPanel()
{
    // DAX IQ combo is synced via syncDaxIqChannel() from PanadapterModel.
    // Regular DAX channel is managed by the VFO widget.
}

void SpectrumOverlayMenu::syncDaxIqChannel(int channel)
{
    if (!m_daxIqCmb) return;
    QSignalBlocker sb(m_daxIqCmb);
    int idx = qBound(0, channel, m_daxIqCmb->count() - 1);
    m_daxIqCmb->setCurrentIndex(idx);
}

void SpectrumOverlayMenu::toggleDaxPanel()
{
    bool wasVisible = m_daxPanelVisible;
    hideAllSubPanels();
    if (!wasVisible) {
        syncDaxPanel();
        m_daxPanelVisible = true;
        int btnCenterY = m_menuBtns[kBtnDax]->y() + m_menuBtns[kBtnDax]->height() / 2;
        int panelY = y() + btnCenterY - m_daxPanel->sizeHint().height() / 2;
        m_daxPanel->move(x() + width(), std::max(0, panelY));
        m_daxPanel->raise();
        m_daxPanel->show();
        m_menuBtns[kBtnDax]->setStyleSheet(kMenuBtnActive);
    }
}

void SpectrumOverlayMenu::buildMemoryPanel()
{
    m_memoryPanel = new MemoryBrowsePanel(parentWidget());
    m_memoryPanel->hide();
    connect(m_memoryPanel, &MemoryBrowsePanel::memoryActivated, this,
            [this](int memoryIndex) {
        hideAllSubPanels();
        emit memoryActivated(memoryIndex, m_panId);
    });
    connect(m_memoryPanel, &MemoryBrowsePanel::quickAddRequested, this, [this]() {
        hideAllSubPanels();
        emit quickAddMemoryRequested(m_panId);
    });
}

void SpectrumOverlayMenu::toggleMemoryPanel()
{
    const bool wasVisible = m_memoryPanelVisible;
    hideAllSubPanels();
    if (!wasVisible && m_memoryPanel) {
        m_memoryPanelVisible = true;
        const int panelH = m_memoryPanel->sizeHint().height();
        int panelY = y();
        if (auto* parent = parentWidget())
            panelY = std::clamp(panelY, 0, qMax(0, parent->height() - panelH));
        else
            panelY = std::max(0, panelY);
        m_memoryPanel->move(x() + width(), panelY);
        m_memoryPanel->raise();
        m_memoryPanel->show();
        m_menuBtns[kBtnMemoryBrowse]->setStyleSheet(kMenuBtnActive);
        m_memoryPanel->focusClosestToFrequency(m_slice ? m_slice->frequency() : -1.0);
    }
}

// ── Sub-panel toggle helpers ──────────────────────────────────────────────────

void SpectrumOverlayMenu::hideAllSubPanels()
{
    m_bandPanelVisible = false;
    if (m_bandPanel) m_bandPanel->hide();
    m_xvtrPanelVisible = false;
    if (m_xvtrPanel) m_xvtrPanel->hide();
    m_antPanelVisible = false;
    if (m_antPanel) m_antPanel->hide();
    m_daxPanelVisible = false;
    if (m_daxPanel) m_daxPanel->hide();
    m_displayPanelVisible = false;
    if (m_displayPanel) m_displayPanel->hide();
    m_memoryPanelVisible = false;
    if (m_memoryPanel) m_memoryPanel->hide();
    for (int idx : {kBtnBand, kBtnAnt, kBtnDisplay,
                    kBtnMemoryBrowse, kBtnDax}) {
        if (idx >= 0 && idx < m_menuBtns.size())
            m_menuBtns[idx]->setStyleSheet(kMenuBtnNormal);
    }
}

void SpectrumOverlayMenu::showBandPanelAt(const QPoint& pos)
{
    if (!m_bandPanel)
        return;

    m_bandPanelVisible = true;
    m_bandPanel->move(pos);
    m_bandPanel->raise();
    m_bandPanel->show();
    m_menuBtns[kBtnBand]->setStyleSheet(kMenuBtnActive);
}

void SpectrumOverlayMenu::toggleBandPanel()
{
    const bool wasVisible = m_bandPanel && m_bandPanel->isVisible();
    hideAllSubPanels();
    if (!wasVisible)
        showBandPanelAt(QPoint(x() + width(), y()));
}

void SpectrumOverlayMenu::toggleAntPanel()
{
    bool wasVisible = m_antPanelVisible;
    hideAllSubPanels();
    if (!wasVisible) {
        syncAntPanel();
        m_antPanelVisible = true;
        int antBtnCenterY = m_menuBtns[kBtnAnt]->y() + m_menuBtns[kBtnAnt]->height() / 2;
        int panelY = y() + antBtnCenterY - m_antPanel->sizeHint().height() / 2;
        m_antPanel->move(x() + width(), std::max(0, panelY));
        m_antPanel->raise();
        m_antPanel->show();
        m_menuBtns[kBtnAnt]->setStyleSheet(kMenuBtnActive);
    }
}

// ── Main menu toggle and layout ───────────────────────────────────────────────

void SpectrumOverlayMenu::toggle()
{
    m_expanded = !m_expanded;
    if (!m_expanded)
        hideAllSubPanels();
    updateLayout();
}

void SpectrumOverlayMenu::updateLayout()
{
    constexpr int pad = 2;
    constexpr int gap = 2;

    m_toggleBtn->setText(m_expanded ? QStringLiteral("\u2190") : QStringLiteral("\u2192"));
    m_toggleBtn->move(pad, pad);

    int y = pad + BTN_H + gap;
    for (auto* btn : m_menuBtns) {
        btn->setVisible(m_expanded);
        if (m_expanded) {
            btn->move(pad, y);
            y += BTN_H + gap;
        }
    }

    int totalH = m_expanded ? (pad + BTN_H + gap + m_menuBtns.size() * (BTN_H + gap))
                            : (pad + BTN_H + pad);
    setFixedSize(pad + BTN_W + pad, totalH);
}

// ── Display sub-panel ─────────────────────────────────────────────────────────

void SpectrumOverlayMenu::buildDisplayPanel()
{
    m_displayPanel = new QWidget(parentWidget());
    m_displayPanel->setStyleSheet("QWidget { background: rgba(15, 15, 26, 220); "
                                   "border: 1px solid #304050; border-radius: 3px; }");
    m_displayPanel->hide();

    auto* grid = new QGridLayout(m_displayPanel);
    grid->setContentsMargins(8, 6, 8, 6);
    grid->setSpacing(4);
    grid->setColumnStretch(1, 1);

    auto labelStyle = QStringLiteral("QLabel { color: #8090a0; font-size: 10px; border: none; }");
    auto valStyle   = QStringLiteral("QLabel { color: #c8d8e8; font-size: 10px; border: none;"
                                      " min-width: 24px; }");
    auto sliderStyle = QStringLiteral(
        "QSlider { border: none; }"
        "QSlider::groove:horizontal { height: 4px; background: #203040; border-radius: 2px; }"
        "QSlider::handle:horizontal { width: 10px; height: 10px; margin: -3px 0;"
        " background: #00b4d8; border-radius: 5px; }");
    auto btnStyle = QStringLiteral(
        "QPushButton { background: #1a2a3a; color: #c8d8e8; border: 1px solid #205070;"
        " border-radius: 3px; font-size: 10px; font-weight: bold; padding: 2px 6px; }"
        "QPushButton:hover { background: #204060; }"
        "QPushButton:checked { background: #006040; color: #00ff88; border-color: #00a060; }");

    int row = 0;
    // Grid columns: 0=label, 1=button (optional), 2=slider, 3=value

    // Helper: label col 0, slider col 1-2, value col 3
    auto makeRow = [&](const QString& text, int lo, int hi, int def,
                       QSlider*& slider, QLabel*& valLbl) {
        auto* lbl = new QLabel(text);
        lbl->setStyleSheet(labelStyle);
        grid->addWidget(lbl, row, 0);

        slider = new GuardedSlider(Qt::Horizontal);
        slider->setRange(lo, hi);
        slider->setValue(def);
        slider->setStyleSheet(sliderStyle);
        grid->addWidget(slider, row, 1, 1, 2);

        valLbl = new QLabel(QString::number(def));
        valLbl->setStyleSheet(valStyle);
        valLbl->setFixedWidth(28);
        valLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        grid->addWidget(valLbl, row, 3);
        ++row;
    };

    // Helper: label col 0, button col 1, slider col 2, value col 3
    auto makeRowWithBtn = [&](const QString& text, int lo, int hi, int def,
                              QSlider*& slider, QLabel*& valLbl,
                              QPushButton*& btn, const QString& btnText) {
        auto* lbl = new QLabel(text);
        lbl->setStyleSheet(labelStyle);
        grid->addWidget(lbl, row, 0);

        btn = new QPushButton(btnText);
        btn->setCheckable(true);
        btn->setFixedSize(36, 18);
        btn->setStyleSheet(btnStyle);
        grid->addWidget(btn, row, 1);

        slider = new GuardedSlider(Qt::Horizontal);
        slider->setRange(lo, hi);
        slider->setValue(def);
        slider->setStyleSheet(sliderStyle);
        grid->addWidget(slider, row, 2);

        valLbl = new QLabel(QString::number(def));
        valLbl->setStyleSheet(valStyle);
        valLbl->setFixedWidth(28);
        valLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        grid->addWidget(valLbl, row, 3);
        ++row;
    };

    // ── Sliders ───────────────────────────────────────────────────────────

    // AVG
    makeRow("AVG:", 0, 100, 0, m_avgSlider, m_avgLabel);
    connect(m_avgSlider, &QSlider::valueChanged, this, [this](int v) {
        m_avgLabel->setText(QString::number(v));
        emit fftAverageChanged(v);
    });

    // FPS
    makeRow("FPS:", 5, 30, 25, m_fpsSlider, m_fpsLabel);
    connect(m_fpsSlider, &QSlider::valueChanged, this, [this](int v) {
        m_fpsLabel->setText(QString::number(v));
        emit fftFpsChanged(v);
    });

    // Line Width
    {
        auto* lbl = new QLabel("Line Width:");
        lbl->setStyleSheet(labelStyle);
        grid->addWidget(lbl, row, 0);

        m_lineWidthSlider = new QSlider(Qt::Horizontal);
        m_lineWidthSlider->setRange(0, 10);
        m_lineWidthSlider->setValue(4);
        m_lineWidthSlider->setSingleStep(1);
        m_lineWidthSlider->setPageStep(1);
        m_lineWidthSlider->setStyleSheet(sliderStyle);
        grid->addWidget(m_lineWidthSlider, row, 1, 1, 2);

        m_lineWidthLabel = new QLabel("2.0");
        m_lineWidthLabel->setStyleSheet(valStyle);
        m_lineWidthLabel->setFixedWidth(28);
        m_lineWidthLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        grid->addWidget(m_lineWidthLabel, row, 3);
        ++row;

        connect(m_lineWidthSlider, &QSlider::valueChanged, this, [this](int v) {
            float w = v * 0.5f;
            m_lineWidthLabel->setText(v == 0 ? "Off" : QString::number(w, 'f', 1));
            emit fftLineWidthChanged(w);
        });
    }

    // Gain
    makeRow("Gain:", 0, 100, 50, m_gainSlider, m_gainLabel);
    connect(m_gainSlider, &QSlider::valueChanged, this, [this](int v) {
        m_gainLabel->setText(QString::number(v));
        emit wfColorGainChanged(v);
    });

    // Rate
    makeRow("Rate:", 1, 30, 15, m_rateSlider, m_rateLabel);
    connect(m_rateSlider, &QSlider::valueChanged, this, [this](int v) {
        m_rateLabel->setText(QString::number(v));
        emit wfLineDurationChanged(v + 70);
    });

    // Fill: color picker button + slider
    {
        auto* lbl = new QLabel("Fill:");
        lbl->setStyleSheet(labelStyle);
        grid->addWidget(lbl, row, 0);

        m_fillColorBtn = new QPushButton;
        m_fillColorBtn->setFixedSize(18, 18);
        m_fillColorBtn->setStyleSheet(
            QString("QPushButton { background: %1; border: 1px solid #506070;"
                    " border-radius: 2px; }")
                .arg(m_fillColor.name()));
        m_fillColorBtn->setToolTip("Choose fill color");
        grid->addWidget(m_fillColorBtn, row, 1);

        m_fillSlider = new GuardedSlider(Qt::Horizontal);
        m_fillSlider->setRange(0, 100);
        m_fillSlider->setValue(70);
        m_fillSlider->setStyleSheet(sliderStyle);
        grid->addWidget(m_fillSlider, row, 2);

        m_fillLabel = new QLabel("70");
        m_fillLabel->setStyleSheet(valStyle);
        m_fillLabel->setFixedWidth(28);
        m_fillLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        grid->addWidget(m_fillLabel, row, 3);
        ++row;

        connect(m_fillSlider, &QSlider::valueChanged, this, [this](int v) {
            m_fillLabel->setText(QString::number(v));
            emit fftFillAlphaChanged(v / 100.0f);
        });
        connect(m_fillColorBtn, &QPushButton::clicked, this, [this] {
            QColor c = QColorDialog::getColor(m_fillColor, this, "FFT Fill Color",
                                               QColorDialog::DontUseNativeDialog);
            if (c.isValid()) {
                m_fillColor = c;
                m_fillColorBtn->setStyleSheet(
                    QString("QPushButton { background: %1; border: 1px solid #506070;"
                            " border-radius: 2px; }")
                        .arg(c.name()));
                emit fftFillColorChanged(c);
            }
        });
    }

    // Black + Auto.  Single slider with two roles depending on AUTO state:
    //   AUTO off → manual black-level (0..100), value persists in
    //              m_blackManualValue and emits wfBlackLevelChanged.
    //   AUTO on  → noise-floor offset (0..100, 50 = noise floor), value
    //              persists in m_blackAutoOffsetValue and emits
    //              wfAutoBlackOffsetChanged.
    makeRowWithBtn("Black:", 0, 100, 50, m_blackSlider, m_blackLabel,
                   m_autoBlackBtn, "Auto");
    m_autoBlackBtn->setChecked(true);
    connect(m_blackSlider, &QSlider::valueChanged, this, [this](int v) {
        m_blackLabel->setText(QString::number(v));
        if (m_autoBlackBtn && m_autoBlackBtn->isChecked()) {
            m_blackAutoOffsetValue = v;
            emit wfAutoBlackOffsetChanged(v);
        } else {
            m_blackManualValue = v;
            emit wfBlackLevelChanged(v);
        }
    });
    connect(m_autoBlackBtn, &QPushButton::toggled, this, [this](bool on) {
        emit wfAutoBlackChanged(on);
        // Swap the displayed slider value to whichever role just became
        // active.  Block signals so the swap doesn't echo back as a user
        // edit — we just want the UI to reflect the matching stored value.
        if (m_blackSlider) {
            QSignalBlocker bs(m_blackSlider);
            const int v = on ? m_blackAutoOffsetValue : m_blackManualValue;
            m_blackSlider->setValue(v);
            if (m_blackLabel)
                m_blackLabel->setText(QString::number(v));
        }
        if (m_blackSlider) {
            m_blackSlider->setToolTip(on
                ? "Auto-black target offset. 50 = at noise floor; lower = darker, higher = lighter."
                : "Waterfall black level. Decrease to darken the noise floor.");
        }
        if (!on)
            emit wfBlackLevelChanged(m_blackManualValue);
        else
            emit wfAutoBlackOffsetChanged(m_blackAutoOffsetValue);
    });

    // NB Blank + Off/On
    makeRowWithBtn("NB Blank:", 5, 95, 15, m_wfBlankerThreshSlider, m_wfBlankerThreshLabel,
                   m_wfBlankerBtn, "Off");
    m_wfBlankerBtn->setToolTip("Suppress impulse noise stripes in waterfall");
    connect(m_wfBlankerBtn, &QPushButton::toggled, this, [this](bool on) {
        m_wfBlankerBtn->setText(on ? "On" : "Off");
        emit wfBlankerEnabledChanged(on);
    });
    connect(m_wfBlankerThreshSlider, &QSlider::valueChanged, this, [this](int v) {
        float t = 1.0f + v / 100.0f;
        m_wfBlankerThreshLabel->setText(QString::number(t, 'f', 2));
        emit wfBlankerThresholdChanged(t);
    });

    // BG Opacity
    {
        auto* lbl = new QLabel("BG Opacity:");
        lbl->setStyleSheet(labelStyle);
        grid->addWidget(lbl, row, 0);
        m_bgOpacitySlider = new GuardedSlider(Qt::Horizontal);
        m_bgOpacitySlider->setRange(0, 100);
        m_bgOpacitySlider->setValue(80);
        m_bgOpacitySlider->setStyleSheet(sliderStyle);
        grid->addWidget(m_bgOpacitySlider, row, 1, 1, 2);
        m_bgOpacityLabel = new QLabel("80");
        m_bgOpacityLabel->setStyleSheet(valStyle);
        m_bgOpacityLabel->setFixedWidth(28);
        m_bgOpacityLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        grid->addWidget(m_bgOpacityLabel, row, 3);
        connect(m_bgOpacitySlider, &QSlider::valueChanged, this, [this](int v) {
            m_bgOpacityLabel->setText(QString::number(v));
            emit backgroundOpacityChanged(v);
        });
        ++row;
    }

    // ── Background buttons ────────────────────────────────────────────────
    {
        auto* lbl = new QLabel("Background:");
        lbl->setStyleSheet(labelStyle);
        grid->addWidget(lbl, row, 0, 1, 2);
        auto* bgBtn = new QPushButton("Choose...");
        bgBtn->setFixedHeight(18);
        bgBtn->setStyleSheet(btnStyle);
        connect(bgBtn, &QPushButton::clicked, this, [this] {
            emit backgroundImageRequested();
        });
        grid->addWidget(bgBtn, row, 2);
        auto* clearBtn = new QPushButton("Clear");
        clearBtn->setFixedHeight(18);
        clearBtn->setStyleSheet(btnStyle);
        clearBtn->setToolTip("Revert to the default logo background");
        connect(clearBtn, &QPushButton::clicked, this, [this] {
            emit backgroundImageCleared();
        });
        grid->addWidget(clearBtn, row, 3);
        ++row;
    }

    // ── Toggle button row ─────────────────────────────────────────────────
    {
        auto* toggleRow = new QWidget;
        auto* toggleLayout = new QHBoxLayout(toggleRow);
        toggleLayout->setContentsMargins(0, 2, 0, 2);
        toggleLayout->setSpacing(3);

        auto makeToggle = [&](const QString& text, QPushButton*& btn, bool checked = false) {
            btn = new QPushButton(text);
            btn->setCheckable(true);
            btn->setChecked(checked);
            btn->setStyleSheet(btnStyle);
            btn->setFixedHeight(20);
            toggleLayout->addWidget(btn);
        };

        makeToggle("Heat Map", m_heatMapBtn);
        makeToggle("Grid", m_showGridBtn, true);
        makeToggle("Wt Avg", m_weightedAvgBtn);

        grid->addWidget(toggleRow, row, 0, 1, 4);
        ++row;

        connect(m_heatMapBtn, &QPushButton::toggled, this, [this](bool on) {
            emit fftHeatMapChanged(on);
        });
        connect(m_showGridBtn, &QPushButton::toggled, this, [this](bool on) {
            emit showGridChanged(on);
        });
        connect(m_weightedAvgBtn, &QPushButton::toggled, this, [this](bool on) {
            emit fftWeightedAverageChanged(on);
        });
    }

    // ── Noise Floor reference line ────────────────────────────────────────
    makeRowWithBtn("Floor:", 1, 99, 75, m_floorSlider, m_floorLabel,
                   m_floorEnableBtn, "Off");
    m_floorSlider->setEnabled(false);
    connect(m_floorEnableBtn, &QPushButton::toggled, this, [this](bool on) {
        m_floorEnableBtn->setText(on ? "On" : "Off");
        m_floorSlider->setEnabled(on);
        emit noiseFloorEnableChanged(on);
    });
    connect(m_floorSlider, &QSlider::valueChanged, this, [this](int v) {
        m_floorLabel->setText(QString::number(v));
        emit noiseFloorPositionChanged(v);
    });

    // ── Auto-squelch margin ───────────────────────────────────────────────
    {
        auto& s = AppSettings::instance();
        int savedMargin = std::clamp(s.value("AutoSqlMarginDb", "10").toInt(), 5, 20);
        makeRow("SQL Margin:", 5, 20, savedMargin, m_autoSqlMarginSlider, m_autoSqlMarginLabel);
        connect(m_autoSqlMarginSlider, &QSlider::valueChanged, this, [this](int v) {
            m_autoSqlMarginLabel->setText(QString::number(v));
            auto& s2 = AppSettings::instance();
            s2.setValue("AutoSqlMarginDb", QString::number(v));
            s2.save();
            emit autoSqlMarginDbChanged(v);
        });
    }

    // ── Freq Grid Spacing dropdown (#1390) ──────────────────────────────
    {
        auto* lbl = new QLabel("Grid:");
        lbl->setStyleSheet(labelStyle);
        grid->addWidget(lbl, row, 0);
        m_freqGridSpacingCmb = new QComboBox;
        m_freqGridSpacingCmb->setFixedHeight(18);
        m_freqGridSpacingCmb->setStyleSheet(comboStyleSheet());
        m_freqGridSpacingCmb->addItem("Auto", 0);
        for (int khz : {1, 2, 5, 10, 25, 50, 100})
            m_freqGridSpacingCmb->addItem(QString("%1 kHz").arg(khz), khz);
        grid->addWidget(m_freqGridSpacingCmb, row, 1, 1, 3);
        connect(m_freqGridSpacingCmb, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int idx) {
            emit freqGridSpacingChanged(m_freqGridSpacingCmb->itemData(idx).toInt());
        });
        ++row;
    }

    // ── Scheme dropdown ───────────────────────────────────────────────────
    {
        auto* lbl = new QLabel("Scheme:");
        lbl->setStyleSheet(labelStyle);
        grid->addWidget(lbl, row, 0);
        m_colorSchemeCmb = new QComboBox;
        m_colorSchemeCmb->setFixedHeight(18);
        m_colorSchemeCmb->setStyleSheet(comboStyleSheet());
        for (int i = 0; i < static_cast<int>(WfColorScheme::Count); ++i)
            m_colorSchemeCmb->addItem(wfSchemeName(static_cast<WfColorScheme>(i)));
        grid->addWidget(m_colorSchemeCmb, row, 1, 1, 3);
        connect(m_colorSchemeCmb, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int idx) { emit wfColorSchemeChanged(idx); });
        ++row;
    }

    // ── Reset button ──────────────────────────────────────────────────────
    {
        auto* resetBtn = new QPushButton("Reset to Defaults");
        resetBtn->setStyleSheet(btnStyle);
        resetBtn->setToolTip("Reset all display settings to their default values");
        connect(resetBtn, &QPushButton::clicked, this, [this] {
            emit displaySettingsReset();
        });
        grid->addWidget(resetBtn, row, 0, 1, 4);
        ++row;
    }

    // Display panel tooltips
    m_avgSlider->setToolTip("FFT frame averaging. Higher values smooth the spectrum trace but reduce time resolution.");
    m_fpsSlider->setToolTip("FFT refresh rate in frames per second.");
    m_fillSlider->setToolTip("Opacity of the spectrum fill area below the trace.");
    if (m_heatMapBtn) m_heatMapBtn->setToolTip("Colors the spectrum trace by signal strength instead of a single color.");
    if (m_showGridBtn) m_showGridBtn->setToolTip("Show or hide the frequency and dB grid lines on the panadapter.");
    if (m_weightedAvgBtn) m_weightedAvgBtn->setToolTip("Weights recent FFT frames more heavily for faster response to signal changes.");
    m_gainSlider->setToolTip("Waterfall color gain. Higher values brighten weak signals.");
    m_blackSlider->setToolTip("Waterfall black level. Decrease to darken the noise floor.");
    if (m_autoBlackBtn) m_autoBlackBtn->setToolTip("Automatically adjusts the waterfall black level to match the current noise floor.");
    m_rateSlider->setToolTip("Waterfall line duration. Higher values scroll faster.");
    if (m_wfBlankerThreshSlider) m_wfBlankerThreshSlider->setToolTip("Waterfall noise blanking threshold. Higher values blank more aggressively.");
    if (m_freqGridSpacingCmb) m_freqGridSpacingCmb->setToolTip("Frequency grid line spacing. Auto adapts to the current span.");
    if (m_colorSchemeCmb) m_colorSchemeCmb->setToolTip("Selects the waterfall color palette.");
    if (m_bgOpacitySlider) m_bgOpacitySlider->setToolTip("Opacity of the background image overlay.");
    if (m_floorEnableBtn) m_floorEnableBtn->setToolTip("Shows a noise floor reference line on the spectrum display.");
    if (m_floorSlider) m_floorSlider->setToolTip("Vertical position of the noise floor reference line (% from top).");
    if (m_autoSqlMarginSlider) m_autoSqlMarginSlider->setToolTip("Auto-squelch margin in dBm above the measured noise floor.");

    m_displayPanel->adjustSize();
}

void SpectrumOverlayMenu::syncDisplaySettings(int avg, int fps, int fillPct,
                                               bool weightedAvg, const QColor& fillColor,
                                               int gain, int black, bool autoBlack,
                                               int autoBlackOffset, int rate,
                                               int floorPos, bool floorEnable,
                                               bool heatMap, int colorScheme,
                                               bool showGrid,
                                               float lineWidth)
{
    if (!m_avgSlider) return;  // panel not built yet

    QSignalBlocker b1(m_avgSlider), b2(m_fpsSlider), b3(m_fillSlider),
                   b4(m_weightedAvgBtn), b5(m_gainSlider), b6(m_blackSlider),
                   b7(m_autoBlackBtn), b8(m_rateSlider);

    m_avgSlider->setValue(avg);
    m_avgLabel->setText(QString::number(avg));
    m_fpsSlider->setValue(fps);
    m_fpsLabel->setText(QString::number(fps));
    m_fillSlider->setValue(fillPct);
    m_fillLabel->setText(QString::number(fillPct));
    m_weightedAvgBtn->setChecked(weightedAvg);
    m_fillColor = fillColor;
    m_fillColorBtn->setStyleSheet(
        QString("QPushButton { background: %1; border: 1px solid #506070;"
                " border-radius: 2px; }").arg(fillColor.name()));
    m_gainSlider->setValue(gain);
    m_gainLabel->setText(QString::number(gain));
    m_blackManualValue     = black;
    m_blackAutoOffsetValue = autoBlackOffset;
    const int displayBlack = autoBlack ? autoBlackOffset : black;
    m_blackSlider->setValue(displayBlack);
    m_blackLabel->setText(QString::number(displayBlack));
    m_blackSlider->setToolTip(autoBlack
        ? "Auto-black target offset. 50 = at noise floor; lower = darker, higher = lighter."
        : "Waterfall black level. Decrease to darken the noise floor.");
    m_autoBlackBtn->setChecked(autoBlack);
    int rateSliderVal = std::clamp(rate - 70, 1, 30);  // line_duration 71-100 → slider 1-30
    m_rateSlider->setValue(rateSliderVal);
    m_rateLabel->setText(QString::number(rateSliderVal));

    if (m_floorSlider) {
        QSignalBlocker bf(m_floorSlider), be(m_floorEnableBtn);
        m_floorSlider->setValue(floorPos);
        m_floorLabel->setText(QString::number(floorPos));
        m_floorEnableBtn->setChecked(floorEnable);
        m_floorEnableBtn->setText(floorEnable ? "On" : "Off");
        m_floorSlider->setEnabled(floorEnable);
    }
    if (m_heatMapBtn) {
        QSignalBlocker bh(m_heatMapBtn);
        m_heatMapBtn->setChecked(heatMap);
    }
    if (m_showGridBtn) {
        QSignalBlocker bg(m_showGridBtn);
        m_showGridBtn->setChecked(showGrid);
    }
    if (m_lineWidthSlider) {
        QSignalBlocker blw(m_lineWidthSlider);
        int sliderVal = std::clamp(static_cast<int>(lineWidth / 0.5f), 0, 10);
        m_lineWidthSlider->setValue(sliderVal);
        m_lineWidthLabel->setText(sliderVal == 0 ? "Off" : QString::number(sliderVal * 0.5f, 'f', 1));
    }
    if (m_colorSchemeCmb) {
        QSignalBlocker bc(m_colorSchemeCmb);
        m_colorSchemeCmb->setCurrentIndex(colorScheme);
    }
}

void SpectrumOverlayMenu::syncExtraDisplaySettings(bool blankerOn, float blankerThresh,
                                                    int bgOpacity,
                                                    int freqGridSpacingKhz)
{
    if (m_freqGridSpacingCmb) {
        QSignalBlocker b(m_freqGridSpacingCmb);
        int idx = m_freqGridSpacingCmb->findData(freqGridSpacingKhz);
        if (idx >= 0) m_freqGridSpacingCmb->setCurrentIndex(idx);
        else          m_freqGridSpacingCmb->setCurrentIndex(0);  // Auto
    }
    if (m_wfBlankerBtn) {
        QSignalBlocker b(m_wfBlankerBtn);
        m_wfBlankerBtn->setChecked(blankerOn);
        m_wfBlankerBtn->setText(blankerOn ? "On" : "Off");
    }
    if (m_wfBlankerThreshSlider) {
        QSignalBlocker b(m_wfBlankerThreshSlider);
        int sliderVal = static_cast<int>((blankerThresh - 1.0f) * 100.0f);
        m_wfBlankerThreshSlider->setValue(sliderVal);
        if (m_wfBlankerThreshLabel)
            m_wfBlankerThreshLabel->setText(QString::number(blankerThresh, 'f', 2));
    }
    if (m_bgOpacitySlider) {
        QSignalBlocker b(m_bgOpacitySlider);
        m_bgOpacitySlider->setValue(bgOpacity);
        if (m_bgOpacityLabel)
            m_bgOpacityLabel->setText(QString::number(bgOpacity));
    }
}

void SpectrumOverlayMenu::toggleDisplayPanel()
{
    bool wasVisible = m_displayPanelVisible;
    hideAllSubPanels();
    if (!wasVisible) {
        m_displayPanelVisible = true;
        int menuBottom = y() + height();
        int panelH = m_displayPanel->sizeHint().height();
        int panelY = menuBottom - panelH;
        m_displayPanel->move(x() + width(), std::max(0, panelY));
        m_displayPanel->raise();
        m_displayPanel->show();
        m_menuBtns[kBtnDisplay]->setStyleSheet(kMenuBtnActive);
    }
}

void SpectrumOverlayMenu::setWnbState(bool on, int level)
{
    QSignalBlocker b1(m_wnbBtn), b2(m_wnbSlider);
    m_wnbBtn->setChecked(on);
    m_wnbSlider->setValue(level);
    m_wnbLabel->setText(QString::number(level));
}

void SpectrumOverlayMenu::setSwrSweepPowerWatts(int watts)
{
    if (!m_swrPowerSlider)
        return;

    watts = qBound(1, watts, 10);
    QSignalBlocker blocker(m_swrPowerSlider);
    m_swrPowerSlider->setValue(watts);
    if (m_swrPowerLabel)
        m_swrPowerLabel->setText(QString("%1 W").arg(watts));
}

void SpectrumOverlayMenu::setRfGain(int gain)
{
    QSignalBlocker b(m_rfGainSlider);
    m_rfGainSlider->setValue(gain);
    m_rfGainLabel->setText(QString("%1 dB").arg(gain));
    m_lastEmittedRfGain = gain;  // keep emit-dedupe in sync with external updates
}

void SpectrumOverlayMenu::setRfGainRange(int low, int high, int step)
{
    if (!m_rfGainSlider) return;
    QSignalBlocker b(m_rfGainSlider);
    m_rfGainSlider->setRange(low, high);
    m_rfGainSlider->setSingleStep(step);
    m_rfGainSlider->setPageStep(step);
    m_rfGainSlider->setTickInterval(step);
    m_rfGainSlider->setToolTip(
        QString("RF Gain: %1 to %2%3 dB (%4 dB steps)\n"
                "Step size is determined by radio hardware.")
            .arg(low).arg(high > 0 ? "+" : "").arg(high).arg(step));
}

void SpectrumOverlayMenu::setXvtrBands(const QVector<XvtrBand>& bands)
{
    const bool bandPanelWasVisible = m_bandPanel && m_bandPanel->isVisible();
    const QPoint bandPanelPos = m_bandPanel ? m_bandPanel->pos()
                                            : QPoint(x() + width(), y());

    // Remove old XVTR band buttons from main band panel
    for (auto* btn : m_xvtrBandBtns)
        btn->deleteLater();
    m_xvtrBandBtns.clear();

    // Rebuild the main band panel to insert XVTR bands between
    // HF bands and utility buttons (WWV/GEN/2200/630/XVTR). (#571)
    if (m_bandPanel) {
        // Delete existing band panel and rebuild
        m_bandPanel->hide();
        m_bandPanel->deleteLater();
        m_bandPanel = nullptr;
    }

    m_bandPanel = new QWidget(parentWidget());
    m_bandPanel->setStyleSheet("QWidget { background: rgba(15, 15, 26, 220); "
                                "border: 1px solid #304050; border-radius: 3px; }");
    m_bandPanel->hide();
    m_bandPanel->installEventFilter(this);

    auto* grid = new QGridLayout(m_bandPanel);
    grid->setContentsMargins(2, 2, 2, 2);
    grid->setSpacing(2);

    const QString bandBtnStyle =
        "QPushButton { background: rgba(30, 40, 55, 220); "
        "border: 1px solid #304050; border-radius: 3px; "
        "color: #c8d8e8; font-size: 11px; font-weight: bold; }"
        "QPushButton:hover { background: rgba(0, 112, 192, 180); "
        "border: 1px solid #0090e0; }";

    const QString xvtrBtnStyle =
        "QPushButton { background: rgba(30, 40, 55, 220); "
        "border: 1px solid #304050; border-radius: 3px; "
        "color: #00d0ff; font-size: 11px; font-weight: bold; }"
        "QPushButton:hover { background: rgba(0, 112, 192, 180); "
        "border: 1px solid #0090e0; }";

    // HF bands (indices 0-10)
    constexpr int hfLayout[][3] = {
        {0, 1, 2},      // 160, 80, 60
        {3, 4, 5},      // 40, 30, 20
        {6, 7, 8},      // 17, 15, 12
        {9, 10, -1},    // 10, 6
    };

    int row = 0;
    for (int r = 0; r < 4; ++r) {
        for (int col = 0; col < 3; ++col) {
            int idx = hfLayout[r][col];
            if (idx < 0) continue;
            auto* btn = new QPushButton(BAND_GRID[idx].label, m_bandPanel);
            btn->setFixedSize(BAND_BTN_W, BAND_BTN_H);
            btn->setStyleSheet(bandBtnStyle);
            QString bandName = QString::fromLatin1(BAND_GRID[idx].bandName);
            double freq = BAND_GRID[idx].freqMhz;
            QString mode = QString::fromLatin1(BAND_GRID[idx].mode);
            connect(btn, &QPushButton::clicked, this, [this, bandName, freq, mode]() {
                hideAllSubPanels();
                emit bandSelected(bandName, freq, mode);
            });
            grid->addWidget(btn, row, col);
        }
        ++row;
    }

    // XVTR bands (inserted between HF and utility)
    m_xvtrBandBtns.clear();
    for (int i = 0; i < bands.size(); ++i) {
        auto* btn = new QPushButton(bands[i].name, m_bandPanel);
        btn->setFixedSize(BAND_BTN_W, BAND_BTN_H);
        btn->setStyleSheet(xvtrBtnStyle);
        const double freq = bands[i].rfFreqMhz;
        const QString name = bands[i].name;
        connect(btn, &QPushButton::clicked, this, [this, name, freq]() {
            hideAllSubPanels();
            emit bandSelected(name, freq, "USB");
        });
        grid->addWidget(btn, row + i / 3, i % 3);
        m_xvtrBandBtns.append(btn);
    }
    if (!bands.isEmpty())
        row += (bands.size() + 2) / 3;  // advance past XVTR rows

    // Utility buttons: WWV, GEN, 2200, 630, XVTR config
    constexpr int utilLayout[][3] = {
        {11, 12, -1},   // WWV, GEN
        {13, 14, 15},   // 2200, 630, XVTR
    };
    for (int r = 0; r < 2; ++r) {
        for (int col = 0; col < 3; ++col) {
            int idx = utilLayout[r][col];
            if (idx < 0) continue;
            auto* btn = new QPushButton(BAND_GRID[idx].label, m_bandPanel);
            btn->setFixedSize(BAND_BTN_W, BAND_BTN_H);
            btn->setStyleSheet(bandBtnStyle);
            QString bandName = QString::fromLatin1(BAND_GRID[idx].bandName);
            double freq = BAND_GRID[idx].freqMhz;
            QString mode = QString::fromLatin1(BAND_GRID[idx].mode);
            if (idx == 15) {
                connect(btn, &QPushButton::clicked, this, [this]() {
                    hideAllSubPanels();
                    emit xvtrSetupRequested();
                });
            } else if (bandName.isEmpty()) {
                btn->setEnabled(false);
            } else {
                connect(btn, &QPushButton::clicked, this, [this, bandName, freq, mode]() {
                    hideAllSubPanels();
                    emit bandSelected(bandName, freq, mode);
                });
            }
            grid->addWidget(btn, row, col);
        }
        ++row;
    }

    m_bandPanel->adjustSize();
    m_bandPanelVisible = bandPanelWasVisible;
    if (bandPanelWasVisible)
        showBandPanelAt(bandPanelPos);
    else
        m_menuBtns[kBtnBand]->setStyleSheet(kMenuBtnNormal);

    // Rebuild the XVTR sub-panel (kept as fallback)
    if (m_xvtrPanel) {
        m_xvtrPanel->deleteLater();
        m_xvtrPanel = nullptr;
    }

    m_xvtrPanel = new QWidget(parentWidget());
    m_xvtrPanel->setStyleSheet("QWidget { background: rgba(15, 15, 26, 220); "
                                "border: 1px solid #304050; border-radius: 3px; }");
    m_xvtrPanel->hide();
    m_xvtrPanel->installEventFilter(this);
    m_xvtrPanelVisible = false;

    auto* xvGrid = new QGridLayout(m_xvtrPanel);
    xvGrid->setContentsMargins(2, 2, 2, 2);
    xvGrid->setSpacing(2);

    const QString btnStyle =
        "QPushButton { background: rgba(30, 40, 55, 220); "
        "border: 1px solid #304050; border-radius: 3px; "
        "color: #c8d8e8; font-size: 11px; font-weight: bold; }"
        "QPushButton:hover { background: rgba(0, 112, 192, 180); "
        "border: 1px solid #0090e0; }";

    static constexpr int XVTR_COLS = 2;
    static constexpr int XVTR_MIN_ROWS = 4;
    int totalSlots = qMax(XVTR_MIN_ROWS * XVTR_COLS,
                          (bands.size() + 1 + XVTR_COLS - 1) / XVTR_COLS * XVTR_COLS);

    const QString disabledStyle = btnStyle +
        "QPushButton:disabled { background: rgba(15, 15, 26, 180); "
        "color: #252535; border: 1px solid #1a1a2a; }";

    // Fill grid: configured bands first, then empty slots, HF button last
    int slot = 0;
    for (const auto& xvtr : bands) {
        auto* btn = new QPushButton(xvtr.name, m_xvtrPanel);
        btn->setFixedSize(BAND_BTN_W, BAND_BTN_H);
        btn->setStyleSheet(btnStyle);

        const double freq = xvtr.rfFreqMhz;
        const QString name = xvtr.name;
        connect(btn, &QPushButton::clicked, this, [this, name, freq]() {
            emit bandSelected(name, freq, "FM");
            m_xvtrPanel->hide();
            m_xvtrPanelVisible = false;
        });

        xvGrid->addWidget(btn, slot / XVTR_COLS, slot % XVTR_COLS);
        ++slot;
    }

    // Fill remaining slots (except last) with blank disabled buttons
    while (slot < totalSlots - 1) {
        auto* blank = new QPushButton("", m_xvtrPanel);
        blank->setFixedSize(BAND_BTN_W, BAND_BTN_H);
        blank->setStyleSheet(disabledStyle);
        blank->setEnabled(false);
        xvGrid->addWidget(blank, slot / XVTR_COLS, slot % XVTR_COLS);
        ++slot;
    }

    // HF button in last slot (bottom-right)
    auto* hfBtn = new QPushButton("HF", m_xvtrPanel);
    hfBtn->setFixedSize(BAND_BTN_W, BAND_BTN_H);
    hfBtn->setStyleSheet(btnStyle);
    connect(hfBtn, &QPushButton::clicked, this, [this]() {
        const QPoint pos = m_xvtrPanel->pos();
        if (m_xvtrPanel)
            m_xvtrPanel->hide();
        m_xvtrPanelVisible = false;
        showBandPanelAt(pos);
    });
    xvGrid->addWidget(hfBtn, slot / XVTR_COLS, slot % XVTR_COLS);

    m_xvtrPanel->adjustSize();
}

bool SpectrumOverlayMenu::eventFilter(QObject* obj, QEvent* event)
{
    if (event->type() == QEvent::MouseButtonDblClick) {
        if (auto* slider = qobject_cast<QSlider*>(obj)) {
            slider->setValue(50);
            return true;
        }
    }
    // Consume mouse/wheel events on sub-panels so they don't reach the spectrum
    if (obj == m_bandPanel || obj == m_antPanel
        || obj == m_daxPanel || obj == m_displayPanel || obj == m_memoryPanel) {
        if (event->type() == QEvent::Wheel
            || event->type() == QEvent::MouseButtonPress
            || event->type() == QEvent::MouseButtonRelease) {
            return true;  // consumed
        }
    }
    return QWidget::eventFilter(obj, event);
}

} // namespace AetherSDR
