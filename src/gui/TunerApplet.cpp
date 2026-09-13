#include "TunerApplet.h"
#include "HGauge.h"
#include "TgxlPanelWidgets.h"
#include "models/TunerModel.h"
#include "models/MeterModel.h"
#include "models/BandSettings.h"

#include <QAccessible>
#include <QPushButton>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QEvent>
#include <QLayout>
#include <QSignalBlocker>
#include <QSpacerItem>
#include <QTimer>
#include "core/ThemeManager.h"
namespace AetherSDR {

namespace {

// The expanded panel's design size — the size at which every metric below is
// its literal value. Actual metrics are that value times a single scale
// derived from how much room the panel actually has, so growing the window
// grows the contents rather than the padding around them.
//
// Same idea as CrossNeedleMeterWidget, which fits a fixed design canvas into
// its widget and scales the painter onto it. That works because the meter is
// one painted face; this panel is a widget tree, so the scale is applied to
// each metric instead of to a QPainter. The limiting dimension wins, so the
// panel keeps its proportions instead of stretching.
constexpr qreal kDesignWidth  = 380.0;
// Only a first guess at the contents' height: applyDensity replaces it with
// the measured value as soon as there is a laid-out column to measure.
constexpr qreal kDesignHeight = 250.0;
constexpr qreal kMinScale = 0.8;   // below this the type stops being legible
constexpr qreal kMaxScale = 3.0;

// The gap below the controls. Deliberately not scaled: it exists so they
// clear the frame rather than sit against it, which is a constant few pixels
// at any size.
constexpr int kBottomGap = 8;

// The discrete keys are letterbox-shaped rather than square. Their width is
// set by the control row's split with the dials, so the aspect is applied as
// a cap on their height: the row is as tall as the dials, and without it the
// keys stretch to match and become columns.
constexpr qreal kKeyAspect = 16.0 / 9.0;
constexpr int kKeyFontDesignPx = 13;
// Breathing room around the widest caption, in design pixels.
constexpr int kKeyPaddingDesignPx = 18;

// The three states TUNE cycles through visually. Kept as named templates
// because both presentations' TUNE buttons wear them and the tuning handler
// swaps between them in two places.
constexpr const char* kTuneIdleStyle =
    "QPushButton { background: {{color.background.2}}; border: 1px solid {{color.background.2}}; "
    "border-radius: 3px; color: {{color.text.primary}}; font-weight: bold; }"
    "QPushButton:hover { background: {{color.background.1}}; }";
constexpr const char* kTuneBusyStyle =
    "QPushButton { background: #cc2222; border: 1px solid {{color.accent.danger}}; "
    "border-radius: 3px; color: {{color.text.primary}}; font-weight: bold; }";

// The expanded presentation's STBY / BYP keys: resting, and lit while the
// tuner is in the state that key selects.
constexpr const char* kPanelKeyIdleStyle =
    "QPushButton { background: {{color.background.2}}; border: 1px solid {{color.background.2}}; "
    "border-radius: 3px; color: {{color.text.primary}}; font-weight: bold; }"
    "QPushButton:hover { background: {{color.background.1}}; }";
constexpr const char* kStandbyActiveStyle =
    "QPushButton { background: {{color.tgxl.key.standby.background}}; "
    "border: 1px solid {{color.tgxl.key.standby.foreground}}; border-radius: 3px; "
    "color: {{color.tgxl.key.standby.foreground}}; font-weight: bold; }";
constexpr const char* kBypassActiveStyle =
    "QPushButton { background: {{color.tgxl.key.bypass.background}}; "
    "border: 1px solid {{color.tgxl.key.bypass.foreground}}; border-radius: 3px; "
    "color: {{color.tgxl.key.bypass.foreground}}; font-weight: bold; }";

}  // namespace

// ── TunerApplet ─────────────────────────────────────────────────────────────

TunerApplet::TunerApplet(QWidget* parent)
    : QWidget(parent)
{
    theme::setContainer(this, QStringLiteral("applet/tuner"));
    hide();   // hidden by default until toggled on
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    // Label clear timer: once power drops below threshold, wait 800 ms before
    // blanking the PWR/SWR text so inter-packet noise doesn't cause blinking.
    m_labelClearTimer = new QTimer(this);
    m_labelClearTimer->setSingleShot(true);
    m_labelClearTimer->setInterval(800);
    connect(m_labelClearTimer, &QTimer::timeout, this, [this]() {
        m_labelShowing = false;
        m_pwrLabel->setText("PWR");
        m_swrLabel->setText("SWR");
    });

    // Peak hold: clear the white tick 2.5 s after the last new peak.
    m_peakTimer = new QTimer(this);
    m_peakTimer->setSingleShot(true);
    m_peakTimer->setInterval(2500);
    connect(m_peakTimer, &QTimer::timeout, this, [this]() {
        m_peakFwd = 0.0f;
        static_cast<HGauge*>(m_fwdGauge)->clearPeak();
    });

    buildUI();
    applyDensity();
}

void TunerApplet::applyTuneButtonText(const QString& text)
{
    m_tuneBtn->setText(text);
    m_panelTuneBtn->setText(text);
}

void TunerApplet::applyTuneButtonStyle(const char* styleTemplate)
{
    auto& theme = AetherSDR::ThemeManager::instance();
    theme.applyStyleSheet(m_tuneBtn, styleTemplate);
    theme.applyStyleSheet(m_panelTuneBtn, styleTemplate);
}

void TunerApplet::setAmplifierMode(bool hasAmp)
{
    setPowerScale(100, hasAmp);
}

void TunerApplet::setPowerScale(int maxWatts, bool hasAmplifier)
{
    if (m_havePowerScale && maxWatts == m_lastMaxWatts && hasAmplifier == m_lastHasAmplifier) {
        return;
    }
    m_havePowerScale = true;
    m_lastMaxWatts = maxWatts;
    m_lastHasAmplifier = hasAmplifier;

    auto* gauge = static_cast<HGauge*>(m_fwdGauge);
    if (hasAmplifier) {
        // PGXL: 0–2000 W, yellow > 1000 W, red > 1500 W
        gauge->setRange(0.0f, 2000.0f, 1500.0f,
            {{0, "0"}, {500, "500"}, {1000, "1K"}, {1500, "1.5K"}, {2000, "2K"}},
            1000.0f);
    } else if (maxWatts > 100) {
        // Aurora (500 W): 0–600 W, yellow > 400 W, red > 500 W
        gauge->setRange(0.0f, 600.0f, 500.0f,
            {{0, "0"}, {100, "100"}, {200, "200"}, {300, "300"},
             {400, "400"}, {500, "500"}, {600, "600"}},
            400.0f);
    } else {
        // Barefoot radio: 0–200 W, yellow > 80 W, red > 125 W
        gauge->setRange(0.0f, 200.0f, 125.0f,
            {{0, "0"}, {50, "50"}, {100, "100"}, {150, "150"}, {200, "200"}},
            80.0f);
    }
}

void TunerApplet::buildUI()
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // Body with margins
    auto* body = new QWidget;
    m_vbox = new QVBoxLayout(body);
    auto* vbox = m_vbox;
    vbox->setContentsMargins(4, 2, 4, 2);
    vbox->setSpacing(2);

