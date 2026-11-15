#include "ClientCompEditor.h"
#include "ClientCompEditorCanvas.h"
#include "ClientCompKnob.h"
#include "ClientCompLimiterButton.h"
#include "ClientCompMeter.h"
#include "ClientCompThresholdFader.h"
#include "EditorFramelessTitleBar.h"
#include "core/AppSettings.h"
#include "core/AudioEngine.h"
#include "core/ClientComp.h"

#include <QCloseEvent>
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

namespace AetherSDR {

namespace {

constexpr int kDefaultWidth  = 860;
constexpr int kDefaultHeight = 400;

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

const QString kLimBtnStyle = QStringLiteral(
    "QPushButton {"
    "  background: #1a2a3a; border: 1px solid #205070; border-radius: 3px;"
    "  color: #c8d8e8; font-size: 10px; font-weight: bold; padding: 3px 6px;"
    "}"
    "QPushButton:hover { background: #204060; }"
    "QPushButton:checked {"
    "  background: #006040; color: #00ff88; border: 1px solid #00a060;"
    "}");

} // namespace

ClientCompEditor::ClientCompEditor(AudioEngine* engine, QWidget* parent)
    : QWidget(parent, Qt::Window | Qt::FramelessWindowHint)
    , m_audio(engine)
{
    setWindowTitle("Aetherial Compressor");
    setStyleSheet(kWindowStyle);
    resize(kDefaultWidth, kDefaultHeight);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 0, 8, 8);
    root->setSpacing(6);

    auto* titleBar = new EditorFramelessTitleBar;
    m_titleBar = titleBar;
    root->addWidget(titleBar);

    // Bypass moved to the CHAIN widget's single-click gesture — nothing
    // here in the header.

