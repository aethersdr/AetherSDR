#include "StripDeEssPanel.h"
#include "ClientCompKnob.h"
#include "ClientDeEssCurveWidget.h"
#include "EditorFramelessTitleBar.h"
#include "MeterSmoother.h"
#include "core/AppSettings.h"
#include "core/AudioEngine.h"
#include "core/ClientDeEss.h"

#include <QCloseEvent>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QMoveEvent>
#include <QElapsedTimer>
#include <QPainter>
#include <QPaintEvent>
#include <QPushButton>
#include <QResizeEvent>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>

namespace MasterSDR {

namespace {

constexpr int kDefaultWidth  = 720;
constexpr int kDefaultHeight = 340;
constexpr float kGrMaxDb    = 24.0f;   // bar full-scale (right-anchored)

constexpr const char* kWindowStyle =
    "QWidget { background: #08121d; color: #d7e7f2; }"
    "QLabel  { background: transparent; color: #8aa8c0; font-size: 11px; }";

// Gain-reduction bar mirroring ClientDeEssApplet's m_grBar — dark bg,
// soft-red fill anchored on the right, -6 dB tick.  Uses the shared
// MeterSmoother (30 ms attack, 180 ms release, polled at ~120 Hz) so
// the motion matches every other meter in the app.
class DeEssGrBar : public QWidget {
public:
    explicit DeEssGrBar(QWidget* parent = nullptr) : QWidget(parent)
    {
        setFixedHeight(10);
        m_animTimer.setTimerType(Qt::PreciseTimer);
        m_animTimer.setInterval(kMeterSmootherIntervalMs);
        connect(&m_animTimer, &QTimer::timeout, this, [this]() {
            if (!m_smooth.tick(m_animElapsed.restart()))
                m_animTimer.stop();
            update();
        });
    }
    void setGrDb(float grDb) {
        if (std::fabs(grDb - m_grDb) < 0.05f) return;
        m_grDb = grDb;
        m_smooth.setTarget(std::clamp(-grDb, 0.0f, kGrMaxDb) / kGrMaxDb);
        if (!m_smooth.needsAnimation()) {
            if (m_animTimer.isActive()) m_animTimer.stop();
            update();
        } else if (!m_animTimer.isActive()) {
            m_animElapsed.restart();
            m_animTimer.start();
        }
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        const QRectF r = rect();
        p.fillRect(r, QColor("#0a1420"));
        const float frac = m_smooth.value();
        if (frac > 0.0f) {
            const float w = frac * r.width();
            QRectF fill(r.right() - w, r.top() + 1.0, w, r.height() - 2.0);
            p.fillRect(fill, QColor("#d47272"));   // soft red
        }
        // -6 dB tick (typical Amount setting).
        const float tickX = r.right() - (6.0f / kGrMaxDb) * r.width();
        p.setPen(QPen(QColor("#2a4458"), 1.0));
        p.drawLine(QPointF(tickX, r.top()), QPointF(tickX, r.bottom()));
    }
private:
    float         m_grDb{0.0f};
    MeterSmoother m_smooth;
    QTimer        m_animTimer;
    QElapsedTimer m_animElapsed;
};

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

} // namespace

StripDeEssPanel::StripDeEssPanel(AudioEngine* engine, QWidget* parent)
    : QWidget(parent)
    , m_audio(engine)
{
    const QString title = QString::fromUtf8("Masterial De-Esser \xe2\x80\x94 TX");
    setWindowTitle(title);
    setStyleSheet(kWindowStyle);
    resize(kDefaultWidth, kDefaultHeight);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 0, 8, 8);
    root->setSpacing(6);

    auto* titleBar = new EditorFramelessTitleBar;
    titleBar->setTitleText(title);
    m_titleBar = titleBar;
    root->addWidget(titleBar);

    // Bypass moved to the CHAIN widget's single-click gesture.

    // Body: 3-column — left knobs | big curve | right knobs
    auto* body = new QHBoxLayout;
    body->setSpacing(12);

