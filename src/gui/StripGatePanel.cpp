#include "StripGatePanel.h"
#include "PanelTick.h"
#include "ClientCompKnob.h"
#include "ClientGateCurveWidget.h"
#include "ClientGateLevelView.h"
#include "EditorFramelessTitleBar.h"
#include "Theme.h"
#include "core/AppSettings.h"
#include "core/AudioEngine.h"
#include "core/ClientGate.h"

#include <QCloseEvent>
#include <QButtonGroup>
#include <QComboBox>
#include <QHBoxLayout>
#include <QStackedWidget>
#include <QHideEvent>
#include <QLabel>
#include <QMoveEvent>
#include <QSlider>
#include <QPushButton>
#include <QResizeEvent>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>

namespace AetherSDR {

namespace {

constexpr int kDefaultWidth  = 760;
constexpr int kDefaultHeight = 380;

constexpr const char* kWindowStyle =
    "QWidget { background: #08121d; color: #d7e7f2; }"
    "QLabel  { background: transparent; color: #8aa8c0; font-size: 11px; }";

const QString kBypassStyle = QStringLiteral(
    "QPushButton {"
    "  background: #0e1b28;"
    "  color: #8aa8c0;"
    "  border: 1px solid #243a4e;"
    "  border-radius: 3px;"
    "  font-size: 11px;"
    "  font-weight: bold;"
    "  padding: 3px 12px;"
    "}"
    "QPushButton:hover { background: #1a2a3a; }"
    "QPushButton:checked {"
    "  background: #3a2a0e;"
    "  color: #f2c14e;"
    "  border: 1px solid #f2c14e;"
    "}"
    "QPushButton:checked:hover { background: #4a3a1e; }");

// Flip (Expander/Gate) — two custom checked states instead of the
// usual idle→active swap: Expander (unchecked) uses the SQL/MIC
// green palette, Gate (checked) uses the amber palette that marks
// more-aggressive settings elsewhere in the chain.  The colour
// itself tells you which mode you're in without reading the label.
// Unchecked is muted, checked is lit. The green-when-off styling this
// inherited from the single Flip button was readable while only one of these
// existed at a time; with both halves of a pair on screen it said that Level
// and Curve were both on, and only the shade told you which.
// Tokens, not literals: the unchecked half of each pair is the panel's own
// ground and label colour, and the checked half is the transmit-amber the rest
// of the app already uses to mark a more aggressive setting. Applied through
// ThemeManager::applyStyleSheet() so the {{...}} resolve.
const QString kFlipStyle = QStringLiteral(
    "QPushButton {"
    "  background: {{color.background.1}};"
    "  border: 1px solid {{color.border.strong}}; border-radius: 3px;"
    "  color: {{color.text.secondary}};"
    "  font-size: 10px; font-weight: bold; padding: 3px 6px;"
    "}"
    "QPushButton:hover {"
    "  background: {{color.background.2}}; color: {{color.text.primary}};"
    "}"
    "QPushButton:checked {"
    "  background: {{color.background.tx}}; color: {{color.meter.gainReduction}};"
    "  border: 1px solid {{color.meter.gainReduction}};"
    "}");

// Lookahead dropdown values in ms.  0 disables the delay line; 1 and
// 1.5 ms match Ableton's preset options; 3 / 5 added for users who
// want more headroom for very fast transients at the cost of latency.
const QList<float> kLookaheadOptions{ 0.0f, 1.0f, 1.5f, 3.0f, 5.0f };

} // namespace

StripGatePanel::StripGatePanel(AudioEngine* engine, QWidget* parent)
    : QWidget(parent)
    , m_audio(engine)
{
    setWindowTitle("Aetherial Gate");
    setStyleSheet(kWindowStyle);
    resize(kDefaultWidth, kDefaultHeight);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 0, 8, 8);
    root->setSpacing(6);

    auto* titleBar = new EditorFramelessTitleBar;
    m_titleBar = titleBar;
    root->addWidget(titleBar);

    // Bypass moved to the CHAIN widget's single-click gesture.

    // The page reads the way the EQ's does: a toolbar of switches along the
    // top, the display filling everything under it, and every knob in one row
    // at the foot. The old shape -- a column of knobs down the left with the
    // display beside it -- spent a third of a 562 px page on a 150 px column
    // and left a hole under it that nothing could fill.
    auto* toolbar = new QHBoxLayout;
    toolbar->setSpacing(8);