    static const char* kRowLabelStyle =
        "QLabel { color: #c8d8e8; font-size: 11px; font-weight: bold; }";

    // Forward Power gauge — default barefoot (0–200 W); switches to
    // 0–2000 W if a PGXL amplifier is detected via setAmplifierMode().
    // External row label carries the live value; internal gauge label is empty.
    m_pwrLabel = new QLabel("PWR", this);
    m_pwrLabel->setFixedWidth(72);
    m_pwrLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_pwrLabel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_pwrLabel->setStyleSheet(kRowLabelStyle);
    m_fwdGauge = new HGauge(0.0f, 200.0f, 125.0f, "", "",
        {{0, "0"}, {50, "50"}, {100, "100"}, {150, "150"}, {200, "200"}},
        this, 80.0f);
    // Slow release: bar rises quickly on RF bursts but decays over ~800 ms
    static_cast<HGauge*>(m_fwdGauge)->setBallistics({0.030f, 0.800f});
    m_fwdGauge->setAccessibleName(tr("Forward power"));
    auto* pwrRow = new QHBoxLayout;
    pwrRow->setSpacing(4);
    pwrRow->addWidget(m_pwrLabel);
    pwrRow->addWidget(m_fwdGauge, 1);
    vbox->addLayout(pwrRow);

    // SWR gauge
    m_swrLabel = new QLabel("SWR", this);
    m_swrLabel->setFixedWidth(72);
    m_swrLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_swrLabel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_swrLabel->setStyleSheet(kRowLabelStyle);
    m_swrGauge = new HGauge(1.0f, 3.0f, 2.5f, "", "",
        {{1.0f, "1"}, {1.5f, "1.5"}, {2.5f, "2.5"}, {3.0f, "3"}},
        this, 2.0f);
    m_swrGauge->setAccessibleName(tr("SWR"));
    auto* swrRow = new QHBoxLayout;
    swrRow->setSpacing(4);
    swrRow->addWidget(m_swrLabel);
    swrRow->addWidget(m_swrGauge, 1);
    vbox->addLayout(swrRow);

    // Port status strips — between the meters and the relay controls, where
    // the tuner's own panel puts them. Expanded presentation only.
    {
        m_portRowsBox = new QWidget;
        auto* area = new QVBoxLayout(m_portRowsBox);
        area->setContentsMargins(0, 0, 0, 0);
        area->setSpacing(0);

        // Live: the two strips, with the tuner-wide bypass indicator beside
        // them spanning both.
        m_portLiveBox = new QWidget(m_portRowsBox);
        auto* live = new QHBoxLayout(m_portLiveBox);
        live->setContentsMargins(0, 0, 0, 0);
        live->setSpacing(4);

        auto* rows = new QVBoxLayout;
        rows->setContentsMargins(0, 0, 0, 0);
        rows->setSpacing(2);
        m_portA = new TgxlPortRow(QStringLiteral("A"), m_portLiveBox);
        m_portB = new TgxlPortRow(QStringLiteral("B"), m_portLiveBox);
        rows->addWidget(m_portA);
        rows->addWidget(m_portB);
        live->addLayout(rows, 1);

        m_bypassSpan = new QLabel(tr("BYP"), m_portLiveBox);
        m_bypassSpan->setAlignment(Qt::AlignCenter);
        // Fills the height of the two strips beside it, which a box layout
        // does for free — Expanding here would instead make the strip block
        // compete with the bottom pad for slack and stretch both rows.
        m_bypassSpan->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
        m_bypassSpan->setVisible(false);
        m_bypassSpan->setAccessibleName(tr("Tuner bypassed"));
        live->addWidget(m_bypassSpan);

        area->addWidget(m_portLiveBox);

        // Standby: one banner over the whole area.
        m_standbyBanner = new QLabel(tr("STANDBY"), m_portRowsBox);
        m_standbyBanner->setAlignment(Qt::AlignCenter);
        m_standbyBanner->setVisible(false);
        m_standbyBanner->setAccessibleName(tr("Tuner in standby"));
        area->addWidget(m_standbyBanner);

        // Fixed, not Maximum: Maximum still lets the layout shrink it, and
        // then a panel being dragged shorter squeezes the strips while the
        // pad below them is still metres deep. See applyDensity's note.
        m_portRowsBox->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        vbox->addWidget(m_portRowsBox);
    }

    // Bottom section: relay bars (75% left) + buttons (25% right)
    m_dockedControls = new QWidget;
    auto* bottomRow = new QHBoxLayout(m_dockedControls);
    bottomRow->setContentsMargins(0, 0, 0, 0);
    bottomRow->setSpacing(4);

