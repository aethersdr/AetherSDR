#include "ClientGateEditor.h"
#include "ClientCompKnob.h"
#include "ClientGateLevelView.h"
#include "EditorFramelessTitleBar.h"
#include "core/AppSettings.h"
#include "core/AudioEngine.h"
#include "core/ClientGate.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QLabel>
#include <QMoveEvent>
#include <QPushButton>
#include <QResizeEvent>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>

namespace MasterSDR {

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
const QString kFlipStyle = QStringLiteral(
    "QPushButton {"
    "  background: #006040; border: 1px solid #00a060; border-radius: 3px;"
    "  color: #00ff88; font-size: 10px; font-weight: bold; padding: 3px 6px;"
    "}"
    "QPushButton:hover { background: #007050; }"
    "QPushButton:checked {"
    "  background: #3a2a0e; color: #f2c14e; border: 1px solid #f2c14e;"
    "}"
    "QPushButton:checked:hover { background: #4a3a1e; }");

// Lookahead dropdown values in ms.  0 disables the delay line; 1 and
// 1.5 ms match Ableton's preset options; 3 / 5 added for users who
// want more headroom for very fast transients at the cost of latency.
const QList<float> kLookaheadOptions{ 0.0f, 1.0f, 1.5f, 3.0f, 5.0f };

} // namespace

ClientGateEditor::ClientGateEditor(AudioEngine* engine, QWidget* parent)
    : QWidget(parent, Qt::Window | Qt::FramelessWindowHint)
    , m_audio(engine)
{
    setWindowTitle("Masterial Gate");
    setStyleSheet(kWindowStyle);
    resize(kDefaultWidth, kDefaultHeight);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 0, 8, 8);
    root->setSpacing(6);

    auto* titleBar = new EditorFramelessTitleBar;
    m_titleBar = titleBar;
    root->addWidget(titleBar);

    // Bypass moved to the CHAIN widget's single-click gesture.

    // ── Main body: left control column + right level view ─────────
    auto* body = new QHBoxLayout;
    body->setSpacing(12);

    // Left column: threshold, return, flip/lookahead
    auto* left = new QVBoxLayout;
    left->setSpacing(8);

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
            this, &ClientGateEditor::applyThreshold);
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
            this, &ClientGateEditor::applyReturn);
    left->addWidget(m_returnKnob, 0, Qt::AlignHCenter);

    // Spacer pushes Peek + Flip to the bottom of the column so the
    // Threshold/Return knobs stay anchored at the top while the mode
    // toggle and lookahead picker hug the bottom edge.
    left->addStretch();

    // Peek (lookahead) row — sits directly above the Flip button.
    {
        auto* lookWrap = new QHBoxLayout;
        lookWrap->setSpacing(4);
        auto* lookLbl = new QLabel("Peek:");
        lookWrap->addWidget(lookLbl);
        m_lookahead = new QComboBox;
        m_lookahead->setStyleSheet(
            "QComboBox { padding-left: 0px; padding-right: 0px; }"
            "QComboBox::drop-down { width: 14px; }");
        for (float v : kLookaheadOptions) {
            const QString label = (v <= 0.0f)
                ? "Off" : (QString::number(v, 'g', 2) + " ms");
            m_lookahead->addItem(label, v);
        }
        connect(m_lookahead,
                QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int i) {
            if (i < 0 || i >= kLookaheadOptions.size()) return;
            applyLookahead(kLookaheadOptions[i]);
        });
        lookWrap->addWidget(m_lookahead, 1);
        left->addLayout(lookWrap);
    }

    // Flip button (Expander ↔ Gate) — bottom-most control.
    m_flip = new QPushButton("Flip");
    m_flip->setCheckable(true);
    m_flip->setStyleSheet(kFlipStyle);
    m_flip->setFixedHeight(22);
    m_flip->setToolTip(
        "Flip between downward Expander (gentle) and Gate (hard) "
        "modes.  Snaps ratio + floor to preset pairs; other knobs "
        "stay where you left them.");
    connect(m_flip, &QPushButton::toggled, this, [this](bool checked) {
        applyMode(checked ? 1 : 0);
    });
    left->addWidget(m_flip);

    body->addLayout(left, 0);

    // Right side: level view + bottom knob row
    auto* right = new QVBoxLayout;
    right->setSpacing(8);

    m_levelView = new ClientGateLevelView;
    right->addWidget(m_levelView, 1);

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
    m_attack = makeBottomKnob("Attack");
    m_attack->setRange(0.1f, 100.0f);
    m_attack->setDefault(0.5f);
    m_attack->setValueFromNorm([](float n) {
        return 0.1f * std::pow(1000.0f, n);           // 0.1 → 100
    });
    m_attack->setNormFromValue([](float v) {
        return std::log(std::max(0.1f, v) / 0.1f) / std::log(1000.0f);
    });
    m_attack->setLabelFormat([](float v) {
        return QString::number(v, 'f', v < 10.0f ? 2 : 1) + " ms";
    });
    connect(m_attack, &ClientCompKnob::valueChanged,
            this, &ClientGateEditor::applyAttack);
    bottom->addWidget(m_attack, 0, Qt::AlignHCenter);

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
            this, &ClientGateEditor::applyHold);
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
            this, &ClientGateEditor::applyRelease);
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
            this, &ClientGateEditor::applyFloor);
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
            this, &ClientGateEditor::applyRatio);
    bottom->addWidget(m_ratio, 0, Qt::AlignHCenter);

    right->addLayout(bottom);

    body->addLayout(right, 1);

    root->addLayout(body);

    // Bind the level view to the gate once so it starts polling.
    if (m_audio && gate()) {
        m_levelView->setGate(gate());
    }

    // Initial sync from the engine state.
    syncControlsFromEngine();

    // Continuous polling so changes in the docked applet knobs mirror
    // here live, and vice versa.  30 Hz is cheap — each knob setValue
    // is a short clamp + repaint when values differ.
    m_syncTimer = new QTimer(this);
    m_syncTimer->setInterval(33);
    connect(m_syncTimer, &QTimer::timeout,
            this, &ClientGateEditor::syncControlsFromEngine);
}