    // Left column: Frequency / Q / Threshold
    auto* left = new QVBoxLayout;
    left->setSpacing(10);

    m_freq = new ClientCompKnob;
    m_freq->setLabel("Freq");
    m_freq->setCenterLabelMode(true);
    m_freq->setRange(1000.0f, 12000.0f);
    m_freq->setDefault(6000.0f);
    // Log sweep across 1..12 kHz so the knob resolution matches how
    // ears perceive frequency (doubles are equidistant).
    m_freq->setValueFromNorm([](float n) {
        return 1000.0f * std::pow(12.0f, n);       // 1 → 12 kHz
    });
    m_freq->setNormFromValue([](float v) {
        return std::log(std::max(1000.0f, v) / 1000.0f)
               / std::log(12.0f);
    });
    m_freq->setLabelFormat([](float v) {
        return (v >= 1000.0f)
            ? QString::number(v / 1000.0f, 'f', 2) + " kHz"
            : QString::number(v, 'f', 0) + " Hz";
    });
    m_freq->setFixedSize(76, 76);
    connect(m_freq, &ClientCompKnob::valueChanged,
            this, &StripDeEssPanel::applyFrequency);
    left->addWidget(m_freq, 0, Qt::AlignHCenter);

    m_q = new ClientCompKnob;
    m_q->setLabel("Q");
    m_q->setCenterLabelMode(true);
    m_q->setRange(0.5f, 5.0f);
    m_q->setDefault(2.0f);
    m_q->setValueFromNorm([](float n) { return 0.5f + n * 4.5f; });
    m_q->setNormFromValue([](float v) { return (v - 0.5f) / 4.5f; });
    m_q->setLabelFormat([](float v) {
        return QString::number(v, 'f', 2);
    });
    m_q->setFixedSize(76, 76);
    connect(m_q, &ClientCompKnob::valueChanged,
            this, &StripDeEssPanel::applyQ);
    left->addWidget(m_q, 0, Qt::AlignHCenter);

    m_threshold = new ClientCompKnob;
    m_threshold->setLabel("Threshold");
    m_threshold->setCenterLabelMode(true);
    m_threshold->setRange(-60.0f, 0.0f);
    m_threshold->setDefault(-30.0f);
    m_threshold->setValueFromNorm([](float n) { return -60.0f + n * 60.0f; });
    m_threshold->setNormFromValue([](float v) { return (v + 60.0f) / 60.0f; });
    m_threshold->setLabelFormat([](float v) {
        return QString::number(v, 'f', 1) + " dB";
    });
    m_threshold->setFixedSize(76, 76);
    connect(m_threshold, &ClientCompKnob::valueChanged,
            this, &StripDeEssPanel::applyThreshold);
    left->addWidget(m_threshold, 0, Qt::AlignHCenter);

    left->addStretch();
    body->addLayout(left, 0);

    // Center: sidechain bandpass response curve (bigger than the
    // applet's compact version — shows labels, threshold line, ball).
    // Curve on top, GR bar below — same visual family as the docked
    // applet's gain-reduction strip.  Curve height drops from 200 to
    // ~140 to make room for the bar without growing the panel.
    auto* centre = new QVBoxLayout;
    centre->setContentsMargins(0, 0, 0, 0);
    centre->setSpacing(4);
    m_curve = new ClientDeEssCurveWidget;
    m_curve->setCompactMode(false);
    m_curve->setMinimumHeight(140);
    centre->addWidget(m_curve, 1);
    auto* grBar = new DeEssGrBar;
    m_grBar = grBar;
    centre->addWidget(grBar);
    body->addLayout(centre, 1);

    // Right column: Amount / Attack / Release
    auto* right = new QVBoxLayout;
    right->setSpacing(10);