    // Left column: relay bars
    auto* relayCol = new QVBoxLayout;
    relayCol->setSpacing(2);
    m_c1Bar = new RelayBar("C1");
    m_c1Bar->setAccessibleName(tr("Tuner capacitor C1"));
    m_lBar  = new RelayBar("L");
    m_lBar->setAccessibleName(tr("Tuner inductor L"));
    m_c2Bar = new RelayBar("C2");
    m_c2Bar->setAccessibleName(tr("Tuner capacitor C2"));
    relayCol->addWidget(m_c1Bar);
    relayCol->addWidget(m_lBar);
    relayCol->addWidget(m_c2Bar);
    bottomRow->addLayout(relayCol, 7);  // stretch 7 (70%)

    // Right column: buttons
    auto* btnCol = new QVBoxLayout;
    btnCol->setSpacing(2);

    m_tuneBtn = new QPushButton("TUNE");
    m_tuneBtn->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_tuneBtn, kTuneIdleStyle);
    btnCol->addWidget(m_tuneBtn);

    m_operateBtn = new QPushButton("OPERATE");
    m_operateBtn->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_operateBtn, "QPushButton { background: {{color.background.2}}; border: 1px solid {{color.background.2}}; "
        "border-radius: 3px; color: {{color.text.primary}}; font-size: 10px; font-weight: bold; }"
        "QPushButton:hover { background: {{color.background.1}}; }");
    btnCol->addWidget(m_operateBtn);

    bottomRow->addLayout(btnCol, 3);  // stretch 3 (30%)

    vbox->addWidget(m_dockedControls);

    // Expanded controls (dials + discrete keys), hidden while docked.
    buildExpandedUI(vbox);

    // Antenna switch row (TGXL 3x1) — hidden until direct connection active
    {
        m_antContainer = new QWidget;
        m_antContainer->setVisible(false);
        m_antContainer->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        auto* antRow = new QHBoxLayout(m_antContainer);
        antRow->setContentsMargins(0, 0, 0, 0);
        antRow->setSpacing(2);

        auto makeAntBtn = [](const QString& text) {
            auto* btn = new QPushButton(text);
            btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            btn->setFixedHeight(22);
            AetherSDR::ThemeManager::instance().applyStyleSheet(btn, "QPushButton { background: {{color.background.1}}; border: 1px solid {{color.background.2}}; "
                "border-radius: 3px; color: {{color.text.primary}}; font-size: 10px; font-weight: bold; }"
                "QPushButton:hover { background: {{color.background.1}}; }");
            return btn;
        };

        m_ant1Btn = makeAntBtn("ANT 1");
        m_ant2Btn = makeAntBtn("ANT 2");
        m_ant3Btn = makeAntBtn("ANT 3");
        antRow->addWidget(m_ant1Btn);
        antRow->addWidget(m_ant2Btn);
        antRow->addWidget(m_ant3Btn);

        connect(m_ant1Btn, &QPushButton::clicked, this, [this]() {
            if (m_model) m_model->setAntennaA(1);
        });
        connect(m_ant2Btn, &QPushButton::clicked, this, [this]() {
            if (m_model) m_model->setAntennaA(2);
        });
        connect(m_ant3Btn, &QPushButton::clicked, this, [this]() {
            if (m_model) m_model->setAntennaA(3);
        });

        vbox->addWidget(m_antContainer);
    }

    // One pad under the controls, absorbing whatever the scaling did not use.
    // kBottomGap is its floor rather than a margin on the layout so there is a
    // single thing deciding the space below the controls.
    m_bottomStretch = new QSpacerItem(0, kBottomGap,
                                      QSizePolicy::Minimum, QSizePolicy::Fixed);
    vbox->addSpacerItem(m_bottomStretch);

    updatePortRows();

    outer->addWidget(body);

    // The alert overlay is a child of the applet rather than a row in its
    // layout, so it can cover the whole thing. Created last so it sits on top
    // of everything already added.
    m_alertOverlay = new QLabel(this);
    m_alertOverlay->setAlignment(Qt::AlignCenter);
    m_alertOverlay->setWordWrap(true);
    m_alertOverlay->setVisible(false);
    m_alertOverlay->setAccessibleName(tr("Tuner alert"));
    // Deliberately NOT transparent to mouse events. While it is up the
    // controls beneath it cannot be seen, and a click that lands on something
    // invisible is worse than one that lands on nothing. Nothing is lost:
    // every alert the tuner sends arrives after the tune has already stopped.

    // TUNE key: starts a tune, or stops the one already running. Which it
    // does follows m_tuning, the same flag that decides the caption, so the
    // key can never say STOP and start a tune.
    for (auto* tune : {m_tuneBtn, m_panelTuneBtn}) {
        connect(tune, &QPushButton::clicked, this, [this]() {
            if (!m_model) return;
            if (m_tuning) {
                m_model->abortTune();
                return;
            }
            m_model->autoTune();
        });
    }

    // Manual relay adjustment via mousewheel scroll (#469) — the bar and the
    // dial are two views of one relay bank, so both drive the same step.
    connect(static_cast<RelayBar*>(m_c1Bar), &RelayBar::relayAdjusted, this,
            [this](int dir) { if (m_model) m_model->adjustRelay(0, dir); });
    connect(static_cast<RelayBar*>(m_lBar), &RelayBar::relayAdjusted, this,
            [this](int dir) { if (m_model) m_model->adjustRelay(1, dir); });
    connect(static_cast<RelayBar*>(m_c2Bar), &RelayBar::relayAdjusted, this,
            [this](int dir) { if (m_model) m_model->adjustRelay(2, dir); });
    connect(m_c1Dial, &RelayDial::relayAdjusted, this,
            [this](int dir) { if (m_model) m_model->adjustRelay(0, dir); });
    connect(m_lDial, &RelayDial::relayAdjusted, this,
            [this](int dir) { if (m_model) m_model->adjustRelay(1, dir); });
    connect(m_c2Dial, &RelayDial::relayAdjusted, this,
            [this](int dir) { if (m_model) m_model->adjustRelay(2, dir); });

    // OPERATE button: cycle through OPERATE → BYPASS → STANDBY → OPERATE
    connect(m_operateBtn, &QPushButton::clicked, this,
            &TunerApplet::cycleOperateState);
}