    // ── Main body: knobs column | canvas | meters | makeup/limiter ──
    {
        auto* body = new QHBoxLayout;
        body->setSpacing(8);

        // Knobs column (Ratio / Attack / Release / Knee).
        {
            auto* col = new QVBoxLayout;
            col->setSpacing(4);

            m_ratio = new ClientCompKnob;
            m_ratio->setCenterLabelMode(true);
            m_ratio->setLabel("Ratio");
            m_ratio->setRange(1.0f, 20.0f);
            m_ratio->setDefault(3.0f);
            m_ratio->setValueFromNorm([](float n) {
                // Exponential — 1:1 at 0, 20:1 at 1, with a gentle curve
                // so the middle of the knob lands near 4:1.
                return 1.0f * std::pow(20.0f, n);
            });
            m_ratio->setNormFromValue([](float v) {
                if (v <= 1.0f) return 0.0f;
                return std::log(v) / std::log(20.0f);
            });
            m_ratio->setLabelFormat([](float v) {
                return QString::number(v, 'f', 2) + " :1";
            });
            m_ratio->setFixedSize(76, 76);
            col->addWidget(m_ratio);

            m_attack = new ClientCompKnob;
            m_attack->setCenterLabelMode(true);
            m_attack->setLabel("Attack");
            m_attack->setRange(0.1f, 300.0f);
            m_attack->setDefault(20.0f);
            m_attack->setValueFromNorm([](float n) {
                return 0.1f * std::pow(3000.0f, n);  // 0.1 .. 300 ms
            });
            m_attack->setNormFromValue([](float v) {
                if (v <= 0.1f) return 0.0f;
                return std::log(v / 0.1f) / std::log(3000.0f);
            });
            m_attack->setLabelFormat([](float v) {
                return v < 10.0f ? QString::number(v, 'f', 1) + " ms"
                                  : QString::number(v, 'f', 0) + " ms";
            });
            m_attack->setFixedSize(76, 76);
            col->addWidget(m_attack);

            m_release = new ClientCompKnob;
            m_release->setCenterLabelMode(true);
            m_release->setLabel("Release");
            m_release->setRange(5.0f, 2000.0f);
            m_release->setDefault(200.0f);
            m_release->setValueFromNorm([](float n) {
                return 5.0f * std::pow(400.0f, n);    // 5 .. 2000 ms
            });
            m_release->setNormFromValue([](float v) {
                if (v <= 5.0f) return 0.0f;
                return std::log(v / 5.0f) / std::log(400.0f);
            });
            m_release->setLabelFormat([](float v) {
                return QString::number(v, 'f', 0) + " ms";
            });
            m_release->setFixedSize(76, 76);
            col->addWidget(m_release);

            m_knee = new ClientCompKnob;
            m_knee->setCenterLabelMode(true);
            m_knee->setLabel("Knee");
            m_knee->setRange(0.0f, 24.0f);
            m_knee->setDefault(6.0f);
            m_knee->setLabelFormat([](float v) {
                return QString::number(v, 'f', 1) + " dB";
            });
            m_knee->setFixedSize(76, 76);
            col->addWidget(m_knee);

            col->addStretch();
            body->addLayout(col);
        }

        // Threshold fader — combined input-level meter + threshold
        // slider, matching the Client EQ output-fader visual language.
        // Drag the amber handle to set threshold; the chevron on the
        // curve canvas stays linked via the shared ClientComp state.
        m_threshFader = new ClientCompThresholdFader;
        body->addWidget(m_threshFader);

        // Canvas (center).
        m_canvas = new ClientCompEditorCanvas;
        body->addWidget(m_canvas, 1);

        // GR + Output meters.
        {
            auto* col = new QVBoxLayout;
            col->setSpacing(2);

            m_grMeter = new ClientCompMeter;
            m_grMeter->setMode(ClientCompMeter::Mode::GainReduction);
            m_grMeter->setLabel("GR");
            m_grMeter->setMinimumWidth(20);

            m_outputMeter = new ClientCompMeter;
            m_outputMeter->setMode(ClientCompMeter::Mode::Level);
            m_outputMeter->setLabel("Out");
            m_outputMeter->setMinimumWidth(20);

            col->addWidget(m_grMeter, 1);
            body->addLayout(col);

            auto* col2 = new QVBoxLayout;
            col2->setSpacing(2);
            col2->addWidget(m_outputMeter, 1);
            body->addLayout(col2);
        }

        // Limiter + Makeup column.
        {
            auto* col = new QVBoxLayout;
            col->setSpacing(6);

            m_limiterEnable = new ClientCompLimiterButton;
            m_limiterEnable->setToolTip(
                "Brickwall peak limiter on the compressor output.\n"
                "Button glows red when the limiter is actively clamping.");
            col->addWidget(m_limiterEnable);

            m_ceiling = new ClientCompKnob;
            m_ceiling->setCenterLabelMode(true);
            m_ceiling->setLabel("Ceiling");
            m_ceiling->setRange(-24.0f, 0.0f);
            m_ceiling->setDefault(-1.0f);
            m_ceiling->setLabelFormat([](float v) {
                return QString::number(v, 'f', 1) + " dB";
            });
            m_ceiling->setFixedSize(76, 76);
            col->addWidget(m_ceiling);

            m_makeup = new ClientCompKnob;
            m_makeup->setCenterLabelMode(true);
            m_makeup->setLabel("Makeup");
            m_makeup->setRange(-12.0f, 24.0f);
            m_makeup->setDefault(0.0f);
            m_makeup->setLabelFormat([](float v) {
                return (v >= 0.0f ? "+" : "") + QString::number(v, 'f', 1) + " dB";
            });
            m_makeup->setFixedSize(76, 76);
            col->addWidget(m_makeup);
            col->addStretch();
            body->addLayout(col);
        }

        root->addLayout(body, 1);
    }

    // ── Signal wiring ───────────────────────────────────────────────
    if (m_audio && comp()) {
        m_canvas->setComp(comp());
    }

    connect(m_canvas, &ClientCompEditorCanvas::thresholdChanged,
            this, &ClientCompEditor::applyThreshold);
    connect(m_threshFader, &ClientCompThresholdFader::thresholdChanged,
            this, &ClientCompEditor::applyThreshold);
    connect(m_canvas, &ClientCompEditorCanvas::ratioChanged,
            this, &ClientCompEditor::applyRatio);

    connect(m_ratio,   &ClientCompKnob::valueChanged,
            this, &ClientCompEditor::applyRatio);
    connect(m_attack,  &ClientCompKnob::valueChanged,
            this, &ClientCompEditor::applyAttack);
    connect(m_release, &ClientCompKnob::valueChanged,
            this, &ClientCompEditor::applyRelease);
    connect(m_knee,    &ClientCompKnob::valueChanged,
            this, &ClientCompEditor::applyKnee);
    connect(m_makeup,  &ClientCompKnob::valueChanged,
            this, &ClientCompEditor::applyMakeup);
    connect(m_ceiling, &ClientCompKnob::valueChanged,
            this, &ClientCompEditor::applyLimiterCeiling);
    connect(m_limiterEnable, &QPushButton::toggled,
            this, &ClientCompEditor::applyLimiterEnabled);