    auto* body = new QHBoxLayout;
    body->setSpacing(12);

    // Knobs, all of them, in one row at the foot.
    auto* left = new QHBoxLayout;
    left->setSpacing(4);

    // Threshold — the single largest control, matches Ableton's big
    // top-left knob.  -80..0 dB linear.
    m_threshold = new ClientCompKnob;
    m_threshold->setLabel("Thresh");
    m_threshold->setCenterLabelMode(true);
    m_threshold->setRange(-80.0f, 0.0f);
    m_threshold->setDefault(-40.0f);
    m_threshold->setValueFromNorm([](float n) { return -80.0f + n * 80.0f; });
    m_threshold->setNormFromValue([](float v) { return (v + 80.0f) / 80.0f; });
    m_threshold->setLabelFormat([](float v) {
        return QString::number(v, 'f', 1) + " dB";
    });
    m_threshold->setFixedSize(76, 76);
    connect(m_threshold, &ClientCompKnob::valueChanged,
            this, &StripGatePanel::applyThreshold);
    left->addWidget(m_threshold, 0, Qt::AlignHCenter);

    // Return — hysteresis, 0..20 dB linear.
    m_returnKnob = new ClientCompKnob;
    m_returnKnob->setLabel("Return");
    m_returnKnob->setCenterLabelMode(true);
    m_returnKnob->setRange(0.0f, 20.0f);
    m_returnKnob->setDefault(2.0f);
    m_returnKnob->setValueFromNorm([](float n) { return n * 20.0f; });
    m_returnKnob->setNormFromValue([](float v) { return v / 20.0f; });
    m_returnKnob->setLabelFormat([](float v) {
        return QString::number(v, 'f', 2) + " dB";
    });
    m_returnKnob->setFixedSize(76, 76);
    connect(m_returnKnob, &ClientCompKnob::valueChanged,
            this, &StripGatePanel::applyReturn);
    left->addWidget(m_returnKnob, 0, Qt::AlignHCenter);

    // Level / Curve — a pair, not a toggle. Both states stay on screen, and
    // the lit one is the one being shown.
    {
        toolbar->addWidget(new QLabel("View:"));
        auto* group = new QButtonGroup(this);
        group->setExclusive(true);

        const auto makeViewBtn = [&](const QString& text, int page) {
            auto* b = new QPushButton(text);
            b->setObjectName(QStringLiteral("gateView") + text);
            b->setCheckable(true);
            AetherSDR::ThemeManager::instance().applyStyleSheet(b, kFlipStyle);
            b->setFixedHeight(22);
            group->addButton(b, page);
            toolbar->addWidget(b);
            return b;
        };
        m_viewLevelBtn = makeViewBtn(QStringLiteral("Level"), 0);
        m_viewCurveBtn = makeViewBtn(QStringLiteral("Curve"), 1);
        m_viewLevelBtn->setChecked(true);
        m_viewLevelBtn->setToolTip("Live level history.");
        m_viewCurveBtn->setToolTip("Static transfer curve.");

        connect(group, &QButtonGroup::idClicked, this, [this](int page) {
            if (m_viewStack) m_viewStack->setCurrentIndex(page);
        });
    }

    // Gate / Expander — the same treatment: hard gating or gentle downward
    // expansion, each named on its own button.
    {
        // Air between the pairs, and a word in front of each: four buttons in
        // an even row read as one set of four choices, when they are two
        // questions -- what the display shows, and how the gate behaves.
        toolbar->addSpacing(16);
        toolbar->addWidget(new QLabel("Mode:"));
        auto* group = new QButtonGroup(this);
        group->setExclusive(true);

        const auto makeModeBtn = [&](const QString& text) {
            auto* b = new QPushButton(text);
            b->setObjectName(QStringLiteral("gateMode") + text);
            b->setCheckable(true);
            AetherSDR::ThemeManager::instance().applyStyleSheet(b, kFlipStyle);
            b->setFixedHeight(22);
            group->addButton(b);
            toolbar->addWidget(b);
            return b;
        };
        m_gateBtn = makeModeBtn(QStringLiteral("Gate"));
        m_expanderBtn = makeModeBtn(QStringLiteral("Expander"));
        // What the old Flip tooltip said, split between the two buttons that
        // now do its job: switching snaps ratio and floor to that mode's preset
        // pair, and leaves every other knob where the operator put it.
        m_gateBtn->setToolTip(
            "Gate: hard gating below the threshold.\n"
            "Snaps ratio + floor to the gate pair; other knobs stay put.");
        m_expanderBtn->setToolTip(
            "Expander: gentle downward expansion below the threshold.\n"
            "Snaps ratio + floor to the expander pair; other knobs stay put.");

        connect(m_gateBtn, &QPushButton::clicked, this,
                [this]() { applyMode(true); });
        connect(m_expanderBtn, &QPushButton::clicked, this,
                [this]() { applyMode(false); });
    }