// ── Expanded (floating / canvas) presentation ───────────────────────────────

void TunerApplet::buildExpandedUI(QVBoxLayout* vbox)
{
    auto& theme = AetherSDR::ThemeManager::instance();

    m_panelControls = new QWidget;
    auto* row = new QHBoxLayout(m_panelControls);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(6);

    // Three relay dials on the left.
    auto* dials = new QHBoxLayout;
    dials->setSpacing(6);
    m_c1Dial = new RelayDial(QStringLiteral("C1"), m_panelControls);
    m_c1Dial->setAccessibleName(tr("Tuner capacitor C1"));
    m_lDial = new RelayDial(QStringLiteral("L"), m_panelControls);
    m_lDial->setAccessibleName(tr("Tuner inductor L"));
    m_c2Dial = new RelayDial(QStringLiteral("C2"), m_panelControls);
    m_c2Dial->setAccessibleName(tr("Tuner capacitor C2"));
    dials->addWidget(m_c1Dial);
    dials->addWidget(m_lDial);
    dials->addWidget(m_c2Dial);
    row->addLayout(dials);

    // Discrete keys on the right. STBY and BYP each toggle their own state
    // against OPERATE, so the state the panel is in is always one press from
    // the state it came from — the rail's single cycling button cannot do
    // that, which is why it stays the rail's button and not this one.
    m_keysLayout = new QHBoxLayout;
    auto* keys = m_keysLayout;
    keys->setSpacing(4);
    auto makeKey = [this](const QString& text, const QString& tip) {
        auto* btn = new QPushButton(text, m_panelControls);
        // Wide as the column allows; the height follows from kKeyAspect once
        // the layout has settled that width (see applyKeyAspect).
        btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        btn->setToolTip(tip);
        btn->setAccessibleName(text);
        btn->setAccessibleDescription(tip);
        return btn;
    };
    m_stbyBtn = makeKey(tr("STBY"), tr("Toggle standby — the tuner stops following the radio"));
    m_bypBtn  = makeKey(tr("BYP"),  tr("Toggle bypass — RF passes straight through the tuner"));
    m_panelTuneBtn = makeKey(tr("TUNE"), tr("Start an auto-tune cycle"));
    for (auto* key : {m_stbyBtn, m_bypBtn}) {
        theme.applyStyleSheet(key, kPanelKeyIdleStyle);
    }
    theme.applyStyleSheet(m_panelTuneBtn, kTuneIdleStyle);
    // Centred, because a key capped shorter than the row would otherwise sit
    // against its top edge rather than level with the dials.
    keys->addWidget(m_stbyBtn, 0, Qt::AlignVCenter);
    keys->addWidget(m_bypBtn, 0, Qt::AlignVCenter);
    keys->addWidget(m_panelTuneBtn, 0, Qt::AlignVCenter);
    // Dials left, keys right, the slack between them. Every widget in this
    // row is sized from the scale rather than by stretching, so the row needs
    // somewhere to put spare width that is not inside either group.
    row->addStretch(1);
    row->addLayout(keys);

    // Seed size: the widest caption at the design font, measured now, while
    // the keys still have their natural size hints. "STOP" is included
    // because TUNE becomes it mid-tune and the keys must not resize when it
    // does.
    {
        QFont seedFont = m_stbyBtn->font();
        seedFont.setPixelSize(kKeyFontDesignPx);
        const QFontMetrics fm(seedFont);
        int textWidth = 0;
        for (const QString& caption : {m_stbyBtn->text(), m_bypBtn->text(),
                                       m_panelTuneBtn->text(), tr("STOP")}) {
            textWidth = qMax(textWidth, fm.horizontalAdvance(caption));
        }
        m_keySeedWidth = textWidth + kKeyPaddingDesignPx;
    }

    m_panelControls->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    vbox->addWidget(m_panelControls);

    connect(m_stbyBtn, &QPushButton::clicked, this, [this]() {
        if (!m_model) return;
        if (!m_model->isOperate()) {
            // Already in standby — return to operate. Same order as
            // cycleOperateState's standby leg so both paths command the
            // tuner identically.
            m_model->setBypass(false);
            m_model->setOperate(true);
        } else {
            m_model->setBypass(false);
            m_model->setOperate(false);
        }
    });
    connect(m_bypBtn, &QPushButton::clicked, this, [this]() {
        if (!m_model) return;
        if (m_model->isOperate() && m_model->isBypass()) {
            m_model->setBypass(false);   // back to operate, still out of standby
        } else {
            m_model->setOperate(true);
            m_model->setBypass(true);
        }
    });
}

void TunerApplet::setFloating(bool floating)
{
    if (floating == m_floating) return;
    m_floating = floating;
    applyDensity();
}

qreal TunerApplet::contentScale() const
{
    // Docked, the rail gives every tile the same width and a fixed height;
    // scaling there would make one tile disagree with its neighbours.
    if (!m_floating) return 1.0;
    if (width() <= 0 || height() <= 0) return 1.0;

    // The height term budgets for the contents ONLY — the pad's minimum is
    // taken off first, and the divisor is what the contents actually need at
    // scale 1.0. Those two together are what make the pad drain before
    // anything above it moves: while the width is the limiting term the
    // contents hold their size and the surplus is all pad, and the moment
    // height becomes limiting the arithmetic lands the contents at exactly
    // height - kBottomGap, so the pad is at its minimum rather than still
    // holding space that the contents just gave up.
    const qreal natural = m_naturalContentHeight > 1.0 ? m_naturalContentHeight
                                                       : kDesignHeight;
    return qBound(kMinScale,
                  qMin(width() / kDesignWidth,
                       (height() - kBottomGap) / natural),
                  kMaxScale);
}