ClientGateEditor::~ClientGateEditor() = default;

ClientGate* ClientGateEditor::gate() const
{
    if (!m_audio) return nullptr;
    return m_side == Side::Rx ? m_audio->clientGateRx()
                              : m_audio->clientGateTx();
}

void ClientGateEditor::saveGateSettings() const
{
    if (!m_audio) return;
    if (m_side == Side::Rx) m_audio->saveClientGateRxSettings();
    else                    m_audio->saveClientGateSettings();
}

void ClientGateEditor::showForTx()
{
    m_side = Side::Tx;
    if (m_levelView && gate()) m_levelView->setGate(gate());
    const QString title = QString::fromUtf8("Masterial Gate \xe2\x80\x94 TX");
    if (m_titleBar)
        static_cast<EditorFramelessTitleBar*>(m_titleBar)->setTitleText(title);
    setWindowTitle(title);
    syncControlsFromEngine();
    restoreGeometryFromSettings();
    show();
    raise();
    activateWindow();
    if (m_syncTimer) m_syncTimer->start();
}

void ClientGateEditor::showForRx()
{
    m_side = Side::Rx;
    if (m_levelView && gate()) m_levelView->setGate(gate());
    const QString title = QString::fromUtf8("Masterial Gate \xe2\x80\x94 RX");
    if (m_titleBar)
        static_cast<EditorFramelessTitleBar*>(m_titleBar)->setTitleText(title);
    setWindowTitle(title);
    syncControlsFromEngine();
    restoreGeometryFromSettings();
    show();
    raise();
    activateWindow();
    if (m_syncTimer) m_syncTimer->start();
}