    m_amount = new ClientCompKnob;
    m_amount->setLabel("Amount");
    m_amount->setCenterLabelMode(true);
    m_amount->setRange(-24.0f, 0.0f);
    m_amount->setDefault(-6.0f);
    m_amount->setValueFromNorm([](float n) { return -24.0f + n * 24.0f; });
    m_amount->setNormFromValue([](float v) { return (v + 24.0f) / 24.0f; });
    m_amount->setLabelFormat([](float v) {
        return QString::number(v, 'f', 1) + " dB";
    });
    m_amount->setFixedSize(76, 76);
    connect(m_amount, &ClientCompKnob::valueChanged,
            this, &StripDeEssPanel::applyAmount);
    right->addWidget(m_amount, 0, Qt::AlignHCenter);

    m_attack = new ClientCompKnob;
    m_attack->setLabel("Attack");
    m_attack->setCenterLabelMode(true);
    m_attack->setRange(0.1f, 30.0f);
    m_attack->setDefault(1.0f);
    m_attack->setValueFromNorm([](float n) {
        return 0.1f * std::pow(300.0f, n);         // 0.1..30 ms
    });
    m_attack->setNormFromValue([](float v) {
        return std::log(std::max(0.1f, v) / 0.1f) / std::log(300.0f);
    });
    m_attack->setLabelFormat([](float v) {
        return QString::number(v, 'f', v < 10.0f ? 2 : 1) + " ms";
    });
    m_attack->setFixedSize(76, 76);
    connect(m_attack, &ClientCompKnob::valueChanged,
            this, &StripDeEssPanel::applyAttack);
    right->addWidget(m_attack, 0, Qt::AlignHCenter);

    m_release = new ClientCompKnob;
    m_release->setLabel("Release");
    m_release->setCenterLabelMode(true);
    m_release->setRange(10.0f, 500.0f);
    m_release->setDefault(100.0f);
    m_release->setValueFromNorm([](float n) {
        return 10.0f * std::pow(50.0f, n);         // 10..500 ms
    });
    m_release->setNormFromValue([](float v) {
        return std::log(std::max(10.0f, v) / 10.0f) / std::log(50.0f);
    });
    m_release->setLabelFormat([](float v) {
        return QString::number(v, 'f', v < 100.0f ? 1 : 0) + " ms";
    });
    m_release->setFixedSize(76, 76);
    connect(m_release, &ClientCompKnob::valueChanged,
            this, &StripDeEssPanel::applyRelease);
    right->addWidget(m_release, 0, Qt::AlignHCenter);

    right->addStretch();
    body->addLayout(right, 0);

    root->addLayout(body);

    // Bind the curve to the de-esser so it starts polling.
    if (m_audio && deEss()) {
        m_curve->setDeEss(deEss());
    }

    syncControlsFromEngine();

    m_syncTimer = new QTimer(this);
    m_syncTimer->setInterval(33);
    connect(m_syncTimer, &QTimer::timeout,
            this, &StripDeEssPanel::syncControlsFromEngine);
}

StripDeEssPanel::~StripDeEssPanel() = default;

void StripDeEssPanel::showForTx()
{
    m_side = Side::Tx;
    const QString title = QString::fromUtf8("Masterial De-Esser \xe2\x80\x94 TX");
    setWindowTitle(title);
    if (m_titleBar)
        static_cast<EditorFramelessTitleBar*>(m_titleBar)->setTitleText(title);
    if (m_curve && deEss()) m_curve->setDeEss(deEss());
    syncControlsFromEngine();
    restoreGeometryFromSettings();
    show();
    raise();
    activateWindow();
    if (m_syncTimer) m_syncTimer->start();
}

void StripDeEssPanel::showForRx()
{
    m_side = Side::Rx;
    const QString title = QString::fromUtf8("Masterial De-Esser \xe2\x80\x94 RX");
    setWindowTitle(title);
    if (m_titleBar)
        static_cast<EditorFramelessTitleBar*>(m_titleBar)->setTitleText(title);
    if (m_curve && deEss()) m_curve->setDeEss(deEss());
    syncControlsFromEngine();
    restoreGeometryFromSettings();
    show();
    raise();
    activateWindow();
    if (m_syncTimer) m_syncTimer->start();
}