    syncControlsFromEngine();

    m_meterTimer = new QTimer(this);
    m_meterTimer->setInterval(33);  // ~30 Hz
    connect(m_meterTimer, &QTimer::timeout,
            this, &ClientCompEditor::tickMeters);
}

ClientCompEditor::~ClientCompEditor() = default;

ClientComp* ClientCompEditor::comp() const
{
    if (!m_audio) return nullptr;
    return m_side == Side::Rx ? m_audio->clientCompRx()
                              : m_audio->clientCompTx();
}

void ClientCompEditor::saveCompSettings() const
{
    if (!m_audio) return;
    if (m_side == Side::Rx) m_audio->saveClientCompRxSettings();
    else                    m_audio->saveClientCompSettings();
}

void ClientCompEditor::showForTx()
{
    m_side = Side::Tx;
    if (m_canvas && comp()) m_canvas->setComp(comp());
    const QString title = QString::fromUtf8("Aetherial Compressor \xe2\x80\x94 TX");
    if (m_titleBar)
        static_cast<EditorFramelessTitleBar*>(m_titleBar)->setTitleText(title);
    setWindowTitle(title);
    restoreGeometryFromSettings();
    syncControlsFromEngine();
    if (m_meterTimer) m_meterTimer->start();
    show();
    raise();
    activateWindow();
}

void ClientCompEditor::showForRx()
{
    m_side = Side::Rx;
    if (m_canvas && comp()) m_canvas->setComp(comp());
    const QString title = QString::fromUtf8("Aetherial Compressor \xe2\x80\x94 RX");
    if (m_titleBar)
        static_cast<EditorFramelessTitleBar*>(m_titleBar)->setTitleText(title);
    setWindowTitle(title);
    restoreGeometryFromSettings();
    syncControlsFromEngine();
    if (m_meterTimer) m_meterTimer->start();
    show();
    raise();
    activateWindow();
}

void ClientCompEditor::syncControlsFromEngine()
{
    if (!m_audio) return;
    ClientComp* c = comp();
    if (!c) return;
    QSignalBlocker br(m_ratio);
    QSignalBlocker ba(m_attack);
    QSignalBlocker brl(m_release);
    QSignalBlocker bk(m_knee);
    QSignalBlocker bm(m_makeup);
    QSignalBlocker bce(m_ceiling);
    QSignalBlocker ble(m_limiterEnable);

    m_ratio->setValue(c->ratio());
    m_attack->setValue(c->attackMs());
    m_release->setValue(c->releaseMs());
    m_knee->setValue(c->kneeDb());
    m_makeup->setValue(c->makeupDb());
    m_ceiling->setValue(c->limiterCeilingDb());
    m_limiterEnable->setChecked(c->limiterEnabled());
    if (m_threshFader) m_threshFader->setThresholdDb(c->thresholdDb());
    if (m_canvas) m_canvas->update();
}

void ClientCompEditor::tickMeters()
{
    if (!m_audio) return;
    ClientComp* c = comp();
    if (!c) return;
    if (m_threshFader) m_threshFader->setInputPeakDb(c->inputPeakDb());
    if (m_grMeter)     m_grMeter    ->setValueDb(c->gainReductionDb());
    if (m_outputMeter) {
        m_outputMeter->setValueDb(c->outputPeakDb());
        if (c->limiterEnabled()) {
            m_outputMeter->setLimiterCeilingDb(c->limiterCeilingDb());
            m_outputMeter->setLimiterGrDb(c->limiterGrDb());
        } else {
            m_outputMeter->setLimiterCeilingDb(1.0f);  // disable overlay
            m_outputMeter->setLimiterGrDb(0.0f);
        }
    }
    if (m_limiterEnable) {
        m_limiterEnable->setActive(c->limiterActive() && c->limiterEnabled());
    }

    // Mirror parameter changes made in the applet tile (or other
    // surfaces) back onto the editor knobs.  QSignalBlocker prevents
    // a feedback loop through the valueChanged handlers.
    if (m_ratio)   { QSignalBlocker b(m_ratio);   m_ratio->setValue(c->ratio()); }
    if (m_attack)  { QSignalBlocker b(m_attack);  m_attack->setValue(c->attackMs()); }
    if (m_release) { QSignalBlocker b(m_release); m_release->setValue(c->releaseMs()); }
    if (m_knee)    { QSignalBlocker b(m_knee);    m_knee->setValue(c->kneeDb()); }
    if (m_makeup)  { QSignalBlocker b(m_makeup);  m_makeup->setValue(c->makeupDb()); }
    if (m_ceiling) { QSignalBlocker b(m_ceiling); m_ceiling->setValue(c->limiterCeilingDb()); }
    if (m_threshFader) {
        QSignalBlocker b(m_threshFader);
        m_threshFader->setThresholdDb(c->thresholdDb());
    }
}

