#include "StripCompPanel.h"
#include "PanelTick.h"
#include "ClientCompEditorCanvas.h"
#include "ClientCompKnob.h"
#include "ClientCompLimiterButton.h"
#include "ClientCompMeter.h"
#include "ClientCompThresholdFader.h"
#include "EditorFramelessTitleBar.h"
#include "Theme.h"
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
#include <QSlider>
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

StripCompPanel::StripCompPanel(AudioEngine* engine, QWidget* parent)
    : QWidget(parent)
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

    // The page reads the way the gate editor's does: a toolbar of switches
    // along the top, the display filling everything under it, and every knob
    // in one row at the foot. The old shape — a knob column down the left and
    // a second one down the right — pinched the canvas from both sides and
    // left a hole under each column that nothing could fill.
    auto* toolbar = new QHBoxLayout;
    toolbar->setSpacing(8);

    // Limiter switch at the left, its ceiling at the right-hand end: the two
    // halves of one control, so the ceiling reading sits where the operator
    // looks after arming the limiter rather than in the knob row.
    toolbar->addWidget(new QLabel("Limiter:"));

    m_limiterEnable = new ClientCompLimiterButton;
    m_limiterEnable->setToolTip(
        "Brickwall peak limiter on the compressor output.\n"
        "Button glows red when the limiter is actively clamping.");
    toolbar->addWidget(m_limiterEnable);

    toolbar->addStretch(1);

    toolbar->addWidget(new QLabel("Ceiling:"));

    // Tenths of a dB so the slider lands on the same values the knob could
    // reach; the reading sits beside it because a slider cannot say "-1.0 dB".
    m_ceiling = new QSlider(Qt::Horizontal);
    m_ceiling->setObjectName(QStringLiteral("compCeilingSlider"));
    m_ceiling->setAccessibleName(QStringLiteral("Limiter ceiling"));
    m_ceiling->setRange(-240, 0);
    m_ceiling->setPageStep(10);
    m_ceiling->setFixedWidth(120);
    m_ceiling->setToolTip(
        "Limiter ceiling: the peak level the brickwall will not let the "
        "compressor output exceed.");
    applyPrimarySliderStyle(m_ceiling);
    toolbar->addWidget(m_ceiling);

    m_ceilingValue = new QLabel;
    m_ceilingValue->setFixedWidth(52);
    m_ceilingValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    toolbar->addWidget(m_ceilingValue);

    // Seed before wiring: setValue() during construction would otherwise
    // write the default straight back into the engine and persist it.
    m_ceiling->setValue(-10);                       // -1.0 dB
    m_ceilingValue->setText(QStringLiteral("-1.0 dB"));

    connect(m_ceiling, &QSlider::valueChanged, this, [this](int tenths) {
        const float db = float(tenths) / 10.0f;
        if (m_ceilingValue)
            m_ceilingValue->setText(QString::number(db, 'f', 1) + " dB");
        applyLimiterCeiling(db);
    });

    // ── Body: threshold fader | canvas | GR | Out ───────────────────
    auto* body = new QHBoxLayout;
    body->setSpacing(8);

    // Threshold fader — combined input-level meter + threshold slider,
    // matching the Client EQ output-fader visual language. Drag the amber
    // handle to set threshold; the chevron on the curve canvas stays linked
    // via the shared ClientComp state.
    m_threshFader = new ClientCompThresholdFader;
    body->addWidget(m_threshFader);

    m_canvas = new ClientCompEditorCanvas;
    body->addWidget(m_canvas, 1);

    {
        m_grMeter = new ClientCompMeter;
        m_grMeter->setMode(ClientCompMeter::Mode::GainReduction);
        m_grMeter->setLabel("GR");
        m_grMeter->setTickSide(ClientCompMeter::TickSide::Left);
        m_grMeter->setShowValueLabel(true);
        // Match THRESH fader's compact width — tick column + bar
        // is roughly 22 + 16 = 38 px, plus padding.
        m_grMeter->setFixedWidth(42);

        m_outputMeter = new ClientCompMeter;
        m_outputMeter->setMode(ClientCompMeter::Mode::Level);
        m_outputMeter->setLabel("Out");
        m_outputMeter->setTickSide(ClientCompMeter::TickSide::Right);
        m_outputMeter->setShowValueLabel(true);
        // The Out meter carries the makeup fader, so it needs its own tick
        // gutter on the left for the makeup scale on top of the level ticks
        // on the right — 42 px only ever fitted one of the two.
        m_outputMeter->setMakeupControlEnabled(true);
        m_outputMeter->setFixedWidth(72);

        auto* col = new QVBoxLayout;
        col->setSpacing(2);
        col->addWidget(m_grMeter, 1);
        body->addLayout(col);

        auto* col2 = new QVBoxLayout;
        col2->setSpacing(2);
        col2->addWidget(m_outputMeter, 1);
        body->addLayout(col2);
    }

    // ── Foot: every knob in one row ─────────────────────────────────
    auto* knobs = new QHBoxLayout;
    knobs->setSpacing(8);

    const auto makeKnob = [&](const QString& label) {
        auto* k = new ClientCompKnob;
        k->setCenterLabelMode(true);
        k->setLabel(label);
        k->setFixedSize(76, 76);
        knobs->addWidget(k, 0, Qt::AlignHCenter);
        return k;
    };

    m_ratio = makeKnob("Ratio");
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

    m_attack = makeKnob("Attack");
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

    m_release = makeKnob("Release");
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

    m_knee = makeKnob("Knee");
    m_knee->setRange(0.0f, 24.0f);
    m_knee->setDefault(6.0f);
    m_knee->setLabelFormat([](float v) {
        return QString::number(v, 'f', 1) + " dB";
    });

    // Pre-comp PAPR controls (#2887), TX only — see the header. Drive pushes
    // more material across the threshold so the comp engages harder; Phase
    // rotates voice peaks to be more symmetric so the harder compression
    // doesn't sound trashy. Together they raise RMS without exceeding the
    // ceiling, which is a transmit concern: showForRx() hides both.
    m_drive = makeKnob("Drive");
    m_drive->setRange(0.0f, 18.0f);
    m_drive->setDefault(0.0f);
    m_drive->setLabelFormat([](float v) {
        return "+" + QString::number(v, 'f', 1) + " dB";
    });
    m_drive->setToolTip(
        "Pre-comp drive (#2887). Linear gain before the threshold "
        "so the compressor engages harder and average power lifts. "
        "Pair with Phase to keep peaks clean as drive increases.");

    m_phase = makeKnob("Phase");
    m_phase->setRange(0.0f, 6.0f);
    m_phase->setDefault(0.0f);
    m_phase->setLabelFormat([](float v) {
        const int stages = static_cast<int>(std::round(v));
        return (stages == 0) ? QString("Off")
                             : QString::number(stages) + " stg";
    });
    m_phase->setToolTip(
        "Pre-comp phase rotator (#2887). All-pass cascade that "
        "symmetrizes asymmetric voice peaks before compression. "
        "0 = off, 4 = broadcast default.");

    root->addLayout(toolbar);
    root->addLayout(body, 1);
    root->addLayout(knobs);

    // ── Signal wiring ───────────────────────────────────────────────
    if (m_audio && comp()) {
        m_canvas->setComp(comp());
    }

    connect(m_canvas, &ClientCompEditorCanvas::thresholdChanged,
            this, &StripCompPanel::applyThreshold);
    connect(m_threshFader, &ClientCompThresholdFader::thresholdChanged,
            this, &StripCompPanel::applyThreshold);
    connect(m_canvas, &ClientCompEditorCanvas::ratioChanged,
            this, &StripCompPanel::applyRatio);

    connect(m_ratio,   &ClientCompKnob::valueChanged,
            this, &StripCompPanel::applyRatio);
    connect(m_attack,  &ClientCompKnob::valueChanged,
            this, &StripCompPanel::applyAttack);
    connect(m_release, &ClientCompKnob::valueChanged,
            this, &StripCompPanel::applyRelease);
    connect(m_knee,    &ClientCompKnob::valueChanged,
            this, &StripCompPanel::applyKnee);
    // Makeup now rides the Out meter instead of a knob in the foot row:
    // it is an output-side gain, so it reads where the output level is
    // shown, and the row it left was over-full.
    connect(m_outputMeter, &ClientCompMeter::makeupChanged,
            this, &StripCompPanel::applyMakeup);
    connect(m_limiterEnable, &QPushButton::toggled,
            this, &StripCompPanel::applyLimiterEnabled);
    connect(m_drive, &ClientCompKnob::valueChanged,
            this, &StripCompPanel::applyDrive);
    connect(m_phase, &ClientCompKnob::valueChanged,
            this, &StripCompPanel::applyPhase);

    syncControlsFromEngine();

    m_meterTimer = new QTimer(this);
    m_meterTimer->setInterval(kPanelTickMs);
    connect(m_meterTimer, &QTimer::timeout,
            this, &StripCompPanel::tickMeters);
}

