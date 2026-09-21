#include "StripTubePanel.h"
#include "PanelTick.h"
#include "ClientCompKnob.h"
#include "ClientLevelMeter.h"
#include "ClientTubeCurveWidget.h"
#include "EditorFramelessTitleBar.h"
#include "Theme.h"
#include "core/AppSettings.h"
#include "core/AudioEngine.h"
#include "core/ClientTube.h"

#include <QButtonGroup>
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
#include <QSlider>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace AetherSDR {

namespace {

constexpr int kDefaultWidth  = 720;
constexpr int kDefaultHeight = 360;

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

const QString kModelStyle = QStringLiteral(
    "QPushButton {"
    "  background: #1a2a3a; border: 1px solid #2a4458; border-radius: 3px;"
    "  color: #8aa8c0; font-size: 11px; font-weight: bold;"
    "  padding: 3px 10px; min-width: 26px;"
    "}"
    "QPushButton:hover { background: #24384e; }"
    "QPushButton:checked {"
    "  background: #3a2a0e; color: #f2c14e; border: 1px solid #f2c14e;"
    "}");

} // namespace

StripTubePanel::StripTubePanel(AudioEngine* engine, QWidget* parent)
    : QWidget(parent)
    , m_audio(engine)
{
    setWindowTitle("Aetherial Dynamic Tube Pre-Amp");
    setStyleSheet(kWindowStyle);
    resize(kDefaultWidth, kDefaultHeight);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 0, 8, 8);
    root->setSpacing(6);

    auto* titleBar = new EditorFramelessTitleBar;
    m_titleBar = titleBar;
    root->addWidget(titleBar);

    // Bypass moved to the CHAIN widget's single-click gesture.
    // Exclusive model group — buttons are created below in the Tone/Bias
    // row, but the group owner lives here so the change signal can be
    // wired in one place regardless of button placement.
    m_modelGroup = new QButtonGroup(this);
    m_modelGroup->setExclusive(true);
    connect(m_modelGroup, &QButtonGroup::idToggled, this,
            [this](int id, bool checked) {
        if (checked) applyModel(id);
    });

    // The page reads the way the gate and compressor editors' do: a toolbar
    // of switches along the top, the display filling everything under it, and
    // every knob in one row at the foot. The old shape — a knob column down
    // each side of the curve, with Tone, Bias and a vertical A/B/C stack
    // crammed into a strip beneath it — pinched the curve from both sides and
    // still had no room left for the knob labels.
    auto* toolbar = new QHBoxLayout;
    toolbar->setSpacing(8);

    toolbar->addWidget(new QLabel("Model:"));
    {
        const auto addModelBtn = [&](const QString& label, int idx) {
            auto* btn = new QPushButton(label);
            btn->setObjectName(QStringLiteral("tubeModel") + label);
            btn->setCheckable(true);
            btn->setStyleSheet(kModelStyle);
            btn->setFixedHeight(22);
            m_modelGroup->addButton(btn, idx);
            toolbar->addWidget(btn);
            return btn;
        };
        m_modelA = addModelBtn("A", 0);
        m_modelB = addModelBtn("B", 1);
        m_modelC = addModelBtn("C", 2);
    }

    toolbar->addStretch(1);

    // Dry/Wet is the mix, not a shaping control: it reads as a slider at the
    // right-hand end of the toolbar, where the gate puts Peek and the
    // compressor its ceiling. The reading sits beside it because a slider
    // cannot say "100 %".
    toolbar->addWidget(new QLabel("Dry/Wet:"));

    m_dryWet = new QSlider(Qt::Horizontal);
    m_dryWet->setObjectName(QStringLiteral("tubeDryWetSlider"));
    m_dryWet->setAccessibleName(QStringLiteral("Tube dry/wet mix"));
    m_dryWet->setRange(0, 100);
    m_dryWet->setPageStep(10);
    // Twice the gate's Peek width: Peek has five stops, this has a hundred,
    // so it earns the travel.
    m_dryWet->setFixedWidth(240);
    m_dryWet->setToolTip(
        "Dry/Wet: how much of the saturated signal is blended back over "
        "the clean one. 100 % is fully saturated.");
    applyPrimarySliderStyle(m_dryWet);
    toolbar->addWidget(m_dryWet);

    m_dryWetValue = new QLabel;
    m_dryWetValue->setFixedWidth(42);
    m_dryWetValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    toolbar->addWidget(m_dryWetValue);

    // Seed before wiring: setValue() during construction would otherwise
    // write the default straight back into the engine and persist it.
    m_dryWet->setValue(100);
    m_dryWetValue->setText(QStringLiteral("100 %"));

    connect(m_dryWet, &QSlider::valueChanged, this, [this](int pct) {
        if (m_dryWetValue)
            m_dryWetValue->setText(QString::number(pct) + " %");
        applyDryWet(float(pct) / 100.0f);
    });

    // ── Body: curve | output meter ──────────────────────────────────
    auto* body = new QHBoxLayout;
    body->setSpacing(12);

    m_curve = new ClientTubeCurveWidget;
    m_curve->setCompactMode(false);
    m_curve->setMinimumHeight(180);
    body->addWidget(m_curve, 1);

    // Output level meter — far-right column, mirrors the EQ editor. It keeps
    // its default Expanding policy and fills the full column on both sides.
    m_outMeter = new ClientLevelMeter;
    {
        auto* meterCol = new QVBoxLayout;
        meterCol->setContentsMargins(0, 0, 0, 0);
        meterCol->setSpacing(4);
        meterCol->addWidget(m_outMeter, 1);
        body->addLayout(meterCol);
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

    m_drive = makeKnob("Drive");
    m_drive->setRange(0.0f, 24.0f);
    m_drive->setDefault(0.0f);
    m_drive->setValueFromNorm([](float n) { return n * 24.0f; });
    m_drive->setNormFromValue([](float v) { return v / 24.0f; });
    m_drive->setLabelFormat([](float v) {
        return QString::number(v, 'f', 2) + " dB";
    });
    connect(m_drive, &ClientCompKnob::valueChanged,
            this, &StripTubePanel::applyDrive);

    m_tone = makeKnob("Tone");
    m_tone->setRange(-1.0f, 1.0f);
    m_tone->setDefault(0.0f);
    m_tone->setValueFromNorm([](float n) { return -1.0f + n * 2.0f; });
    m_tone->setNormFromValue([](float v) { return (v + 1.0f) / 2.0f; });
    m_tone->setLabelFormat([](float v) {
        return QString::number(v, 'f', 2);
    });
    connect(m_tone, &ClientCompKnob::valueChanged,
            this, &StripTubePanel::applyTone);

    m_bias = makeKnob("Bias");
    m_bias->setRange(0.0f, 1.0f);
    m_bias->setDefault(0.0f);
    m_bias->setValueFromNorm([](float n) { return n; });
    m_bias->setNormFromValue([](float v) { return v; });
    m_bias->setLabelFormat([](float v) {
        return QString::number(static_cast<int>(v * 100.0f + 0.5f)) + " %";
    });
    connect(m_bias, &ClientCompKnob::valueChanged,
            this, &StripTubePanel::applyBias);

    m_envelope = makeKnob("Envelope");
    m_envelope->setRange(-1.0f, 1.0f);
    m_envelope->setDefault(0.0f);
    m_envelope->setValueFromNorm([](float n) { return -1.0f + n * 2.0f; });
    m_envelope->setNormFromValue([](float v) { return (v + 1.0f) / 2.0f; });
    m_envelope->setLabelFormat([](float v) {
        const int pct = static_cast<int>(v * 100.0f + (v >= 0 ? 0.5f : -0.5f));
        return QString::number(pct) + " %";
    });
    connect(m_envelope, &ClientCompKnob::valueChanged,
            this, &StripTubePanel::applyEnvelope);


    m_release = makeKnob("Release");
    m_release->setRange(10.0f, 500.0f);
    m_release->setDefault(35.0f);
    m_release->setValueFromNorm([](float n) {
        return 10.0f * std::pow(50.0f, n);
    });
    m_release->setNormFromValue([](float v) {
        return std::log(std::max(10.0f, v) / 10.0f) / std::log(50.0f);
    });
    m_release->setLabelFormat([](float v) {
        // One decimal, not two: "35.00 ms" is wider than the knob and lost
        // its leading digit to the clip. Same rule the gate's release uses.
        return QString::number(v, 'f', v < 100.0f ? 1 : 0) + " ms";
    });
    connect(m_release, &ClientCompKnob::valueChanged,
            this, &StripTubePanel::applyRelease);

    m_output = makeKnob("Output");
    m_output->setRange(-24.0f, 12.0f);
    m_output->setDefault(0.0f);
    m_output->setValueFromNorm([](float n) { return -24.0f + n * 36.0f; });
    m_output->setNormFromValue([](float v) { return (v + 24.0f) / 36.0f; });
    m_output->setLabelFormat([](float v) {
        return QString::number(v, 'f', 2) + " dB";
    });
    connect(m_output, &ClientCompKnob::valueChanged,
            this, &StripTubePanel::applyOutput);

    root->addLayout(toolbar);
    root->addLayout(body, 1);
    root->addLayout(knobs);

    if (m_audio && tube()) {
        m_curve->setTube(tube());
    }

    syncControlsFromEngine();

    m_syncTimer = new QTimer(this);
    m_syncTimer->setInterval(kPanelTickMs);
    connect(m_syncTimer, &QTimer::timeout,
            this, &StripTubePanel::syncControlsFromEngine);
}