    // RN2 — the mic denoiser, which runs ahead of every chain stage so that
    // noise is suppressed before the gate, compressor or saturator can amplify
    // it.  It sat on the Tube panel, at the far end of the chain, which put the
    // switch as far as the window could get from the point it acts on.  TX
    // only; showForRx() hides it.  (#2813)
    {
        toolbar->addSpacing(16);
        m_rn2Btn = new QPushButton(QStringLiteral("RN2"));
        m_rn2Btn->setObjectName(QStringLiteral("gateRn2"));
        m_rn2Btn->setAccessibleName(tr("Microphone noise reduction"));
        m_rn2Btn->setCheckable(true);
        m_rn2Btn->setFixedHeight(22);
        m_rn2Btn->setVisible(false);  // flipped on by showForTx()
        m_rn2Btn->setToolTip(tr(
            "Toggle RNNoise neural denoiser on the mic input.  Runs before "
            "any DSP chain stage so noise is suppressed before it can be "
            "amplified by gate / compressor / saturator.  Voice modes only — "
            "digital modes (RADE, DAX, RTTY, FT8, FDV, CW) bypass this stage.  "
            "Saved per Channel Strip profile, and suppressed by the strip's "
            "BYPASS button alongside every other voice stage."));
        // Share the Mode pair's idiom so the toolbar's switches read alike.
        AetherSDR::ThemeManager::instance().applyStyleSheet(m_rn2Btn, kFlipStyle);
        connect(m_rn2Btn, &QPushButton::toggled, this, [this](bool on) {
            if (m_audio) m_audio->setRn2TxEnabled(on);
            // Direct setter call is the single source of truth — engine
            // emits rn2TxEnabledChanged for any cross-widget observer.
        });
        toolbar->addWidget(m_rn2Btn);
    }

    // Peek (lookahead) — a slider at the right-hand end of the toolbar rather
    // than a dropdown. Five stops, so it steps between them; the reading sits
    // beside it because a slider with no number cannot say "1.5 ms".
    {
        toolbar->addStretch(1);

        auto* lookLbl = new QLabel("Peek:");
        toolbar->addWidget(lookLbl);

        m_lookahead = new QSlider(Qt::Horizontal);
        m_lookahead->setObjectName(QStringLiteral("gatePeekSlider"));
        m_lookahead->setAccessibleName(QStringLiteral("Gate lookahead"));
        m_lookahead->setRange(0, int(kLookaheadOptions.size()) - 1);
        m_lookahead->setPageStep(1);
        // Elastic, not fixed: the toolbar is at its width budget on TX, where
        // RN2 joins the switches, and a fixed 120 px here pushed the Mode pair
        // past the edge and clipped "Expander".  The slider is the one item
        // that reads fine narrower, so it gives up the space — capped at 120
        // so RX, which has no RN2 button, still shows it at full length.
        m_lookahead->setMinimumWidth(80);
        m_lookahead->setMaximumWidth(120);
        m_lookahead->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        m_lookahead->setToolTip(
            "Lookahead: how far ahead of the threshold crossing the gate "
            "opens, so an attack is not clipped.");
        applyPrimarySliderStyle(m_lookahead);
        toolbar->addWidget(m_lookahead);

        m_lookaheadValue = new QLabel;
        m_lookaheadValue->setFixedWidth(42);
        m_lookaheadValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        toolbar->addWidget(m_lookaheadValue);

        const auto showValue = [this](int i) {
            if (!m_lookaheadValue || i < 0 || i >= kLookaheadOptions.size()) return;
            const float v = kLookaheadOptions[i];
            m_lookaheadValue->setText(v <= 0.0f
                ? QStringLiteral("Off")
                : QString::number(v, 'g', 2) + QStringLiteral(" ms"));
        };
        showValue(0);
        connect(m_lookahead, &QSlider::valueChanged, this, [this, showValue](int i) {
            if (i < 0 || i >= kLookaheadOptions.size()) return;
            showValue(i);
            applyLookahead(kLookaheadOptions[i]);
        });
    }

