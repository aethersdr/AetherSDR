#include "ClientCompApplet.h"
#include "ClientCompCurveWidget.h"
#include "ClientCompKnob.h"
#include "MeterSmoother.h"
#include "core/AudioEngine.h"
#include "core/ClientComp.h"
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

// Gain-reduction mini-strip used by the applet.  Named at file scope
// (not in an anonymous namespace) so ClientCompApplet.h can forward-
// declare it and keep a typed pointer.  Fill motion uses the shared
// MeterSmoother ballistics so the strip reads identically to every
// other metering surface in the app.
class ClientCompGrBar : public QWidget {
public:
    explicit ClientCompGrBar(QWidget* parent = nullptr) : QWidget(parent)
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
        constexpr float kMaxGr = 20.0f;
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
        constexpr float kMaxGr = 20.0f;
        const float frac = m_smooth.value();
        if (frac > 0.0f) {
            const float w = frac * r.width();
            QRectF fill(r.right() - w, r.top() + 1.0, w, r.height() - 2.0);
            p.fillRect(fill, QColor("#e8a540"));
        }
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

// Enable/Bypass toggle — matches ClientEqApplet's style so the two
// applets sit visually side-by-side with identical button language.
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

ClientCompApplet::ClientCompApplet(Side side, QWidget* parent)
    : QWidget(parent)
    , m_side(side)
{
    theme::setContainer(this, QStringLiteral("applet/comp"));
    // Slider fill at applet/comp scope (amber — compression amount
    // carries a warning-ish cast for "you are colouring the signal").
    AetherSDR::ThemeManager::instance().applyStyleSheet(this,
        "QSlider::sub-page:horizontal { background: {{color.slider.foreground}}; }"
        "QSlider::sub-page:vertical   { background: {{color.slider.foreground}}; }");
    buildUI();
    hide();  // hidden until toggled on from the button tray
}

ClientComp* ClientCompApplet::comp() const
{
    if (!m_audio) return nullptr;
    return m_side == Side::Rx ? m_audio->clientCompRx()
                              : m_audio->clientCompTx();
}

void ClientCompApplet::saveCompSettings() const
{
    if (!m_audio) return;
    if (m_side == Side::Rx) m_audio->saveClientCompRxSettings();
    else                    m_audio->saveClientCompSettings();
}

void ClientCompApplet::buildUI()
{
    setStyleSheet("QWidget { background: transparent; }");

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(4, 4, 4, 4);
    outer->setSpacing(4);

    m_curve = new ClientCompCurveWidget;
    m_curve->setCompactMode(true);
    m_curve->setMinimumHeight(90);
    outer->addWidget(m_curve, 1);

    m_grBar = new ClientCompGrBar;
    outer->addWidget(m_grBar);

    // Enable / Edit buttons removed — CHAIN widget handles bypass
    // (single-click) and editor-open (double-click).

    // Five-knob tuning row — Thresh, Ratio, Attack, Release, Makeup.
    // Mappings match ClientCompEditor.
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
        m_thresh->setRange(-60.0f, 0.0f);
        m_thresh->setDefault(-18.0f);
        m_thresh->setValueFromNorm([](float n) { return -60.0f + n * 60.0f; });
        m_thresh->setNormFromValue([](float v) { return (v + 60.0f) / 60.0f; });
        m_thresh->setLabelFormat([](float v) {
            return QString::number(v, 'f', 1) + " dB";
        });
        row->addWidget(m_thresh);

        m_ratio = makeKnob("Ratio");
        m_ratio->setRange(1.0f, 20.0f);
        m_ratio->setDefault(3.0f);
        m_ratio->setValueFromNorm([](float n) {
            return 1.0f * std::pow(20.0f, n);
        });
        m_ratio->setNormFromValue([](float v) {
            if (v <= 1.0f) return 0.0f;
            return std::log(v) / std::log(20.0f);
        });
        m_ratio->setLabelFormat([](float v) {
            return QString::number(v, 'f', 2) + ":1";
        });
        row->addWidget(m_ratio);

        m_attack = makeKnob("Attack");
        m_attack->setRange(0.1f, 300.0f);
        m_attack->setDefault(20.0f);
        m_attack->setValueFromNorm([](float n) {
            return 0.1f * std::pow(3000.0f, n);
        });
        m_attack->setNormFromValue([](float v) {
            if (v <= 0.1f) return 0.0f;
            return std::log(v / 0.1f) / std::log(3000.0f);
        });
        m_attack->setLabelFormat([](float v) {
            return v < 10.0f ? QString::number(v, 'f', 1) + " ms"
                              : QString::number(v, 'f', 0) + " ms";
        });
        row->addWidget(m_attack);

        m_release = makeKnob("Release");
        m_release->setRange(5.0f, 2000.0f);
        m_release->setDefault(200.0f);
        m_release->setValueFromNorm([](float n) {
            return 5.0f * std::pow(400.0f, n);
        });
        m_release->setNormFromValue([](float v) {
            if (v <= 5.0f) return 0.0f;
            return std::log(v / 5.0f) / std::log(400.0f);
        });
        m_release->setLabelFormat([](float v) {
            return QString::number(v, 'f', 0) + " ms";
        });
        row->addWidget(m_release);

        m_makeup = makeKnob("Makeup");
        m_makeup->setRange(-12.0f, 24.0f);
        m_makeup->setDefault(0.0f);
        m_makeup->setLabelFormat([](float v) {
            return (v >= 0.0f ? "+" : "") + QString::number(v, 'f', 1) + " dB";
        });
        row->addWidget(m_makeup);

        outer->addLayout(row);
    }

