#include "StripReverbPanel.h"
#include "ClientCompKnob.h"
#include "EditorFramelessTitleBar.h"
#include "core/AppSettings.h"
#include "core/AudioEngine.h"
#include "core/ClientReverb.h"

#include <QCloseEvent>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QMoveEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>

namespace MasterSDR {

namespace {

constexpr const char* kWindowStyle =
    "QWidget { background: #08121d; color: #d7e7f2; }"
    "QLabel  { background: transparent; color: #8aa8c0; font-size: 11px; }";

// Visualisation panel between the title bar and the knob row.
// Draws three layered traces that respond to the reverb knobs:
//   - Cyan: dry initial sound (amplitude scales inversely with Mix)
//   - Yellow: first-order reflections (count + spacing track Size,
//             amplitude decays per reflection scaled by Damping)
//   - Magenta: reverberant tail (length = Decay, amplitude = Mix,
//             smoothness/decay-rate = Damping; whole thing shifts
//             right by PreDelay).
class GridBox : public QWidget {
public:
    explicit GridBox(QWidget* parent = nullptr) : QWidget(parent) {
        setFixedHeight(138);
    }
    void setSize(float v)        { m_size = v;        update(); }
    void setDecayS(float v)      { m_decayS = v;      update(); }
    void setDamping(float v)     { m_damping = v;     update(); }
    void setPreDelayMs(float v)  { m_preDelayMs = v;  update(); }
    void setMix(float v)         { m_mix = v;         update(); }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QRectF r = rect();
        p.fillRect(r, QColor("#0a1420"));

        // Background grid.
        const QColor gridColor("#1e3040");
        const QColor axisColor("#2a4458");
        p.setPen(QPen(gridColor, 1.0));
        for (float n : { 0.25f, 0.50f, 0.75f }) {
            const float x = r.left() + n * r.width();
            const float y = r.top()  + n * r.height();
            p.drawLine(QPointF(x, r.top()),    QPointF(x, r.bottom()));
            p.drawLine(QPointF(r.left(), y),   QPointF(r.right(), y));
        }
        p.setPen(QPen(axisColor, 1.0));
        const float cx = r.left() + 0.5f * r.width();
        const float cy = r.top()  + 0.5f * r.height();
        p.drawLine(QPointF(cx, r.top()),    QPointF(cx, r.bottom()));
        p.drawLine(QPointF(r.left(), cy),   QPointF(r.right(), cy));

        // ── Traces ──
        // All three layers share the same left start so they overlap,
        // mirroring the reference visualisation: a single wave packet
        // entering the room, with reflections and tail rising from the
        // same arrival point and extending further right.
        const float w = r.width();
        const float h = r.height();
        const float midY  = r.top() + h * 0.5f;
        const float maxAmp = h * 0.40f;
        const float xStart = w * 0.04f;

        // PreDelay shifts the wet onset to the right of the dry start
        // (0..100 ms → 0..12% of width).
        const float preX = (m_preDelayMs / 100.0f) * w * 0.12f;
        const float wetStart = xStart + preX;

        // Reverberant tail (magenta) — drawn first so the brighter cyan
        // and yellow read on top.  Exponentially decaying oscillation,
        // total length scales with Decay (0.3..5 s → 30%..95% of width).
        {
            const float decayNorm = std::clamp(
                (m_decayS - 0.3f) / (5.0f - 0.3f), 0.0f, 1.0f);
            const float endX = std::min(
                wetStart + (0.30f + 0.65f * decayNorm) * (w - wetStart - 4.0f),
                w - 2.0f);
            const float tailAmp = m_mix * maxAmp;
            const float decayK = 2.0f + 4.0f * m_damping;
            const float cycles = 4.0f + 6.0f * decayNorm;
            const float freq = cycles * 2.0f * float(M_PI) / (endX - wetStart);
            QPainterPath path;
            path.moveTo(wetStart, midY);
            for (float x = wetStart; x <= endX; x += 1.0f) {
                const float t = (x - wetStart) / (endX - wetStart);
                const float env = std::exp(-decayK * t);
                path.lineTo(x, midY + tailAmp * env * std::sin(freq * (x - wetStart)));
            }
            p.setPen(QPen(QColor("#c060e0"), 1.8));
            p.drawPath(path);
        }