    // Stack the level history and the transfer curve so the toggle in
    // the left column can flip between them in place.
    m_viewStack = new QStackedWidget;
    m_levelView = new ClientGateLevelView;
    m_curveView = new ClientGateCurveWidget;
    m_viewStack->addWidget(m_levelView);   // index 0 = live history
    m_viewStack->addWidget(m_curveView);   // index 1 = transfer curve
    body->addWidget(m_viewStack, 1);

    // Bottom row: Attack, Hold, Release, Floor (small knobs).
    auto* bottom = new QHBoxLayout;
    bottom->setSpacing(8);

    auto makeBottomKnob = [](const QString& label) {
        auto* k = new ClientCompKnob;
        k->setLabel(label);
        k->setCenterLabelMode(true);
        k->setFixedSize(76, 76);
        return k;
    };

    // Attack: 0.1..100 ms exponential.

    // Hold: 0..500 ms linear.
    m_hold = makeBottomKnob("Hold");
    m_hold->setRange(0.0f, 500.0f);
    m_hold->setDefault(20.0f);
    m_hold->setValueFromNorm([](float n) { return n * 500.0f; });
    m_hold->setNormFromValue([](float v) { return v / 500.0f; });
    m_hold->setLabelFormat([](float v) {
        return QString::number(v, 'f', 1) + " ms";
    });
    connect(m_hold, &ClientCompKnob::valueChanged,
            this, &StripGatePanel::applyHold);
    bottom->addWidget(m_hold, 0, Qt::AlignHCenter);

    // Release: 5..2000 ms exponential.
    m_release = makeBottomKnob("Release");
    m_release->setRange(5.0f, 2000.0f);
    m_release->setDefault(100.0f);
    m_release->setValueFromNorm([](float n) {
        return 5.0f * std::pow(400.0f, n);           // 5 → 2000
    });
    m_release->setNormFromValue([](float v) {
        return std::log(std::max(5.0f, v) / 5.0f) / std::log(400.0f);
    });
    m_release->setLabelFormat([](float v) {
        return QString::number(v, 'f', v < 100.0f ? 1 : 0) + " ms";
    });
    connect(m_release, &ClientCompKnob::valueChanged,
            this, &StripGatePanel::applyRelease);
    bottom->addWidget(m_release, 0, Qt::AlignHCenter);

    // Floor: -80..0 dB linear.
    m_floor = makeBottomKnob("Floor");
    m_floor->setRange(-80.0f, 0.0f);
    m_floor->setDefault(-15.0f);
    m_floor->setValueFromNorm([](float n) { return -80.0f + n * 80.0f; });
    m_floor->setNormFromValue([](float v) { return (v + 80.0f) / 80.0f; });
    m_floor->setLabelFormat([](float v) {
        return QString::number(v, 'f', 1) + " dB";
    });
    connect(m_floor, &ClientCompKnob::valueChanged,
            this, &StripGatePanel::applyFloor);
    bottom->addWidget(m_floor, 0, Qt::AlignHCenter);

    // Ratio tucked in alongside — not in the vanilla Ableton layout,
    // but necessary because Ableton's Gate is pure-gate and we need
    // ratio for the expander half of the combined module.  1..10 —
    // capped below hard-gate ∞:1 so the knob has finer resolution
    // across the musically useful range.
    m_ratio = makeBottomKnob("Ratio");
    m_ratio->setRange(1.0f, 10.0f);
    m_ratio->setDefault(2.0f);
    m_ratio->setValueFromNorm([](float n) {
        return 1.0f + n * 9.0f;                       // linear 1..10
    });
    m_ratio->setNormFromValue([](float v) { return (v - 1.0f) / 9.0f; });
    m_ratio->setLabelFormat([](float v) {
        return QString::number(v, 'f', 1) + ":1";
    });
    connect(m_ratio, &ClientCompKnob::valueChanged,
            this, &StripGatePanel::applyRatio);
    bottom->addWidget(m_ratio, 0, Qt::AlignHCenter);