void TunerApplet::applyDensity()
{
    auto& theme = AetherSDR::ThemeManager::instance();
    const bool f = m_floating;
    const qreal s = contentScale();
    // A design-pixel metric at the current scale.
    auto px = [s](int base) { return qMax(1, qRound(base * s)); };

    // No bottom margin: m_bottomStretch owns the space under the controls.
    m_vbox->setContentsMargins(f ? px(12) : 4, f ? px(10) : 2,
                               f ? px(12) : 4, f ? 0 : 2);
    m_vbox->setSpacing(f ? px(8) : 2);

    // Expanded fills the window it was given; docked stays the fixed-height
    // tile the rail stacks.
    setSizePolicy(QSizePolicy::Preferred,
                  f ? QSizePolicy::Preferred : QSizePolicy::Fixed);

    for (auto* lbl : {m_pwrLabel, m_swrLabel}) {
        lbl->setFixedWidth(f ? px(96) : 72);
        theme.applyStyleSheet(lbl, QStringLiteral(
            "QLabel { color: {{color.text.primary}}; font-size: %1px; font-weight: bold; }")
            .arg(f ? px(14) : 11));
    }
    for (auto* gauge : {m_fwdGauge, m_swrGauge}) {
        gauge->setFixedHeight(f ? px(34) : 24);
        // The bar grows with the panel; without this its tick lettering would
        // not, which is most of what "it just stretches" looks like.
        static_cast<HGauge*>(gauge)->setMetricScale(f ? s : 1.0);
    }

    m_portA->setScale(f ? s : 1.0);
    m_portB->setScale(f ? s : 1.0);
    for (auto* dial : {m_c1Dial, m_lDial, m_c2Dial}) {
        dial->setPreferredDiameter(px(76));
    }

    // The SWR bar carries its scale as a gradient across the empty track —
    // the one piece of the panel that is a colour, not a layout, so it is
    // resolved from the theme rather than copied off the hardware.
    QGradientStops swrStops;
    if (f) {
        const ThemeGradient scale =
            theme.gradient(this, QStringLiteral("color.tgxl.swrScale"));
        for (const ThemeGradientStop& stop : scale.stops) {
            swrStops.append({stop.at, stop.color});
        }
    }
    static_cast<HGauge*>(m_swrGauge)->setTrackGradient(swrStops);

    applyAlertStyle();
    theme.applyStyleSheet(m_bypassSpan, QStringLiteral(
        "QLabel { border: 2px solid {{color.accent.warning}}; border-radius: 3px; "
        "background: {{color.background.1}}; color: {{color.accent.warning}}; "
        "padding: 0 %1px; font-size: %2px; font-weight: bold; }")
        .arg(px(6)).arg(px(12)));
    theme.applyStyleSheet(m_standbyBanner, QStringLiteral(
        "QLabel { border: 2px solid {{color.tgxl.key.standby.foreground}}; "
        "border-radius: 3px; background: {{color.tgxl.key.standby.background}}; "
        "color: {{color.tgxl.key.standby.foreground}}; "
        "letter-spacing: 2px; font-size: %1px; font-weight: bold; }")
        .arg(px(20)));
    // The banner stands in for both strips, so it claims their combined height
    // — otherwise the panel jumps every time the tuner enters standby.
    m_standbyBanner->setMinimumHeight(m_portA->sizeHint().height() * 2 + 2);

    // Expanding only when popped out: docked, the rail already fixes the
    // tile's height and there is no slack for a pad to take.
    //
    // This is the ONLY item in the column that may change height. Everything
    // above it is vertically Fixed, so the layout has exactly one place to
    // put spare height and exactly one place to take it from: dragging the
    // panel shorter drains the pad to kBottomGap before anything else moves,
    // instead of compressing the strips and dials on the way down. Sizing the
    // contents is the scale's job, not the layout's.
    m_bottomStretch->changeSize(0, kBottomGap, QSizePolicy::Minimum,
                                f ? QSizePolicy::Expanding : QSizePolicy::Fixed);

    m_portRowsBox->setVisible(f);
    m_panelControls->setVisible(f);
    m_dockedControls->setVisible(!f);

    for (auto* btn : {m_stbyBtn, m_bypBtn, m_panelTuneBtn}) {
        QFont keyFont = btn->font();
        keyFont.setPixelSize(px(kKeyFontDesignPx));
        btn->setFont(keyFont);
    }
    applyKeySize(f ? s : 1.0);
    // The rail's own two keys keep the compact size they always had.
    for (auto* btn : {m_tuneBtn, m_operateBtn}) {
        QFont railFont = btn->font();
        railFont.setPixelSize(10);
        btn->setFont(railFont);
    }
    theme.applyStyleSheet(m_panelControls, QString());
    if (auto* row = qobject_cast<QHBoxLayout*>(m_panelControls->layout())) {
        row->setSpacing(px(6));
    }

    m_vbox->invalidate();
    // The expanded control groups were just shown or hidden, and the state
    // colouring on STBY/BYP lives in syncFromModel.
    syncFromModel();
}

void TunerApplet::setRadioModelName(const QString& model)
{
    if (m_radioModelName == model) return;
    m_radioModelName = model;
    updatePortRows();
}

void TunerApplet::setPortAFrequencyMhz(double mhz)
{
    if (qFuzzyCompare(m_portAFreqMhz + 1.0, mhz + 1.0)) return;
    m_portAFreqMhz = mhz;
    updatePortRows();
}

void TunerApplet::setRadioConnected(bool connected)
{
    if (m_radioConnected == connected) return;
    m_radioConnected = connected;
    updatePortRows();
}

void TunerApplet::layOutAlertOverlay()
{
    if (!m_alertOverlay) return;
    m_alertOverlay->setGeometry(rect());
}

void TunerApplet::measureNaturalHeight()
{
    // What the contents occupy at scale 1.0, taken from the laid-out column
    // rather than from its size hint — the hint under-reports by the margins
    // the column adds around it, and a budget built on it lets the pad be
    // squeezed past its minimum before the scale ever reacts.
    //
    // Everything above the pad is vertically fixed, so whatever the pad is
    // not occupying is exactly the contents. Dividing out the scale they were
    // drawn at leaves the panel's natural height, whatever rows it has.
    if (!m_floating || !m_bottomStretch) return;
    if (m_appliedScale <= 0.0) return;
    const qreal content = height() - m_bottomStretch->geometry().height();
    if (content > 1.0) {
        m_naturalContentHeight = content / m_appliedScale;
    }
}