StripCompPanel::~StripCompPanel() = default;

ClientComp* StripCompPanel::comp() const
{
    if (!m_audio) return nullptr;
    return m_side == Side::Rx ? m_audio->clientCompRx()
                              : m_audio->clientCompTx();
}

void StripCompPanel::saveCompSettings() const
{
    if (!m_audio) return;
    if (m_side == Side::Rx) m_audio->saveClientCompRxSettings();
    else                    m_audio->saveClientCompSettings();
}

void StripCompPanel::showForTx()
{
    m_side = Side::Tx;
    if (m_canvas && comp()) m_canvas->setComp(comp());
    if (m_drive) m_drive->setVisible(true);
    if (m_phase) m_phase->setVisible(true);
    const QString title = QString::fromUtf8("Aetherial Compressor \xe2\x80\x94 TX");
    if (m_titleBar)
        static_cast<EditorFramelessTitleBar*>(m_titleBar)->setTitleText(title);
    setWindowTitle(title);
    restoreGeometryFromSettings();
    syncControlsFromEngine();
    // Deliberately does not start the poll: showEvent does that, and only
    // when the widget is actually on screen. Starting it here ran it from
    // construction for a panel that was never shown — and a widget that has
    // never been shown never gets a hideEvent to stop it again (see
    // PanelTick.h).
    show();
    raise();
    activateWindow();
}