    // Thresh and Return lead the row: they are the two the operator sets
    // first, and they were the column this layout did away with.
    for (int i = bottom->count(); i > 0; --i) {
        left->addItem(bottom->takeAt(0));
    }

    root->addLayout(toolbar);
    root->addLayout(body, 1);
    root->addLayout(left);

    // Bind both views to the gate once so they start polling.
    if (m_audio && gate()) {
        m_levelView->setGate(gate());
        m_curveView->setGate(gate());
    }

    // Initial sync from the engine state.
    syncControlsFromEngine();

    // Continuous polling so changes in the docked applet knobs mirror
    // here live, and vice versa.  30 Hz is cheap — each knob setValue
    // is a short clamp + repaint when values differ.
    m_syncTimer = new QTimer(this);
    m_syncTimer->setInterval(kPanelTickMs);
    connect(m_syncTimer, &QTimer::timeout,
            this, &StripGatePanel::syncControlsFromEngine);
}

StripGatePanel::~StripGatePanel() = default;

ClientGate* StripGatePanel::gate() const
{
    if (!m_audio) return nullptr;
    return m_side == Side::Rx ? m_audio->clientGateRx()
                              : m_audio->clientGateTx();
}

void StripGatePanel::saveGateSettings() const
{
    if (!m_audio) return;
    if (m_side == Side::Rx) m_audio->saveClientGateRxSettings();
    else                    m_audio->saveClientGateSettings();
}

void StripGatePanel::showForTx()
{
    m_side = Side::Tx;
    if (gate()) {
        if (m_levelView) m_levelView->setGate(gate());
        if (m_curveView) m_curveView->setGate(gate());
    }
    if (m_rn2Btn) {
        m_rn2Btn->setVisible(true);
        if (m_audio) {
            QSignalBlocker block(m_rn2Btn);
            m_rn2Btn->setChecked(m_audio->rn2TxEnabled());
        }
    }
    const QString title = QString::fromUtf8("Aetherial Gate \xe2\x80\x94 TX");
    if (m_titleBar)
        static_cast<EditorFramelessTitleBar*>(m_titleBar)->setTitleText(title);
    setWindowTitle(title);
    syncControlsFromEngine();
    restoreGeometryFromSettings();
    show();
    raise();
    activateWindow();
    // Deliberately does not start the poll: showEvent does that, and only
    // when the widget is actually on screen. Starting it here ran it from
    // construction for a panel that was never shown — and a widget that has
    // never been shown never gets a hideEvent to stop it again (see
    // PanelTick.h).
}

void StripGatePanel::showForRx()
{
    m_side = Side::Rx;
    if (gate()) {
        if (m_levelView) m_levelView->setGate(gate());
        if (m_curveView) m_curveView->setGate(gate());
    }
    // RX has its own RN2 toggle elsewhere (AetherDspWidget /
    // ClientRxChainWidget).  Hide our copy.  (#2813)
    if (m_rn2Btn) m_rn2Btn->setVisible(false);
    const QString title = QString::fromUtf8("Aetherial Gate \xe2\x80\x94 RX");
    if (m_titleBar)
        static_cast<EditorFramelessTitleBar*>(m_titleBar)->setTitleText(title);
    setWindowTitle(title);
    syncControlsFromEngine();
    restoreGeometryFromSettings();
    show();
    raise();
    activateWindow();
    // Deliberately does not start the poll: showEvent does that, and only
    // when the widget is actually on screen. Starting it here ran it from
    // construction for a panel that was never shown — and a widget that has
    // never been shown never gets a hideEvent to stop it again (see
    // PanelTick.h).
}

