#include "ClientDeEssApplet.h"
#include "ClientCompKnob.h"
#include "ClientDeEssCurveWidget.h"
#include "MeterSmoother.h"
#include "core/AudioEngine.h"
#include "core/ClientDeEss.h"

#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QPushButton>
#include <QSignalBlocker>
#include <QPainter>
#include <QPaintEvent>
#include <QTimer>
#include <QElapsedTimer>
#include <algorithm>
#include <cmath>

namespace MasterSDR {

// GR mini-strip for the de-esser.  File-scope so the applet can hold
// a typed pointer.  Max reduction ≤ 24 dB so scale the full bar to
// that range.
class ClientDeEssGrBar : public QWidget {
public:
    explicit ClientDeEssGrBar(QWidget* parent = nullptr) : QWidget(parent)
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

    void setGrDb(float grDb)
    {
        if (std::fabs(grDb - m_grDb) < 0.05f) return;
        m_grDb = grDb;
        constexpr float kMaxGr = 24.0f;
        m_smooth.setTarget(std::clamp(-m_grDb, 0.0f, kMaxGr) / kMaxGr);
        if (!m_smooth.needsAnimation()) {
            if (m_animTimer.isActive()) m_animTimer.stop();
            update();
        } else if (!m_animTimer.isActive()) {
            m_animElapsed.restart();
            m_animTimer.start();
        }
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        const QRectF r = rect();
        p.fillRect(r, QColor("#0a1420"));
        constexpr float kMaxGr = 24.0f;
        const float frac = m_smooth.value();
        if (frac > 0.0f) {
            const float w = frac * r.width();
            QRectF fill(r.right() - w, r.top() + 1.0, w, r.height() - 2.0);
            p.fillRect(fill, QColor("#d47272"));   // soft red — matches curve
        }
        // -6 dB tick (typical amount)
        const float tickX = r.right() - (6.0f / kMaxGr) * r.width();
        p.setPen(QPen(QColor("#2a4458"), 1.0));
        p.drawLine(QPointF(tickX, r.top()), QPointF(tickX, r.bottom()));
    }

private:
    float         m_grDb{0.0f};
    MeterSmoother m_smooth;
    QTimer        m_animTimer;
    QElapsedTimer m_animElapsed;
};

namespace {

const QString kEnableStyle =
    "QPushButton {"
    "  background: #1a2a3a; border: 1px solid #205070; border-radius: 3px;"
    "  color: #c8d8e8; font-size: 11px; font-weight: bold; padding: 2px 10px;"
    "}"
    "QPushButton:hover { background: #204060; }"
    "QPushButton:checked {"
    "  background: #006040; color: #00ff88; border: 1px solid #00a060;"
    "}";

constexpr const char* kEditStyle =
    "QPushButton {"
    "  background: #1a2a3a; border: 1px solid #205070; border-radius: 3px;"
    "  color: #c8d8e8; font-size: 11px; padding: 2px 10px;"
    "}"
    "QPushButton:hover { background: #204060; }";

} // namespace

ClientDeEssApplet::ClientDeEssApplet(QWidget* parent) : QWidget(parent)
{
    buildUI();
    hide();
}

void ClientDeEssApplet::buildUI()
{
    setStyleSheet("QWidget { background: transparent; }");

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(4, 4, 4, 4);
    outer->setSpacing(4);

    m_curve = new ClientDeEssCurveWidget;
    m_curve->setCompactMode(true);
    m_curve->setMinimumHeight(90);
    outer->addWidget(m_curve, 1);

    m_grBar = new ClientDeEssGrBar;
    outer->addWidget(m_grBar);

    // Enable / Edit buttons removed — CHAIN widget handles bypass
    // (single-click) and editor-open (double-click).

    // Four-knob tuning row — Freq, Q, Thresh, Amount.  Mappings mirror
    // ClientDeEssEditor.
    {
        auto* row = new QHBoxLayout;
        row->setSpacing(4);

        auto makeKnob = [](const QString& label) {
            auto* k = new ClientCompKnob;
            k->setLabel(label);
            k->setCenterLabelMode(true);
            k->setInlineEditEnabled(false);
            k->setFixedSize(38, 48);
            return k;
        };

        m_freq = makeKnob("Freq");
        m_freq->setRange(1000.0f, 12000.0f);
        m_freq->setDefault(6000.0f);
        m_freq->setValueFromNorm([](float n) {
            return 1000.0f * std::pow(12.0f, n);
        });
        m_freq->setNormFromValue([](float v) {
            return std::log(std::max(1000.0f, v) / 1000.0f)
                   / std::log(12.0f);
        });
        m_freq->setLabelFormat([](float v) {
            return (v >= 1000.0f)
                ? QString::number(v / 1000.0f, 'f', 1) + " kHz"
                : QString::number(v, 'f', 0) + " Hz";
        });
        row->addWidget(m_freq);

        m_q = makeKnob("Q");
        m_q->setRange(0.5f, 5.0f);
        m_q->setDefault(2.0f);
        m_q->setValueFromNorm([](float n) { return 0.5f + n * 4.5f; });
        m_q->setNormFromValue([](float v) { return (v - 0.5f) / 4.5f; });
        m_q->setLabelFormat([](float v) {
            return QString::number(v, 'f', 2);
        });
        row->addWidget(m_q);

        m_thresh = makeKnob("Thresh");
        m_thresh->setRange(-60.0f, 0.0f);
        m_thresh->setDefault(-30.0f);
        m_thresh->setValueFromNorm([](float n) { return -60.0f + n * 60.0f; });
        m_thresh->setNormFromValue([](float v) { return (v + 60.0f) / 60.0f; });
        m_thresh->setLabelFormat([](float v) {
            return QString::number(v, 'f', 1) + " dB";
        });
        row->addWidget(m_thresh);

        m_amount = makeKnob("Amount");
        m_amount->setRange(-24.0f, 0.0f);
        m_amount->setDefault(-6.0f);
        m_amount->setValueFromNorm([](float n) { return -24.0f + n * 24.0f; });
        m_amount->setNormFromValue([](float v) { return (v + 24.0f) / 24.0f; });
        m_amount->setLabelFormat([](float v) {
            return QString::number(v, 'f', 1) + " dB";
        });
        row->addWidget(m_amount);

        outer->addLayout(row);
    }

    auto wire = [this](ClientCompKnob* k, auto setter) {
        connect(k, &ClientCompKnob::valueChanged, this, [this, setter](float v) {
            if (!m_audio || !m_audio->clientDeEssTx()) return;
            (m_audio->clientDeEssTx()->*setter)(v);
            m_audio->saveClientDeEssSettings();
        });
    };
    wire(m_freq,   &ClientDeEss::setFrequencyHz);
    wire(m_q,      &ClientDeEss::setQ);
    wire(m_thresh, &ClientDeEss::setThresholdDb);
    wire(m_amount, &ClientDeEss::setAmountDb);

    m_meterTimer = new QTimer(this);
    m_meterTimer->setInterval(33);
    connect(m_meterTimer, &QTimer::timeout, this, &ClientDeEssApplet::tickMeter);
}

void ClientDeEssApplet::setAudioEngine(AudioEngine* engine)
{
    m_audio = engine;
    if (!m_audio) return;
    m_curve->setDeEss(m_audio->clientDeEssTx());
    syncEnableFromEngine();
    m_meterTimer->start();
}

void ClientDeEssApplet::syncEnableFromEngine()
{
    if (m_curve) m_curve->update();

    // Bypass dim — render the whole tile at reduced opacity when the
    // stage is bypassed, matching the dim effect on the EQ curve.
    auto* dsp0 = (m_audio ? m_audio->clientDeEssTx() : nullptr);
    const bool dspEnabled = dsp0 ? dsp0->isEnabled() : true;
    auto* eff = qobject_cast<QGraphicsOpacityEffect*>(graphicsEffect());
    if (!eff) {
        eff = new QGraphicsOpacityEffect(this);
        setGraphicsEffect(eff);
    }
    eff->setOpacity(dspEnabled ? 1.0 : 0.55);

    if (!m_audio || !m_audio->clientDeEssTx()) return;
    ClientDeEss* d = m_audio->clientDeEssTx();
    if (m_freq)   { QSignalBlocker b(m_freq);   m_freq->setValue(d->frequencyHz()); }
    if (m_q)      { QSignalBlocker b(m_q);      m_q->setValue(d->q()); }
    if (m_thresh) { QSignalBlocker b(m_thresh); m_thresh->setValue(d->thresholdDb()); }
    if (m_amount) { QSignalBlocker b(m_amount); m_amount->setValue(d->amountDb()); }
}

void ClientDeEssApplet::refreshEnableFromEngine()
{
    syncEnableFromEngine();
}

void ClientDeEssApplet::onEnableToggled(bool on)
{
    if (!m_audio) return;
    ClientDeEss* d = m_audio->clientDeEssTx();
    if (!d) return;
    d->setEnabled(on);
    m_audio->saveClientDeEssSettings();
    if (m_curve) m_curve->update();
}

void ClientDeEssApplet::tickMeter()
{
    if (!m_audio || !m_grBar) return;
    ClientDeEss* d = m_audio->clientDeEssTx();
    if (!d) return;
    const float gr = d->gainReductionDb();
    m_grBar->setGrDb(gr);
    m_grDb = gr;

    if (m_freq)   { QSignalBlocker b(m_freq);   m_freq->setValue(d->frequencyHz()); }
    if (m_q)      { QSignalBlocker b(m_q);      m_q->setValue(d->q()); }
    if (m_thresh) { QSignalBlocker b(m_thresh); m_thresh->setValue(d->thresholdDb()); }
    if (m_amount) { QSignalBlocker b(m_amount); m_amount->setValue(d->amountDb()); }
}

} // namespace MasterSDR