void StripCompPanel::showForRx()
{
    m_side = Side::Rx;
    if (m_canvas && comp()) m_canvas->setComp(comp());
    if (m_drive) m_drive->setVisible(false);
    if (m_phase) m_phase->setVisible(false);
    // Drive and Phase are transmit conditioning (#2887) and have no place in
    // the receive chain.  Hiding the knobs is not enough: the RX ClientComp
    // could still be carrying non-zero values from an earlier session on this
    // shared panel, and a hidden 6-stage rotator is a trap.  Force bypass.
    if (ClientComp* c = comp()) {
        if (c->driveDb() != 0.0f || c->phaseRotatorStages() != 0) {
            c->setDriveDb(0.0f);
            c->setPhaseRotatorStages(0);
            saveCompSettings();
        }
    }
    const QString title = QString::fromUtf8("Aetherial Compressor \xe2\x80\x94 RX");
    if (m_titleBar)
        static_cast<EditorFramelessTitleBar*>(m_titleBar)->setTitleText(title);
    setWindowTitle(title);
    restoreGeometryFromSettings();
    syncControlsFromEngine();
    // Deliberately does not start the poll: showEvent does that, and only
    // when the widget is actually on screen. Starting it here ran it from
    // construction for a panel that was never shown — and a widget that has
    // never been shown never gets a hideEvent to stop it again (see
    // PanelTick.h).
    show();
    raise();
    activateWindow();
}

void StripCompPanel::syncControlsFromEngine()
{
    if (!m_audio) return;
    ClientComp* c = comp();
    if (!c) return;
    QSignalBlocker br(m_ratio);
    QSignalBlocker ba(m_attack);
    QSignalBlocker brl(m_release);
    QSignalBlocker bk(m_knee);
    QSignalBlocker bm(m_outputMeter);
    QSignalBlocker bce(m_ceiling);
    QSignalBlocker ble(m_limiterEnable);
    QSignalBlocker bdv(m_drive);
    QSignalBlocker bph(m_phase);

    m_ratio->setValue(c->ratio());
    m_attack->setValue(c->attackMs());
    m_release->setValue(c->releaseMs());
    m_knee->setValue(c->kneeDb());
    m_outputMeter->setMakeupDb(c->makeupDb());
    setCeilingDb(c->limiterCeilingDb());
    m_limiterEnable->setChecked(c->limiterEnabled());
    if (m_side == Side::Tx) {
        m_drive->setValue(c->driveDb());
        m_phase->setValue(static_cast<float>(c->phaseRotatorStages()));
    }
    if (m_threshFader) m_threshFader->setThresholdDb(c->thresholdDb());
    if (m_canvas) m_canvas->update();
}

