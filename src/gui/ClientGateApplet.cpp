#include "ClientGateApplet.h"
#include "ClientCompKnob.h"
#include "ClientGateCurveWidget.h"
#include "MeterSmoother.h"
#include "core/AudioEngine.h"
#include "core/ClientGate.h"
#include "core/ThemeManager.h"

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

namespace AetherSDR {

// Gain-reduction mini-strip for the applet.  Named at file scope (not
// an anonymous namespace) so ClientGateApplet.h can forward-declare it
// and keep a typed pointer.  Fill motion uses the shared MeterSmoother
// ballistics so it reads identically to the compressor GR strip.
class ClientGateGrBar : public QWidget {
public:
    explicit ClientGateGrBar(QWidget* parent = nullptr) : QWidget(parent)
    {
        setFixedHeight(10);

        m_animTimer.setTimerType(Qt::PreciseTimer);
        m_animTimer.setInterval(kMeterSmootherIntervalMs);
        connect(&m_animTimer, &QTimer::timeout, this, [this]() {
            const bool settled = !m_smooth.tick(m_animElapsed.restart());
            if (settled)
                m_animTimer.stop();
            if (settled || m_smooth.shouldRepaint())
                update();
        });
    }

    void setGrDb(float grDb)
    {
        if (std::fabs(grDb - m_grDb) < 0.05f) return;
        m_grDb = grDb;
        // Gate attenuation can be much deeper than comp — 40 dB scale
        // reads better for a gate tile.
        constexpr float kMaxGr = 40.0f;
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
        constexpr float kMaxGr = 40.0f;
        const float frac = m_smooth.value();
        if (frac > 0.0f) {
            const float w = frac * r.width();
            QRectF fill(r.right() - w, r.top() + 1.0, w, r.height() - 2.0);
            p.fillRect(fill, QColor("#c8a040"));   // amber — matches curve
        }
        // -15 dB tick (soft-expander default floor)
        const float tickX = r.right() - (15.0f / kMaxGr) * r.width();
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

ClientGateApplet::ClientGateApplet(Side side, QWidget* parent)
    : QWidget(parent)
    , m_side(side)
{
    theme::setContainer(this, QStringLiteral("applet/gate"));
    buildUI();
    hide();
}

ClientGate* ClientGateApplet::gate() const
{
    if (!m_audio) return nullptr;
    return m_side == Side::Rx ? m_audio->clientGateRx()
                              : m_audio->clientGateTx();
}

void ClientGateApplet::saveGateSettings() const
{
    if (!m_audio) return;
    if (m_side == Side::Rx) m_audio->saveClientGateRxSettings();
    else                    m_audio->saveClientGateSettings();
}

void ClientGateApplet::buildUI()
{
    setStyleSheet("QWidget { background: transparent; }");

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(4, 4, 4, 4);
    outer->setSpacing(4);

    m_curve = new ClientGateCurveWidget;
    m_curve->setCompactMode(true);
    m_curve->setMinimumHeight(90);
    outer->addWidget(m_curve, 1);

    m_grBar = new ClientGateGrBar;
    outer->addWidget(m_grBar);

    // Enable / Edit buttons removed — the CHAIN widget now handles
    // bypass (single-click) and opens the editor (double-click).

    // Five-knob tuning row — Thresh, Ratio, Attack, Release, Floor.
    // Mappings mirror ClientGateEditor so turning a knob here gives
    // identical DSP results.
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

        m_thresh = makeKnob("Thresh");
        m_thresh->setRange(-80.0f, 0.0f);
        m_thresh->setDefault(-40.0f);
        m_thresh->setValueFromNorm([](float n) { return -80.0f + n * 80.0f; });
        m_thresh->setNormFromValue([](float v) { return (v + 80.0f) / 80.0f; });
        m_thresh->setLabelFormat([](float v) {
            return QString::number(v, 'f', 1) + " dB";
        });
        row->addWidget(m_thresh);

        m_ratio = makeKnob("Ratio");
        m_ratio->setRange(1.0f, 10.0f);
        m_ratio->setDefault(2.0f);
        m_ratio->setValueFromNorm([](float n) { return 1.0f + n * 9.0f; });
        m_ratio->setNormFromValue([](float v) { return (v - 1.0f) / 9.0f; });
        m_ratio->setLabelFormat([](float v) {
            return QString::number(v, 'f', 1) + ":1";
        });
        row->addWidget(m_ratio);

        m_return = makeKnob("Return");
        m_return->setRange(0.0f, 20.0f);
        m_return->setDefault(2.0f);
        m_return->setValueFromNorm([](float n) { return n * 20.0f; });
        m_return->setNormFromValue([](float v) { return v / 20.0f; });
        m_return->setLabelFormat([](float v) {
            return QString::number(v, 'f', 2) + " dB";
        });
        row->addWidget(m_return);

        m_release = makeKnob("Release");
        m_release->setRange(5.0f, 2000.0f);
        m_release->setDefault(100.0f);
        m_release->setValueFromNorm([](float n) {
            return 5.0f * std::pow(400.0f, n);
        });
        m_release->setNormFromValue([](float v) {
            return std::log(std::max(5.0f, v) / 5.0f) / std::log(400.0f);
        });
        m_release->setLabelFormat([](float v) {
            return QString::number(v, 'f', v < 100.0f ? 1 : 0) + " ms";
        });
        row->addWidget(m_release);

        m_floor = makeKnob("Floor");
        m_floor->setRange(-80.0f, 0.0f);
        m_floor->setDefault(-15.0f);
        m_floor->setValueFromNorm([](float n) { return -80.0f + n * 80.0f; });
        m_floor->setNormFromValue([](float v) { return (v + 80.0f) / 80.0f; });
        m_floor->setLabelFormat([](float v) {
            return QString::number(v, 'f', 1) + " dB";
        });
        row->addWidget(m_floor);

        outer->addLayout(row);
    }

    // Knob → engine wiring.  Each setter is followed by a saveClientGate
    // call so changes survive a restart.  Done in a helper lambda so the
    // connect lines read cleanly.
    auto wire = [this](ClientCompKnob* k, auto setter) {
        connect(k, &ClientCompKnob::valueChanged, this, [this, setter](float v) {
            if (!m_audio || !gate()) return;
            (gate()->*setter)(v);
            saveGateSettings();
        });
    };
    wire(m_thresh,  &ClientGate::setThresholdDb);
    wire(m_ratio,   &ClientGate::setRatio);
    wire(m_return,  &ClientGate::setReturnDb);
    wire(m_release, &ClientGate::setReleaseMs);
    wire(m_floor,   &ClientGate::setFloorDb);

    m_meterTimer = new QTimer(this);
    m_meterTimer->setInterval(33);
    connect(m_meterTimer, &QTimer::timeout, this, &ClientGateApplet::tickMeter);
}

void ClientGateApplet::setAudioEngine(AudioEngine* engine)
{
    m_audio = engine;
    if (!m_audio) return;
    m_curve->setGate(gate());
    syncEnableFromEngine();
    m_meterTimer->start();
}

void ClientGateApplet::syncEnableFromEngine()
{
    if (m_curve) m_curve->update();

    // Bypass dim — render the whole tile at reduced opacity when the
    // stage is bypassed, matching the dim effect on the EQ curve.
    const bool dspEnabled = (m_audio && gate()) ? gate()->isEnabled() : true;
    auto* eff = qobject_cast<QGraphicsOpacityEffect*>(graphicsEffect());
    if (!eff) {
        eff = new QGraphicsOpacityEffect(this);
        setGraphicsEffect(eff);
    }
    eff->setOpacity(dspEnabled ? 1.0 : 0.55);

    if (!m_audio || !gate()) return;
    ClientGate* g = gate();
    if (m_thresh)  { QSignalBlocker b(m_thresh);  m_thresh->setValue(g->thresholdDb()); }
    if (m_ratio)   { QSignalBlocker b(m_ratio);   m_ratio->setValue(g->ratio()); }
    if (m_return)  { QSignalBlocker b(m_return);  m_return->setValue(g->returnDb()); }
    if (m_release) { QSignalBlocker b(m_release); m_release->setValue(g->releaseMs()); }
    if (m_floor)   { QSignalBlocker b(m_floor);   m_floor->setValue(g->floorDb()); }
}

void ClientGateApplet::refreshEnableFromEngine()
{
    syncEnableFromEngine();
}

void ClientGateApplet::onEnableToggled(bool on)
{
    if (!m_audio) return;
    ClientGate* g = gate();
    if (!g) return;
    g->setEnabled(on);
    saveGateSettings();
    if (m_curve) m_curve->update();
}

void ClientGateApplet::tickMeter()
{
    if (!m_audio || !m_grBar) return;
    ClientGate* g = gate();
    if (!g) return;
    const float gr = g->gainReductionDb();
    m_grBar->setGrDb(gr);
    m_grDb = gr;

    // Sync knobs back from engine so changes made in the floating
    // editor show up live here (and vice versa).  QSignalBlocker
    // prevents the setValue from re-driving the engine.
    if (m_thresh)  { QSignalBlocker b(m_thresh);  m_thresh->setValue(g->thresholdDb()); }
    if (m_ratio)   { QSignalBlocker b(m_ratio);   m_ratio->setValue(g->ratio()); }
    if (m_return)  { QSignalBlocker b(m_return);  m_return->setValue(g->returnDb()); }
    if (m_release) { QSignalBlocker b(m_release); m_release->setValue(g->releaseMs()); }
    if (m_floor)   { QSignalBlocker b(m_floor);   m_floor->setValue(g->floorDb()); }
}

} // namespace AetherSDR