void TunerApplet::applyKeySize(qreal scale)
{
    if (!m_stbyBtn || m_keySeedWidth <= 0) return;

    // All three keys are one size, grown from one seed. The seed is the
    // widest caption's natural width, so the narrowest caption gets the same
    // box as the widest rather than the box its own text happened to need,
    // and the height follows at kKeyAspect.
    const int w = qMax(1, qRound(m_keySeedWidth * scale));
    const int h = qMax(1, qRound(w / kKeyAspect));
    for (auto* btn : {m_stbyBtn, m_bypBtn, m_panelTuneBtn}) {
        // Fixed in both axes: the keys are laid out with AlignVCenter so they
        // sit level with the dials, and that makes the layout take their size
        // hint rather than stretch them — a maximum alone would never bind.
        if (btn->minimumSize() != QSize(w, h) || btn->maximumSize() != QSize(w, h)) {
            btn->setFixedSize(w, h);
        }
    }
}

void TunerApplet::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    layOutAlertOverlay();

    // Re-derive the metrics for the new size, but only when the scale has
    // actually moved: a resize arrives for every pixel of a window drag and
    // applyDensity re-applies a dozen style sheets.
    const qreal s = contentScale();
    if (!qFuzzyCompare(s, m_appliedScale)) {
        m_appliedScale = s;
        applyDensity();
    }
    measureNaturalHeight();
}

void TunerApplet::applyAlertStyle()
{
    // The protocol carries no severity field, so it is inferred from the one
    // thing available -- the text. "Tuned ..." is the completion notice and is
    // the tuner reporting success; anything else is something the operator has
    // to act on ("LOW RF POWER" is the other one seen). Unknown text therefore
    // lands on the attention colour, which is the safe way round: a warning
    // shown as good news is worse than the reverse.
    if (!m_alertOverlay) return;
    auto& theme = AetherSDR::ThemeManager::instance();
    const QString tone = m_alertIsGood ? QStringLiteral("{{color.accent.success}}")
                                       : QStringLiteral("{{color.accent.danger}}");
    // Opaque ground: the overlay has to obscure the readings under it, not
    // tint them. The applet's own darkest ground rather than a literal black,
    // so it still reads correctly under a light theme.
    const QString style = QStringLiteral(
        "QLabel { background: {{color.background.0}}; color: %1; "
        "border: none; padding: 8px; font-size: %2px; font-weight: bold; }")
        .arg(tone)
        .arg(m_floating ? 26 : 15);
    theme.applyStyleSheet(m_alertOverlay, style);
}

void TunerApplet::setAlertText(const QString& text)
{
    const QString shown = text.trimmed();
    if (m_alertOverlay->text() == shown) return;
    m_alertOverlay->setText(shown);

    m_alertIsGood = shown.startsWith(QLatin1String("Tuned"), Qt::CaseInsensitive);
    applyAlertStyle();

    // The tuner raises the alert and later clears it with an empty frame; the
    // overlay simply follows those two, so its dwell time is whatever the
    // device chose.
    if (shown.isEmpty()) {
        m_alertOverlay->hide();
    } else {
        layOutAlertOverlay();
        m_alertOverlay->raise();
        m_alertOverlay->show();
    }

    // Announce it: the strip appears and disappears on the tuner's schedule,
    // and a reader that is not looking at it would otherwise never learn a
    // tune had refused to run.
    if (!shown.isEmpty()) {
        m_alertOverlay->setAccessibleDescription(shown);
        if (QAccessible::isActive()) {
            QAccessibleEvent event(m_alertOverlay, QAccessible::Alert);
            QAccessible::updateAccessibility(&event);
        }
    }
}

void TunerApplet::updatePortRows()
{
    if (!m_portA || !m_portB) return;

    // Preferred: the tuner's own per-port readings, which arrive on the
    // direct port-9010 status. It reports what each port is actually hearing,
    // where the client can only report the one radio it happens to be
    // connected to.
    if (m_model && m_model->hasDirectConnection() && m_model->hasPortInfo()) {
        applyPortInfo(m_portA, m_model->portA());
        applyPortInfo(m_portB, m_model->portB());
        updateActivePort();
        return;
    }

    // Fallback: the Flex-relayed "amplifier" status carries no per-port block
    // at all, so without the direct connection this is the client's view of
    // its own radio rather than the tuner's report. Port A is assumed to be
    // the networked radio's and port B to be on RF sense — true of the common
    // wiring, and the honest limit of what is knowable on this path.
    const QString modelName = m_radioModelName.trimmed();
    m_portA->setSourceText(m_radioConnected && !modelName.isEmpty()
                               ? modelName
                               : tr("NO RADIO"));
    m_portB->setSourceText(tr("RF SENSE"));

    const bool haveFreq = m_radioConnected && m_portAFreqMhz > 0.0;
    m_portA->setFrequencyMhz(haveFreq ? m_portAFreqMhz : 0.0);
    m_portA->setBandText(haveFreq ? BandSettings::bandForFrequency(m_portAFreqMhz)
                                  : QString());
    m_portB->setFrequencyMhz(0.0);
    m_portB->setBandText(QString());

    if (m_model) {
        m_portA->setPtt(m_model->pttA());
        m_portB->setPtt(m_model->pttB());
    }
    updateActivePort();
}