void StripCompPanel::setCeilingDb(float db)
{
    if (!m_ceiling) return;
    const int tenths = static_cast<int>(std::lround(db * 10.0f));
    if (m_ceiling->value() == tenths) return;
    m_ceiling->setValue(tenths);
    // A blocked setValue() skips the lambda that owns the reading.
    if (m_ceilingValue)
        m_ceilingValue->setText(QString::number(db, 'f', 1) + " dB");
}

void StripCompPanel::tickMeters()
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
    if (m_outputMeter) { QSignalBlocker b(m_outputMeter); m_outputMeter->setMakeupDb(c->makeupDb()); }
    if (m_ceiling) { QSignalBlocker b(m_ceiling); setCeilingDb(c->limiterCeilingDb()); }
    if (m_drive && m_side == Side::Tx) {
        QSignalBlocker b(m_drive);
        m_drive->setValue(c->driveDb());
    }
    if (m_phase && m_side == Side::Tx) {
        QSignalBlocker b(m_phase);
        m_phase->setValue(static_cast<float>(c->phaseRotatorStages()));
    }
    if (m_threshFader) {
        QSignalBlocker b(m_threshFader);
        m_threshFader->setThresholdDb(c->thresholdDb());
    }
}

void StripCompPanel::applyThreshold(float db)
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

void StripCompPanel::applyRatio(float ratio)
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

void StripCompPanel::applyAttack(float ms)
{
    if (!m_audio) return;
    if (auto* c = comp()) c->setAttackMs(ms);
    saveCompSettings();
}

void StripCompPanel::applyRelease(float ms)
{
    if (!m_audio) return;
    if (auto* c = comp()) c->setReleaseMs(ms);
    saveCompSettings();
}

void StripCompPanel::applyKnee(float db)
{
    if (!m_audio) return;
    if (auto* c = comp()) c->setKneeDb(db);
    saveCompSettings();
    if (m_canvas) m_canvas->update();
}

void StripCompPanel::applyDrive(float db)
{
    if (!m_audio || m_side == Side::Rx) return;
    if (auto* c = comp()) c->setDriveDb(db);
    saveCompSettings();
}

void StripCompPanel::applyPhase(float stages)
{
    if (!m_audio || m_side == Side::Rx) return;
    if (auto* c = comp()) c->setPhaseRotatorStages(static_cast<int>(std::round(stages)));
    saveCompSettings();
}

void StripCompPanel::applyMakeup(float db)
{
    if (!m_audio) return;
    if (auto* c = comp()) c->setMakeupDb(db);
    saveCompSettings();
    if (m_canvas) m_canvas->update();
}

void StripCompPanel::applyLimiterEnabled(bool on)
{
    if (!m_audio) return;
    if (auto* c = comp()) c->setLimiterEnabled(on);
    saveCompSettings();
}

void StripCompPanel::applyLimiterCeiling(float db)
{
    if (!m_audio) return;
    if (auto* c = comp()) c->setLimiterCeilingDb(db);
    saveCompSettings();
}

void StripCompPanel::saveGeometryToSettings()
{
    auto& s = AppSettings::instance();
    s.setValue("StripCompPanelGeometry", saveGeometry().toBase64());
}

void StripCompPanel::restoreGeometryFromSettings()
{
    auto& s = AppSettings::instance();
    const QByteArray geom = QByteArray::fromBase64(
        s.value("StripCompPanelGeometry", "").toByteArray());
    if (!geom.isEmpty()) {
        m_restoring = true;
        restoreGeometry(geom);
        m_restoring = false;
    }
}

void StripCompPanel::closeEvent(QCloseEvent* ev)
{
    if (m_meterTimer) m_meterTimer->stop();
    saveGeometryToSettings();
    QWidget::closeEvent(ev);
}

void StripCompPanel::moveEvent(QMoveEvent* ev)
{
    QWidget::moveEvent(ev);
    if (!m_restoring) saveGeometryToSettings();
}

void StripCompPanel::resizeEvent(QResizeEvent* ev)
{
    QWidget::resizeEvent(ev);
    if (!m_restoring) saveGeometryToSettings();
}

void StripCompPanel::showEvent(QShowEvent* ev)
{
    QWidget::showEvent(ev);
    if (m_meterTimer) m_meterTimer->start();
}

void StripCompPanel::hideEvent(QHideEvent* ev)
{
    QWidget::hideEvent(ev);
    if (m_meterTimer) m_meterTimer->stop();
}

} // namespace AetherSDR