        // First-order reflections (yellow) — overlap with the tail.
        // Number = a few short bursts; spacing controlled by Size, each
        // burst's amplitude attenuated by Damping.
        {
            const int   nRefl    = 6;
            // At Size=1.0 the 6 reflections still span out to ~90% of
            // the width — spacing tightened from 0.15 to 0.11 to fit the
            // extra reflection without spilling past the right edge.
            const float spacing  = (0.04f + m_size * 0.11f) * w;
            const float reflLen  = std::min(spacing * 0.85f, w * 0.10f);
            const float dampStep = 1.0f - m_damping * 0.45f;
            float amp = m_mix * maxAmp * 0.75f;
            p.setPen(QPen(QColor("#ffd070"), 1.6));
            for (int i = 0; i < nRefl; ++i) {
                const float startX = wetStart + i * spacing;
                const float endX   = std::min(startX + reflLen, w - 2.0f);
                if (startX >= w - 2.0f || amp < 1.0f) break;
                const float freq = 3.0f * 2.0f * float(M_PI) / (endX - startX);
                QPainterPath path;
                path.moveTo(startX, midY);
                for (float x = startX; x <= endX; x += 1.0f) {
                    const float t = x - startX;
                    path.lineTo(x, midY + amp * std::sin(freq * t));
                }
                p.drawPath(path);
                amp *= dampStep;
            }
        }

        // Dry initial sound (cyan) — drawn last so it reads on top.
        // 8-cycle pure sine across most of the width; constant amplitude
        // (no envelope), but the *colour* fades from full alpha at the
        // left to fully transparent at the right via a linear gradient
        // brush on the pen, so the trace dissolves into the reverb tail.
        // Amplitude scales inversely with Mix so increasing Mix dims the
        // dry while the wet rises.
        {
            const float dryEnd = xStart + w * 0.92f;
            const float dryAmp = (1.0f - m_mix * 0.5f) * maxAmp;
            constexpr float kCycles = 8.0f;
            const float freq = kCycles * 2.0f * float(M_PI) / (dryEnd - xStart);
            QPainterPath path;
            // Sub-pixel sampling for a smooth curve through every cycle.
            const float step = 0.5f;
            path.moveTo(xStart, midY);
            for (float x = xStart; x <= dryEnd; x += step) {
                const float t = x - xStart;
                path.lineTo(x, midY + dryAmp * std::sin(freq * t));
            }
            QLinearGradient grad(xStart, 0, dryEnd, 0);
            QColor cyan("#4db8d4");
            QColor cyanFade = cyan;
            cyanFade.setAlpha(0);
            grad.setColorAt(0.0, cyan);
            grad.setColorAt(1.0, cyanFade);
            p.setPen(QPen(QBrush(grad), 2.2));
            p.drawPath(path);
        }
    }

private:
    float m_size{0.5f};
    float m_decayS{1.2f};
    float m_damping{0.5f};
    float m_preDelayMs{20.0f};
    float m_mix{0.15f};
};

} // namespace