    auto wire = [this](ClientCompKnob* k, auto setter) {
        connect(k, &ClientCompKnob::valueChanged, this, [this, setter](float v) {
            if (!m_audio || !comp()) return;
            (comp()->*setter)(v);
            saveCompSettings();
        });
    };
    wire(m_thresh,  &ClientComp::setThresholdDb);
    wire(m_ratio,   &ClientComp::setRatio);
    wire(m_attack,  &ClientComp::setAttackMs);
    wire(m_release, &ClientComp::setReleaseMs);
    wire(m_makeup,  &ClientComp::setMakeupDb);

    m_meterTimer = new QTimer(this);
    m_meterTimer->setInterval(33);
    connect(m_meterTimer, &QTimer::timeout, this, &ClientCompApplet::tickMeter);
}

void ClientCompApplet::setAudioEngine(AudioEngine* engine)
{
    m_audio = engine;
    if (!m_audio) return;
    m_curve->setComp(comp());
    syncEnableFromEngine();
    m_meterTimer->start();
}

void ClientCompApplet::syncEnableFromEngine()
{
    if (m_curve) m_curve->update();

    // Bypass dim — render the whole tile at reduced opacity when the
    // stage is bypassed, matching the dim effect on the EQ curve.
    const bool dspEnabled = (m_audio && comp()) ? comp()->isEnabled() : true;
    auto* eff = qobject_cast<QGraphicsOpacityEffect*>(graphicsEffect());
    if (!eff) {
        eff = new QGraphicsOpacityEffect(this);
        setGraphicsEffect(eff);
    }
    eff->setOpacity(dspEnabled ? 1.0 : 0.55);

    if (!m_audio || !comp()) return;
    ClientComp* c = comp();
    if (m_thresh)  { QSignalBlocker b(m_thresh);  m_thresh->setValue(c->thresholdDb()); }
    if (m_ratio)   { QSignalBlocker b(m_ratio);   m_ratio->setValue(c->ratio()); }
    if (m_attack)  { QSignalBlocker b(m_attack);  m_attack->setValue(c->attackMs()); }
    if (m_release) { QSignalBlocker b(m_release); m_release->setValue(c->releaseMs()); }
    if (m_makeup)  { QSignalBlocker b(m_makeup);  m_makeup->setValue(c->makeupDb()); }
}

void ClientCompApplet::refreshEnableFromEngine()
{
    syncEnableFromEngine();
}

void ClientCompApplet::onEnableToggled(bool on)
{
    if (!m_audio) return;
    ClientComp* c = comp();
    if (!c) return;
    c->setEnabled(on);
    saveCompSettings();
    if (m_curve) m_curve->update();
}

void ClientCompApplet::tickMeter()
{
    if (!m_audio || !m_grBar) return;
    ClientComp* c = comp();
    if (!c) return;
    const float gr = c->gainReductionDb();
    m_grBar->setGrDb(gr);
    m_grDb = gr;

    if (m_thresh)  { QSignalBlocker b(m_thresh);  m_thresh->setValue(c->thresholdDb()); }
    if (m_ratio)   { QSignalBlocker b(m_ratio);   m_ratio->setValue(c->ratio()); }
    if (m_attack)  { QSignalBlocker b(m_attack);  m_attack->setValue(c->attackMs()); }
    if (m_release) { QSignalBlocker b(m_release); m_release->setValue(c->releaseMs()); }
    if (m_makeup)  { QSignalBlocker b(m_makeup);  m_makeup->setValue(c->makeupDb()); }
}

} // namespace AetherSDR