void TunerApplet::applyPortInfo(TgxlPortRow* row, const TunerPortInfo& info)
{
    // A port the tuner has no live reading on is one nothing is being heard
    // on. It is labelled RF SENSE rather than with the radio name the tuner
    // reports there anyway: `flexB` reads FLEX-8600 on a port carrying
    // nothing, so trusting it would put a radio on a port that has none.
    row->setSourceText(info.live && !info.source.trimmed().isEmpty()
                           ? info.source.trimmed()
                           : tr("RF SENSE"));

    // freqX is kHz on the wire. The band comes from that frequency through
    // the project's own band table rather than from the tuner's `bandX`
    // index — one band value was all a capture ever showed, and a mapping
    // guessed from a single sample would be wrong silently.
    const double mhz = info.live ? info.freqKhz / 1000.0 : 0.0;
    row->setFrequencyMhz(mhz);
    row->setBandText(mhz > 0.0 ? BandSettings::bandForFrequency(mhz) : QString());

    row->setPtt(info.ptt);
}

void TunerApplet::updateActivePort()
{
    if (!m_portA || !m_portB) return;

    // Which port transmits is settled by matching the TX slice's antenna
    // against each port's configured one — the comparison FlexLib itself
    // makes before it will autotune (Tuner.AutoTune checks TXAnt against
    // PortAAnt/PortBAnt). The tuner's own status cannot answer it: with one
    // radio cabled to both ports, modeA and modeB both read 1 and `active`
    // never moves, so an outline driven from those lights up both rows.
    const QString tx = m_txAntenna.trimmed();
    const QString aAnt = m_model ? m_model->portAAnt().trimmed() : QString();
    const QString bAnt = m_model ? m_model->portBAnt().trimmed() : QString();

    const bool aIsTx = !tx.isEmpty() && !aAnt.isEmpty()
                       && aAnt.compare(tx, Qt::CaseInsensitive) == 0;
    const bool bIsTx = !tx.isEmpty() && !bAnt.isEmpty()
                       && bAnt.compare(tx, Qt::CaseInsensitive) == 0;

    // Neither matching means the radio is transmitting on an antenna that
    // does not run through the tuner at all — FlexLib declines to autotune in
    // exactly that case. Outlining nothing is the honest answer; outlining a
    // port would claim RF is passing through it.
    m_portA->setActive(aIsTx);
    m_portB->setActive(bIsTx);
}

void TunerApplet::setTxAntenna(const QString& antenna)
{
    if (m_txAntenna == antenna) return;
    m_txAntenna = antenna;
    updateActivePort();
}

void TunerApplet::setTunerModel(TunerModel* model)
{
    if (m_model == model) return;
    m_model = model;
    if (!m_model) return;

    // State changes → refresh UI
    connect(m_model, &TunerModel::stateChanged, this, &TunerApplet::syncFromModel);

    // Forward power and SWR from direct TGXL connection (#625)
    connect(m_model, &TunerModel::metersChanged,
            this, &TunerApplet::updateMeters);

    // Enable relay bar scrolling when direct TGXL connection is active (#469)
    auto updateScrollEnabled = [this]() {
        bool on = m_model && m_model->hasDirectConnection();
        static_cast<RelayBar*>(m_c1Bar)->setScrollEnabled(on);
        static_cast<RelayBar*>(m_lBar)->setScrollEnabled(on);
        static_cast<RelayBar*>(m_c2Bar)->setScrollEnabled(on);
        m_c1Dial->setScrollEnabled(on);
        m_lDial->setScrollEnabled(on);
        m_c2Dial->setScrollEnabled(on);
    };
    connect(m_model, &TunerModel::directConnectionChanged, this, updateScrollEnabled);
    updateScrollEnabled();

    // Antenna switch: show buttons only when direct connection is active AND
    // the TGXL reports antA (models without a switch never send antA).
    auto updateAntVisible = [this]() {
        m_antContainer->setVisible(m_model->hasDirectConnection()
                                   && m_model->hasAntennaSwitch());
    };
    connect(m_model, &TunerModel::portsChanged, this, &TunerApplet::updatePortRows);
    // stateChanged carries the port->antenna map, which arrives after the
    // applet is first built and decides which row is outlined.
    connect(m_model, &TunerModel::stateChanged, this, &TunerApplet::updateActivePort);
    connect(m_model, &TunerModel::directConnectionChanged, this,
            [this](bool) { updatePortRows(); });

    connect(m_model, &TunerModel::alertChanged, this, &TunerApplet::setAlertText);
    setAlertText(m_model->alert());

    connect(m_model, &TunerModel::pttChanged, this, [this](bool a, bool b) {
        m_portA->setPtt(a);
        m_portB->setPtt(b);
    });

    connect(m_model, &TunerModel::directConnectionChanged, this, updateAntVisible);
    connect(m_model, &TunerModel::antennaAChanged, this, [this, updateAntVisible](int antA) {
        updateAntVisible();
        updateAntennaButtons(antA);
    });
    updateAntVisible();
    updateAntennaButtons(m_model->antennaA());

    // Tuning state changes → the key's two states. The result of a tune is
    // the overlay's to report, not the key's: the tuner sends it as text with
    // its own dwell, and a key that also flashed a number would be a second
    // place for the same reading to disagree.
    connect(m_model, &TunerModel::tuningChanged, this, [this](bool tuning) {
        m_tuning = tuning;
        if (tuning) {
            applyTuneButtonStyle(kTuneBusyStyle);
            // The key is the abort while a tune is running, so it says what
            // pressing it will do rather than reporting what the tuner is up
            // to — the strips and the red styling already report that.
            applyTuneButtonText(tr("STOP"));
        } else {
            applyTuneButtonStyle(kTuneIdleStyle);
            applyTuneButtonText(tr("TUNE"));
        }
    });

    syncFromModel();
}