StripReverbPanel::StripReverbPanel(AudioEngine* engine, QWidget* parent)
    : QWidget(parent)
    , m_audio(engine)
{
    const QString title = QString::fromUtf8("Masterial FreeVerb \xe2\x80\x94 TX");
    setWindowTitle(title);
    setStyleSheet(kWindowStyle);
    resize(480, 180);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(16, 0, 16, 14);
    root->setSpacing(8);

    auto* titleBar = new EditorFramelessTitleBar;
    titleBar->setTitleText(title);
    root->addWidget(titleBar);

    // Grid panel matching the tube curve widget's visual style — sits
    // between the title bar and the knob row as a placeholder for any
    // future visualisation (decay tail, IR, etc.).
    auto* gridBox = new GridBox;
    m_viz = gridBox;
    root->addWidget(gridBox, 1);

    auto* row = new QHBoxLayout;
    row->setSpacing(12);
    row->addStretch();

    auto makeKnob = [](const QString& label) {
        auto* k = new ClientCompKnob;
        k->setLabel(label);
        k->setCenterLabelMode(true);
        k->setFixedSize(76, 76);
        return k;
    };

    m_size = makeKnob("Size");
    m_size->setRange(0.0f, 1.0f);
    m_size->setDefault(0.5f);
    m_size->setValueFromNorm([](float n) { return n; });
    m_size->setNormFromValue([](float v) { return v; });
    m_size->setLabelFormat([](float v) {
        return QString::number(static_cast<int>(v * 100.0f + 0.5f)) + " %";
    });
    row->addWidget(m_size);

    m_decay = makeKnob("Decay");
    m_decay->setRange(0.3f, 5.0f);
    m_decay->setDefault(1.2f);
    m_decay->setValueFromNorm([](float n) {
        return 0.3f * std::pow(5.0f / 0.3f, n);
    });
    m_decay->setNormFromValue([](float v) {
        if (v <= 0.3f) return 0.0f;
        return std::log(v / 0.3f) / std::log(5.0f / 0.3f);
    });
    m_decay->setLabelFormat([](float v) {
        return QString::number(v, 'f', 2) + " s";
    });
    row->addWidget(m_decay);

    m_damping = makeKnob("Damp");
    m_damping->setRange(0.0f, 1.0f);
    m_damping->setDefault(0.5f);
    m_damping->setValueFromNorm([](float n) { return n; });
    m_damping->setNormFromValue([](float v) { return v; });
    m_damping->setLabelFormat([](float v) {
        return QString::number(static_cast<int>(v * 100.0f + 0.5f)) + " %";
    });
    row->addWidget(m_damping);

    m_preDly = makeKnob("PreDly");
    m_preDly->setRange(0.0f, 100.0f);
    m_preDly->setDefault(20.0f);
    m_preDly->setValueFromNorm([](float n) { return n * 100.0f; });
    m_preDly->setNormFromValue([](float v) { return v / 100.0f; });
    m_preDly->setLabelFormat([](float v) {
        return QString::number(v, 'f', 0) + " ms";
    });
    row->addWidget(m_preDly);

    m_mix = makeKnob("Mix");
    m_mix->setRange(0.0f, 1.0f);
    m_mix->setDefault(0.15f);
    m_mix->setValueFromNorm([](float n) { return n; });
    m_mix->setNormFromValue([](float v) { return v; });
    m_mix->setLabelFormat([](float v) {
        return QString::number(static_cast<int>(v * 100.0f + 0.5f)) + " %";
    });
    row->addWidget(m_mix);

    row->addStretch();
    root->addLayout(row);

    auto wire = [this, gridBox](ClientCompKnob* k, auto setter, auto vizSetter) {
        connect(k, &ClientCompKnob::valueChanged, this,
                [this, setter, vizSetter, gridBox](float v) {
            if (m_audio && m_audio->clientReverbTx()) {
                (m_audio->clientReverbTx()->*setter)(v);
                m_audio->saveClientReverbSettings();
            }
            (gridBox->*vizSetter)(v);
        });
    };
    wire(m_size,    &ClientReverb::setSize,        &GridBox::setSize);
    wire(m_decay,   &ClientReverb::setDecayS,      &GridBox::setDecayS);
    wire(m_damping, &ClientReverb::setDamping,     &GridBox::setDamping);
    wire(m_preDly,  &ClientReverb::setPreDelayMs,  &GridBox::setPreDelayMs);
    wire(m_mix,     &ClientReverb::setMix,         &GridBox::setMix);

    // Seed the viz with current engine values so the traces match
    // the knob positions at first paint.
    if (m_audio && m_audio->clientReverbTx()) {
        auto* rev = m_audio->clientReverbTx();
        gridBox->setSize       (rev->size());
        gridBox->setDecayS     (rev->decayS());
        gridBox->setDamping    (rev->damping());
        gridBox->setPreDelayMs (rev->preDelayMs());
        gridBox->setMix        (rev->mix());
    }

    syncControlsFromEngine();

    // Listen to the engine's reverb-state signal — fires whenever any
    // path (this panel, the docked applet, the floating editor, a
    // preset apply) writes new reverb values.  Replaces the 30 Hz
    // polling that used to keep this UI in sync.
    if (m_audio) {
        connect(m_audio, &AudioEngine::clientReverbStateChanged,
                this, &StripReverbPanel::syncControlsFromEngine);
    }
}