void ClientCompEditor::applyThreshold(float db)
{
    if (!m_audio) return;
    ClientComp* c = comp();
    if (!c) return;
    c->setThresholdDb(db);
    saveCompSettings();
    // Mirror the value onto whichever control didn't originate the
    // change.  Canvas chevron drags land here too, so this keeps the
    // fader handle in sync.  Signal blocking avoids a feedback loop.
    if (m_threshFader && std::fabs(m_threshFader->thresholdDb() - db) > 0.01f) {
        QSignalBlocker b(m_threshFader);
        m_threshFader->setThresholdDb(db);
    }
    if (m_canvas) m_canvas->update();
}

void ClientCompEditor::applyRatio(float ratio)
{
    if (!m_audio) return;
    ClientComp* c = comp();
    if (!c) return;
    c->setRatio(ratio);
    saveCompSettings();
    // Mirror to the knob if the change came from the canvas.
    if (m_ratio && std::fabs(m_ratio->value() - ratio) > 0.001f) {
        QSignalBlocker b(m_ratio);
        m_ratio->setValue(ratio);
    }
    if (m_canvas) m_canvas->update();
}

void ClientCompEditor::applyAttack(float ms)
{
    if (!m_audio) return;
    if (auto* c = comp()) c->setAttackMs(ms);
    saveCompSettings();
}

void ClientCompEditor::applyRelease(float ms)
{
    if (!m_audio) return;
    if (auto* c = comp()) c->setReleaseMs(ms);
    saveCompSettings();
}

void ClientCompEditor::applyKnee(float db)
{
    if (!m_audio) return;
    if (auto* c = comp()) c->setKneeDb(db);
    saveCompSettings();
    if (m_canvas) m_canvas->update();
}

void ClientCompEditor::applyMakeup(float db)
{
    if (!m_audio) return;
    if (auto* c = comp()) c->setMakeupDb(db);
    saveCompSettings();
    if (m_canvas) m_canvas->update();
}

void ClientCompEditor::applyLimiterEnabled(bool on)
{
    if (!m_audio) return;
    if (auto* c = comp()) c->setLimiterEnabled(on);
    saveCompSettings();
}

void ClientCompEditor::applyLimiterCeiling(float db)
{
    if (!m_audio) return;
    if (auto* c = comp()) c->setLimiterCeilingDb(db);
    saveCompSettings();
}

void ClientCompEditor::saveGeometryToSettings()
{
    auto& s = AppSettings::instance();
    s.setValue("ClientCompEditorGeometry", saveGeometry().toBase64());
}

void ClientCompEditor::restoreGeometryFromSettings()
{
    auto& s = AppSettings::instance();
    const QByteArray geom = QByteArray::fromBase64(
        s.value("ClientCompEditorGeometry", "").toByteArray());
    if (!geom.isEmpty()) {
        m_restoring = true;
        restoreGeometry(geom);
        m_restoring = false;
    }
}

void ClientCompEditor::closeEvent(QCloseEvent* ev)
{
    if (m_meterTimer) m_meterTimer->stop();
    saveGeometryToSettings();
    QWidget::closeEvent(ev);
}

void ClientCompEditor::moveEvent(QMoveEvent* ev)
{
    QWidget::moveEvent(ev);
    if (!m_restoring) saveGeometryToSettings();
}

void ClientCompEditor::resizeEvent(QResizeEvent* ev)
{
    QWidget::resizeEvent(ev);
    if (!m_restoring) saveGeometryToSettings();
}

void ClientCompEditor::showEvent(QShowEvent* ev)
{
    QWidget::showEvent(ev);
    if (m_meterTimer) m_meterTimer->start();
}

void ClientCompEditor::hideEvent(QHideEvent* ev)
{
    QWidget::hideEvent(ev);
    if (m_meterTimer) m_meterTimer->stop();
}

} // namespace AetherSDR