ClientDeEss* StripDeEssPanel::deEss() const
{
    if (!m_audio) return nullptr;
    return m_side == Side::Rx ? m_audio->clientDeEssRx()
                              : m_audio->clientDeEssTx();
}

void StripDeEssPanel::saveDeEssSettings() const
{
    if (!m_audio) return;
    if (m_side == Side::Rx) m_audio->saveClientDeEssRxSettings();
    else                    m_audio->saveClientDeEssSettings();
}

void StripDeEssPanel::syncControlsFromEngine()
{
    if (!m_audio || !deEss()) return;
    ClientDeEss* d = deEss();
    m_restoring = true;

    { QSignalBlocker b(m_freq);      m_freq->setValue(d->frequencyHz()); }
    { QSignalBlocker b(m_q);         m_q->setValue(d->q()); }
    { QSignalBlocker b(m_threshold); m_threshold->setValue(d->thresholdDb()); }
    { QSignalBlocker b(m_amount);    m_amount->setValue(d->amountDb()); }
    { QSignalBlocker b(m_attack);    m_attack->setValue(d->attackMs()); }
    { QSignalBlocker b(m_release);   m_release->setValue(d->releaseMs()); }

    if (auto* bar = static_cast<DeEssGrBar*>(m_grBar))
        bar->setGrDb(d->gainReductionDb());

    m_restoring = false;
}

void StripDeEssPanel::applyFrequency(float hz)
{
    if (m_restoring || !m_audio) return;
    deEss()->setFrequencyHz(hz);
    saveDeEssSettings();
    if (m_curve) m_curve->update();
}
void StripDeEssPanel::applyQ(float q)
{
    if (m_restoring || !m_audio) return;
    deEss()->setQ(q);
    saveDeEssSettings();
    if (m_curve) m_curve->update();
}
void StripDeEssPanel::applyThreshold(float db)
{
    if (m_restoring || !m_audio) return;
    deEss()->setThresholdDb(db);
    saveDeEssSettings();
    if (m_curve) m_curve->update();
}
void StripDeEssPanel::applyAmount(float db)
{
    if (m_restoring || !m_audio) return;
    deEss()->setAmountDb(db);
    saveDeEssSettings();
}
void StripDeEssPanel::applyAttack(float ms)
{
    if (m_restoring || !m_audio) return;
    deEss()->setAttackMs(ms);
    saveDeEssSettings();
}
void StripDeEssPanel::applyRelease(float ms)
{
    if (m_restoring || !m_audio) return;
    deEss()->setReleaseMs(ms);
    saveDeEssSettings();
}

void StripDeEssPanel::saveGeometryToSettings()
{
    if (m_restoring) return;
    AppSettings::instance().setValue(
        "StripDeEssPanelGeometry",
        QString::fromLatin1(saveGeometry().toBase64()));
}

void StripDeEssPanel::restoreGeometryFromSettings()
{
    m_restoring = true;
    const QString b64 = AppSettings::instance()
        .value("StripDeEssPanelGeometry", "").toString();
    if (!b64.isEmpty()) {
        restoreGeometry(QByteArray::fromBase64(b64.toLatin1()));
    }
    m_restoring = false;
}

void StripDeEssPanel::closeEvent(QCloseEvent* ev)
{ saveGeometryToSettings(); QWidget::closeEvent(ev); }
void StripDeEssPanel::moveEvent(QMoveEvent* ev)
{ saveGeometryToSettings(); QWidget::moveEvent(ev); }
void StripDeEssPanel::resizeEvent(QResizeEvent* ev)
{ saveGeometryToSettings(); QWidget::resizeEvent(ev); }
void StripDeEssPanel::showEvent(QShowEvent* ev)
{ QWidget::showEvent(ev); }
void StripDeEssPanel::hideEvent(QHideEvent* ev)
{ saveGeometryToSettings(); QWidget::hideEvent(ev); }

} // namespace MasterSDR