StripReverbPanel::~StripReverbPanel() = default;

void StripReverbPanel::showForTx()
{
    syncControlsFromEngine();
    restoreGeometryFromSettings();
    show();
    raise();
    activateWindow();
}

void StripReverbPanel::syncControlsFromEngine()
{
    if (!m_audio || !m_audio->clientReverbTx()) return;
    ClientReverb* r = m_audio->clientReverbTx();
    m_restoring = true;
    if (m_size)    { QSignalBlocker b(m_size);    m_size->setValue(r->size()); }
    if (m_decay)   { QSignalBlocker b(m_decay);   m_decay->setValue(r->decayS()); }
    if (m_damping) { QSignalBlocker b(m_damping); m_damping->setValue(r->damping()); }
    if (m_preDly)  { QSignalBlocker b(m_preDly);  m_preDly->setValue(r->preDelayMs()); }
    if (m_mix)     { QSignalBlocker b(m_mix);     m_mix->setValue(r->mix()); }
    // Push to viz too — knob valueChanged is suppressed by the
    // QSignalBlockers above so the wire lambdas don't fire during
    // external sync (e.g. when the docked applet moves the engine).
    if (auto* viz = dynamic_cast<GridBox*>(m_viz)) {
        viz->setSize       (r->size());
        viz->setDecayS     (r->decayS());
        viz->setDamping    (r->damping());
        viz->setPreDelayMs (r->preDelayMs());
        viz->setMix        (r->mix());
    }
    m_restoring = false;
}

void StripReverbPanel::saveGeometryToSettings()
{
    auto& s = AppSettings::instance();
    s.setValue("StripReverbPanelGeometry", saveGeometry().toBase64());
}

void StripReverbPanel::restoreGeometryFromSettings()
{
    auto& s = AppSettings::instance();
    const QByteArray geom = QByteArray::fromBase64(
        s.value("StripReverbPanelGeometry", "").toByteArray());
    if (!geom.isEmpty()) {
        m_restoring = true;
        restoreGeometry(geom);
        m_restoring = false;
    }
}

void StripReverbPanel::closeEvent(QCloseEvent* ev)
{
    saveGeometryToSettings();
    QWidget::closeEvent(ev);
}

void StripReverbPanel::moveEvent(QMoveEvent* ev)
{
    QWidget::moveEvent(ev);
    if (!m_restoring) saveGeometryToSettings();
}

void StripReverbPanel::resizeEvent(QResizeEvent* ev)
{
    QWidget::resizeEvent(ev);
    if (!m_restoring) saveGeometryToSettings();
}

void StripReverbPanel::showEvent(QShowEvent* ev)
{
    QWidget::showEvent(ev);
}

void StripReverbPanel::hideEvent(QHideEvent* ev)
{
    QWidget::hideEvent(ev);
}

} // namespace MasterSDR