void TunerApplet::syncFromModel()
{
    if (!m_model) return;

    // Relay bars
    m_relayC1 = m_model->relayC1();
    m_relayL  = m_model->relayL();
    m_relayC2 = m_model->relayC2();
    static_cast<RelayBar*>(m_c1Bar)->setValue(m_relayC1);
    static_cast<RelayBar*>(m_lBar)->setValue(m_relayL);
    static_cast<RelayBar*>(m_c2Bar)->setValue(m_relayC2);
    m_c1Dial->setValue(m_relayC1);
    m_lDial->setValue(m_relayL);
    m_c2Dial->setValue(m_relayC2);

    // Operate/Bypass/Standby button — 3-state display
    // operate=1, bypass=0 → OPERATE (green)
    // operate=1, bypass=1 → BYPASS  (orange)
    // operate=0            → STANDBY (default)
    auto& theme = AetherSDR::ThemeManager::instance();
    const bool operate = m_model->isOperate();
    const bool bypass = m_model->isBypass();

    if (operate && !bypass) {
        m_operateBtn->setText("OPERATE");
        theme.applyStyleSheet(m_operateBtn, "QPushButton { background: #006030; border: 1px solid #008040; "
            "border-radius: 3px; color: {{color.text.primary}}; font-weight: bold; }"
            "QPushButton:hover { background: #007040; }");
    } else if (operate && bypass) {
        m_operateBtn->setText("BYPASS");
        theme.applyStyleSheet(m_operateBtn, "QPushButton { background: #8a6000; border: 1px solid #a07000; "
            "border-radius: 3px; color: {{color.text.primary}}; font-weight: bold; }"
            "QPushButton:hover { background: #9a7000; }");
    } else {
        m_operateBtn->setText("STANDBY");
        theme.applyStyleSheet(m_operateBtn, "QPushButton { background: {{color.background.2}}; border: 1px solid {{color.background.2}}; "
            "border-radius: 3px; color: {{color.text.primary}}; font-weight: bold; }"
            "QPushButton:hover { background: {{color.background.1}}; }");
    }

    // Expanded presentation: the same three states, but shown as the lit key
    // plus the port area's own presentation rather than one button's caption.
    theme.applyStyleSheet(m_stbyBtn, !operate ? kStandbyActiveStyle : kPanelKeyIdleStyle);
    theme.applyStyleSheet(m_bypBtn, (operate && bypass) ? kBypassActiveStyle : kPanelKeyIdleStyle);
    applyTunerStateToPorts(operate, bypass);

    updatePortRows();
}

void TunerApplet::applyTunerStateToPorts(bool operate, bool bypass)
{
    // Standby takes the whole area — with the tuner out of circuit, the
    // strips have nothing live left to report.
    const bool standby = !operate;
    m_standbyBanner->setVisible(standby);
    m_portLiveBox->setVisible(!standby);
    if (standby) return;

    // Bypass is tuner-wide, so it shows once beside both strips and the
    // per-port state cell goes empty rather than repeating it. The frequency
    // is still correct but is no longer being matched, so it reads in the
    // bypass colour.
    m_bypassSpan->setVisible(bypass);
    for (auto* row : {m_portA, m_portB}) {
        row->setStateText(bypass ? QString() : QStringLiteral("OPR"));
        row->setBypassed(bypass);
    }
}

void TunerApplet::cycleOperateState()
{
    if (!m_model) return;

    // Cycle: OPERATE → BYPASS → STANDBY → OPERATE
    if (m_model->isOperate() && !m_model->isBypass()) {
        // Currently OPERATE → go to BYPASS
        m_model->setBypass(true);
    } else if (m_model->isOperate() && m_model->isBypass()) {
        // Currently BYPASS → go to STANDBY
        m_model->setBypass(false);
        m_model->setOperate(false);
    } else {
        // Currently STANDBY → go to OPERATE
        m_model->setBypass(false);
        m_model->setOperate(true);
    }
}

void TunerApplet::updateMeters(float fwdPower, float swr)
{
    m_fwdPower = fwdPower;
    m_swr = swr;
    static_cast<HGauge*>(m_fwdGauge)->setValue(fwdPower);
    // TGXL sends swr=0.0000 (return loss = 0 dB) at idle — no incident signal
    // to measure against. The model converts that to rho=1.0 → ratio=99.9, which
    // pegs the gauge. Snap to 1.0 (empty) whenever forward power is below the
    // noise floor; m_swr retains the raw value for the numeric row label.
    // Threshold matches the label threshold (5 W) — 1 W was too low and let
    // idle noise readings light up the SWR bar.
    if (fwdPower >= 5.0f) {
        static_cast<HGauge*>(m_swrGauge)->setValue(swr);
    } else {
        static_cast<HGauge*>(m_swrGauge)->setValueImmediate(1.0f);
    }
    if (fwdPower > m_peakFwd) {
        m_peakFwd = fwdPower;
        static_cast<HGauge*>(m_fwdGauge)->setPeakValue(fwdPower);
        m_peakTimer->start();
    }
    updateValueLabels();
}

void TunerApplet::updateValueLabels()
{
    if (m_fwdPower >= 5.0f) {
        m_labelClearTimer->stop();
        m_labelShowing = true;
        m_pwrLabel->setText(QStringLiteral("PWR  %1").arg(static_cast<int>(m_fwdPower)));
        m_swrLabel->setText(QStringLiteral("SWR  %1:1").arg(m_swr, 0, 'f', 1));
    } else if (m_labelShowing && !m_labelClearTimer->isActive()) {
        // Power dropped — start hold window before blanking
        m_labelClearTimer->start();
    }
}

void TunerApplet::updateAntennaButtons(int antA)
{
    // antA is 0-indexed: 0=ANT1, 1=ANT2, 2=ANT3
    static constexpr const char* kDefault =
        "QPushButton { background: #1a2a3a; border: 1px solid #205070; "
        "border-radius: 3px; color: #c8d8e8; font-size: 10px; font-weight: bold; }"
        "QPushButton:hover { background: #204060; }";
    static constexpr const char* kActive =
        "QPushButton { background: #006030; border: 1px solid #008040; "
        "border-radius: 3px; color: #ffffff; font-size: 10px; font-weight: bold; }";

    m_ant1Btn->setStyleSheet(antA == 0 ? kActive : kDefault);
    m_ant2Btn->setStyleSheet(antA == 1 ? kActive : kDefault);
    m_ant3Btn->setStyleSheet(antA == 2 ? kActive : kDefault);
}

} // namespace AetherSDR

// RelayBar has Q_OBJECT in a header-only class — include MOC output here
#include "moc_HGauge.cpp"