void ClientGateEditor::syncControlsFromEngine()
{
    if (!m_audio || !gate()) return;
    ClientGate* g = gate();

    m_restoring = true;

    {
        QSignalBlocker b(m_flip);
        m_flip->setChecked(g->mode() == ClientGate::Mode::Gate);
        m_flip->setText(g->mode() == ClientGate::Mode::Gate ? "Gate" : "Expander");
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
        QSignalBlocker b(m_attack);     m_attack->setValue(g->attackMs());
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
        m_lookahead->setCurrentIndex(bestIdx);
    }

    m_restoring = false;
}

void ClientGateEditor::applyThreshold(float db)
{
    if (m_restoring || !m_audio) return;
    gate()->setThresholdDb(db);
    saveGateSettings();
    if (m_levelView) m_levelView->update();
}

void ClientGateEditor::applyReturn(float db)
{
    if (m_restoring || !m_audio) return;
    gate()->setReturnDb(db);
    saveGateSettings();
    if (m_levelView) m_levelView->update();
}

void ClientGateEditor::applyRatio(float ratio)
{
    if (m_restoring || !m_audio) return;
    gate()->setRatio(ratio);
    saveGateSettings();
}

void ClientGateEditor::applyAttack(float ms)
{
    if (m_restoring || !m_audio) return;
    gate()->setAttackMs(ms);
    saveGateSettings();
}

void ClientGateEditor::applyHold(float ms)
{
    if (m_restoring || !m_audio) return;
    gate()->setHoldMs(ms);
    saveGateSettings();
}

void ClientGateEditor::applyRelease(float ms)
{
    if (m_restoring || !m_audio) return;
    gate()->setReleaseMs(ms);
    saveGateSettings();
}

void ClientGateEditor::applyFloor(float db)
{
    if (m_restoring || !m_audio) return;
    gate()->setFloorDb(db);
    saveGateSettings();
}

void ClientGateEditor::applyLookahead(float ms)
{
    if (m_restoring || !m_audio) return;
    gate()->setLookaheadMs(ms);
    saveGateSettings();
}

void ClientGateEditor::applyMode(int modeIdx)
{
    if (m_restoring || !m_audio) return;
    gate()->setMode(
        modeIdx == 1 ? ClientGate::Mode::Gate : ClientGate::Mode::Expander);
    saveGateSettings();
    // Mode snaps ratio + floor; re-sync so the knobs show the new values.
    syncControlsFromEngine();
}

// ── Geometry persistence ─────────────────────────────────────────

void ClientGateEditor::saveGeometryToSettings()
{
    if (m_restoring) return;
    AppSettings::instance().setValue(
        "ClientGateEditorGeometry", QString::fromLatin1(saveGeometry().toBase64()));
}

void ClientGateEditor::restoreGeometryFromSettings()
{
    m_restoring = true;
    const QString b64 = AppSettings::instance()
        .value("ClientGateEditorGeometry", "").toString();
    if (!b64.isEmpty()) {
        restoreGeometry(QByteArray::fromBase64(b64.toLatin1()));
    }
    m_restoring = false;
}

void ClientGateEditor::closeEvent(QCloseEvent* ev)
{
    saveGeometryToSettings();
    QWidget::closeEvent(ev);
}

void ClientGateEditor::moveEvent(QMoveEvent* ev)
{
    saveGeometryToSettings();
    QWidget::moveEvent(ev);
}

void ClientGateEditor::resizeEvent(QResizeEvent* ev)
{
    saveGeometryToSettings();
    QWidget::resizeEvent(ev);
}

void ClientGateEditor::showEvent(QShowEvent* ev)
{
    QWidget::showEvent(ev);
}

void ClientGateEditor::hideEvent(QHideEvent* ev)
{
    saveGeometryToSettings();
    QWidget::hideEvent(ev);
}

} // namespace MasterSDR
