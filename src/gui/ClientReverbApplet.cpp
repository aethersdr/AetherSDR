#include "ClientReverbApplet.h"
#include "ClientCompKnob.h"
#include "core/AudioEngine.h"
#include "core/ClientReverb.h"

#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace MasterSDR {

namespace {

// Small live visualisation matching the strip-side reverb panel:
// cyan dry sine packet, yellow first-order reflections, magenta
// reverberant tail.  All five knob values feed in via setters; layout
// algorithm identical to StripReverbPanel::GridBox so the two views
// read consistently.  Compact (90 px) to fit the applet footprint.
class ReverbVizBox : public QWidget {
public:
    explicit ReverbVizBox(QWidget* parent = nullptr) : QWidget(parent) {
        setFixedHeight(90);
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

        // Background grid + axes.
        const QColor gridColor("#1e3040");
        const QColor axisColor("#2a4458");
        p.setPen(QPen(gridColor, 1.0));
        for (float n : { 0.25f, 0.50f, 0.75f }) {
            const float x = r.left() + n * r.width();
            const float y = r.top()  + n * r.height();
            p.drawLine(QPointF(x, r.top()),  QPointF(x, r.bottom()));
            p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
        }
        p.setPen(QPen(axisColor, 1.0));
        const float cx = r.left() + 0.5f * r.width();
        const float cy = r.top()  + 0.5f * r.height();
        p.drawLine(QPointF(cx, r.top()),  QPointF(cx, r.bottom()));
        p.drawLine(QPointF(r.left(), cy), QPointF(r.right(), cy));

        const float w = r.width();
        const float h = r.height();
        const float midY  = r.top() + h * 0.5f;
        const float maxAmp = h * 0.40f;
        const float xStart = w * 0.04f;
        const float preX   = (m_preDelayMs / 100.0f) * w * 0.12f;
        const float wetStart = xStart + preX;

        // Reverb tail (magenta).
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
            p.setPen(QPen(QColor("#c060e0"), 1.6));
            p.drawPath(path);
        }

        // First-order reflections (yellow).
        {
            const int   nRefl    = 6;
            const float spacing  = (0.04f + m_size * 0.11f) * w;
            const float reflLen  = std::min(spacing * 0.85f, w * 0.10f);
            const float dampStep = 1.0f - m_damping * 0.45f;
            float amp = m_mix * maxAmp * 0.75f;
            p.setPen(QPen(QColor("#ffd070"), 1.4));
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

        // Dry sine packet (cyan), gradient-faded to the right.
        {
            const float dryEnd = xStart + w * 0.92f;
            const float dryAmp = (1.0f - m_mix * 0.5f) * maxAmp;
            constexpr float kCycles = 8.0f;
            const float freq = kCycles * 2.0f * float(M_PI) / (dryEnd - xStart);
            QPainterPath path;
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
            p.setPen(QPen(QBrush(grad), 1.8));
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

ClientReverbApplet::ClientReverbApplet(QWidget* parent) : QWidget(parent)
{
    buildUI();
    hide();
}

void ClientReverbApplet::buildUI()
{
    setStyleSheet("QWidget { background: transparent; }");

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(4, 4, 4, 4);
    outer->setSpacing(4);

    // Live visualisation — same algorithm as the strip's larger
    // GridBox.  Sits above the knob row so the user sees the dry
    // packet, reflections, and tail update as knobs move.
    auto* viz = new ReverbVizBox;
    m_viz = viz;
    outer->addWidget(viz);

    auto* row = new QHBoxLayout;
    row->setSpacing(4);
    row->addStretch();

    auto makeKnob = [](const QString& label) {
        auto* k = new ClientCompKnob;
        k->setLabel(label);
        k->setCenterLabelMode(true);
        k->setInlineEditEnabled(false);
        k->setFixedSize(38, 48);
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
        // Exponential 0.3 → 5.0 (~16.7x)
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

    m_preDly = makeKnob("Pre");
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
    outer->addLayout(row);

    auto wire = [this, viz](ClientCompKnob* k, auto engineSetter, auto vizSetter) {
        connect(k, &ClientCompKnob::valueChanged, this,
                [this, viz, engineSetter, vizSetter](float v) {
            if (m_audio && m_audio->clientReverbTx()) {
                (m_audio->clientReverbTx()->*engineSetter)(v);
                m_audio->saveClientReverbSettings();
            }
            (viz->*vizSetter)(v);
        });
    };
    wire(m_size,    &ClientReverb::setSize,        &ReverbVizBox::setSize);
    wire(m_decay,   &ClientReverb::setDecayS,      &ReverbVizBox::setDecayS);
    wire(m_damping, &ClientReverb::setDamping,     &ReverbVizBox::setDamping);
    wire(m_preDly,  &ClientReverb::setPreDelayMs,  &ReverbVizBox::setPreDelayMs);
    wire(m_mix,     &ClientReverb::setMix,         &ReverbVizBox::setMix);

    // No timer — sync is event-driven via AudioEngine's
    // clientReverbStateChanged signal (wired in setAudioEngine).
}

void ClientReverbApplet::setAudioEngine(AudioEngine* engine)
{
    m_audio = engine;
    if (!m_audio) return;
    syncKnobsFromEngine();
    connect(m_audio, &AudioEngine::clientReverbStateChanged,
            this, &ClientReverbApplet::syncKnobsFromEngine);
}

void ClientReverbApplet::refreshEnableFromEngine()
{
    syncKnobsFromEngine();
}

void ClientReverbApplet::syncKnobsFromEngine()
{
    // Bypass dim — render the whole tile at reduced opacity when the
    // stage is bypassed, matching the dim effect on the EQ curve.
    auto* r0 = (m_audio ? m_audio->clientReverbTx() : nullptr);
    const bool dspEnabled = r0 ? r0->isEnabled() : true;
    auto* eff = qobject_cast<QGraphicsOpacityEffect*>(graphicsEffect());
    if (!eff) {
        eff = new QGraphicsOpacityEffect(this);
        setGraphicsEffect(eff);
    }
    eff->setOpacity(dspEnabled ? 1.0 : 0.55);

    if (!m_audio || !m_audio->clientReverbTx()) return;
    ClientReverb* r = m_audio->clientReverbTx();
    if (m_size)    { QSignalBlocker b(m_size);    m_size->setValue(r->size()); }
    if (m_decay)   { QSignalBlocker b(m_decay);   m_decay->setValue(r->decayS()); }
    if (m_damping) { QSignalBlocker b(m_damping); m_damping->setValue(r->damping()); }
    if (m_preDly)  { QSignalBlocker b(m_preDly);  m_preDly->setValue(r->preDelayMs()); }
    if (m_mix)     { QSignalBlocker b(m_mix);     m_mix->setValue(r->mix()); }
    if (auto* viz = dynamic_cast<ReverbVizBox*>(m_viz)) {
        viz->setSize       (r->size());
        viz->setDecayS     (r->decayS());
        viz->setDamping    (r->damping());
        viz->setPreDelayMs (r->preDelayMs());
        viz->setMix        (r->mix());
    }
}

} // namespace MasterSDR