StripTubePanel::~StripTubePanel() = default;

ClientTube* StripTubePanel::tube() const
{
    if (!m_audio) return nullptr;
    return m_side == Side::Rx ? m_audio->clientTubeRx()
                              : m_audio->clientTubeTx();
}

void StripTubePanel::saveTubeSettings() const
{
    if (!m_audio) return;
    if (m_side == Side::Rx) m_audio->saveClientTubeRxSettings();
    else                    m_audio->saveClientTubeSettings();
}

void StripTubePanel::showForTx()
{
    m_side = Side::Tx;
    if (m_curve && tube()) m_curve->setTube(tube());
    const QString title = QString::fromUtf8(
        "Aetherial Dynamic Tube Pre-Amp \xe2\x80\x94 TX");
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

void StripTubePanel::showForRx()
{
    m_side = Side::Rx;
    if (m_curve && tube()) m_curve->setTube(tube());
    const QString title = QString::fromUtf8(
        "Aetherial Dynamic Tube Pre-Amp \xe2\x80\x94 RX");
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

void StripTubePanel::syncControlsFromEngine()
{
    if (!m_audio || !tube()) return;
    ClientTube* t = tube();
    m_restoring = true;

    {
        const int idx = static_cast<int>(t->model());
        QPushButton* btn = (idx == 1) ? m_modelB
                          : (idx == 2) ? m_modelC
                          : m_modelA;
        QSignalBlocker b(m_modelGroup);
        btn->setChecked(true);
    }
    { QSignalBlocker b(m_drive);    m_drive->setValue(t->driveDb()); }
    { QSignalBlocker b(m_bias);     m_bias->setValue(t->biasAmount()); }
    { QSignalBlocker b(m_tone);     m_tone->setValue(t->tone()); }
    { QSignalBlocker b(m_output);   m_output->setValue(t->outputGainDb()); }
    setDryWetMix(t->dryWet());
    { QSignalBlocker b(m_envelope); m_envelope->setValue(t->envelopeAmount()); }
    { QSignalBlocker b(m_release);  m_release->setValue(t->releaseMs()); }

    if (m_outMeter) m_outMeter->setPeakDb(t->outputPeakDb());

    m_restoring = false;
}

void StripTubePanel::setDryWetMix(float mix)
{
    if (!m_dryWet) return;
    const int pct = std::clamp(static_cast<int>(std::lround(mix * 100.0f)), 0, 100);
    if (m_dryWet->value() == pct) return;
    QSignalBlocker b(m_dryWet);
    m_dryWet->setValue(pct);
    // A blocked setValue() skips the lambda that owns the reading.
    if (m_dryWetValue) m_dryWetValue->setText(QString::number(pct) + " %");
}

void StripTubePanel::applyModel(int idx)
{
    if (m_restoring || !m_audio) return;
    tube()->setModel(
        idx == 1 ? ClientTube::Model::B :
        idx == 2 ? ClientTube::Model::C :
                   ClientTube::Model::A);
    saveTubeSettings();
    if (m_curve) m_curve->update();
}

void StripTubePanel::applyDrive(float db)
{
    if (m_restoring || !m_audio) return;
    tube()->setDriveDb(db);
    saveTubeSettings();
    if (m_curve) m_curve->update();
}

void StripTubePanel::applyBias(float v)
{
    if (m_restoring || !m_audio) return;
    tube()->setBiasAmount(v);
    saveTubeSettings();
    if (m_curve) m_curve->update();
}

void StripTubePanel::applyTone(float v)
{
    if (m_restoring || !m_audio) return;
    tube()->setTone(v);
    saveTubeSettings();
}

void StripTubePanel::applyOutput(float db)
{
    if (m_restoring || !m_audio) return;
    tube()->setOutputGainDb(db);
    saveTubeSettings();
}

void StripTubePanel::applyDryWet(float v)
{
    if (m_restoring || !m_audio) return;
    tube()->setDryWet(v);
    saveTubeSettings();
}

void StripTubePanel::applyEnvelope(float v)
{
    if (m_restoring || !m_audio) return;
    tube()->setEnvelopeAmount(v);
    saveTubeSettings();
}


void StripTubePanel::applyRelease(float ms)
{
    if (m_restoring || !m_audio) return;
    tube()->setReleaseMs(ms);
    saveTubeSettings();
}

void StripTubePanel::saveGeometryToSettings()
{
    if (m_restoring) return;
    AppSettings::instance().setValue(
        "StripTubePanelGeometry",
        QString::fromLatin1(saveGeometry().toBase64()));
}

void StripTubePanel::restoreGeometryFromSettings()
{
    m_restoring = true;
    const QString b64 = AppSettings::instance()
        .value("StripTubePanelGeometry", "").toString();
    if (!b64.isEmpty()) {
        restoreGeometry(QByteArray::fromBase64(b64.toLatin1()));
    }
    m_restoring = false;
}

void StripTubePanel::closeEvent(QCloseEvent* ev)
{ saveGeometryToSettings(); QWidget::closeEvent(ev); }
void StripTubePanel::moveEvent(QMoveEvent* ev)
{ saveGeometryToSettings(); QWidget::moveEvent(ev); }
void StripTubePanel::resizeEvent(QResizeEvent* ev)
{ saveGeometryToSettings(); QWidget::resizeEvent(ev); }
void StripTubePanel::showEvent(QShowEvent* ev)
{
    QWidget::showEvent(ev);
    if (m_syncTimer) m_syncTimer->start();
}

void StripTubePanel::hideEvent(QHideEvent* ev)
{
    saveGeometryToSettings();
    // Stacked behind another tab, or the window closed: stop polling.
    if (m_syncTimer) m_syncTimer->stop();
    QWidget::hideEvent(ev);
}

} // namespace AetherSDR