void StripGatePanel::syncControlsFromEngine()
{
    if (!m_audio || !gate()) return;
    ClientGate* g = gate();

    m_restoring = true;

    {
        const bool hard = (g->mode() == ClientGate::Mode::Gate);
        QSignalBlocker bg(m_gateBtn);
        QSignalBlocker be(m_expanderBtn);
        m_gateBtn->setChecked(hard);
        m_expanderBtn->setChecked(!hard);
    }
    {
        QSignalBlocker b(m_threshold);  m_threshold->setValue(g->thresholdDb());
    }
    {
        QSignalBlocker b(m_returnKnob); m_returnKnob->setValue(g->returnDb());
    }
    {
        QSignalBlocker b(m_ratio);      m_ratio->setValue(g->ratio());
    }
    {
    }
    {
        QSignalBlocker b(m_hold);       m_hold->setValue(g->holdMs());
    }
    {
        QSignalBlocker b(m_release);    m_release->setValue(g->releaseMs());
    }
    {
        QSignalBlocker b(m_floor);      m_floor->setValue(g->floorDb());
    }
    {
        QSignalBlocker b(m_lookahead);
        const float la = g->lookaheadMs();
        int bestIdx = 0;
        float bestDiff = 1e9f;
        for (int i = 0; i < kLookaheadOptions.size(); ++i) {
            const float d = std::fabs(kLookaheadOptions[i] - la);
            if (d < bestDiff) { bestDiff = d; bestIdx = i; }
        }
        m_lookahead->setValue(bestIdx);
        if (m_lookaheadValue) {
            const float v = kLookaheadOptions[bestIdx];
            m_lookaheadValue->setText(v <= 0.0f
                ? QStringLiteral("Off")
                : QString::number(v, 'g', 2) + QStringLiteral(" ms"));
        }
    }

    m_restoring = false;
}

void StripGatePanel::applyThreshold(float db)
{
    if (m_restoring || !m_audio) return;
    gate()->setThresholdDb(db);
    saveGateSettings();
    if (m_levelView) m_levelView->update();
    if (m_curveView) m_curveView->update();
}

void StripGatePanel::applyReturn(float db)
{
    if (m_restoring || !m_audio) return;
    gate()->setReturnDb(db);
    saveGateSettings();
    if (m_levelView) m_levelView->update();
    if (m_curveView) m_curveView->update();
}

void StripGatePanel::applyRatio(float ratio)
{
    if (m_restoring || !m_audio) return;
    gate()->setRatio(ratio);
    saveGateSettings();
}


void StripGatePanel::applyHold(float ms)
{
    if (m_restoring || !m_audio) return;
    gate()->setHoldMs(ms);
    saveGateSettings();
}

void StripGatePanel::applyRelease(float ms)
{
    if (m_restoring || !m_audio) return;
    gate()->setReleaseMs(ms);
    saveGateSettings();
}

void StripGatePanel::applyFloor(float db)
{
    if (m_restoring || !m_audio) return;
    gate()->setFloorDb(db);
    saveGateSettings();
}

void StripGatePanel::applyLookahead(float ms)
{
    if (m_restoring || !m_audio) return;
    gate()->setLookaheadMs(ms);
    saveGateSettings();
}

void StripGatePanel::applyMode(int modeIdx)
{
    if (m_restoring || !m_audio) return;
    gate()->setMode(
        modeIdx == 1 ? ClientGate::Mode::Gate : ClientGate::Mode::Expander);
    saveGateSettings();
    // Mode snaps ratio + floor; re-sync so the knobs show the new values.
    syncControlsFromEngine();
}

// ── Geometry persistence ─────────────────────────────────────────

void StripGatePanel::saveGeometryToSettings()
{
    if (m_restoring) return;
    AppSettings::instance().setValue(
        "StripGatePanelGeometry", QString::fromLatin1(saveGeometry().toBase64()));
}

void StripGatePanel::restoreGeometryFromSettings()
{
    m_restoring = true;
    const QString b64 = AppSettings::instance()
        .value("StripGatePanelGeometry", "").toString();
    if (!b64.isEmpty()) {
        restoreGeometry(QByteArray::fromBase64(b64.toLatin1()));
    }
    m_restoring = false;
}

void StripGatePanel::closeEvent(QCloseEvent* ev)
{
    saveGeometryToSettings();
    QWidget::closeEvent(ev);
}

void StripGatePanel::moveEvent(QMoveEvent* ev)
{
    saveGeometryToSettings();
    QWidget::moveEvent(ev);
}

void StripGatePanel::resizeEvent(QResizeEvent* ev)
{
    saveGeometryToSettings();
    QWidget::resizeEvent(ev);
}

void StripGatePanel::showEvent(QShowEvent* ev)
{
    QWidget::showEvent(ev);
    if (m_syncTimer) m_syncTimer->start();
}

void StripGatePanel::hideEvent(QHideEvent* ev)
{
    saveGeometryToSettings();
    // Stacked behind another tab, or the window closed: stop reading the
    // engine for something nobody can see.
    if (m_syncTimer) m_syncTimer->stop();
    QWidget::hideEvent(ev);
}

} // namespace AetherSDR
