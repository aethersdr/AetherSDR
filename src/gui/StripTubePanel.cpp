#include "StripTubePanel.h"
#include "ClientCompKnob.h"
#include "ClientLevelMeter.h"
#include "ClientTubeCurveWidget.h"
#include "EditorFramelessTitleBar.h"
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
#include <QVBoxLayout>
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

    auto* body = new QHBoxLayout;
    body->setSpacing(12);

    // ── Left column: Dry/Wet, Output, Drive ─────────────────────────
    auto* left = new QVBoxLayout;
    left->setSpacing(10);

    m_dryWet = new ClientCompKnob;
    m_dryWet->setLabel("Dry/Wet");
    m_dryWet->setCenterLabelMode(true);
    m_dryWet->setRange(0.0f, 1.0f);
    m_dryWet->setDefault(1.0f);
    m_dryWet->setValueFromNorm([](float n) { return n; });
    m_dryWet->setNormFromValue([](float v) { return v; });
    m_dryWet->setLabelFormat([](float v) {
        return QString::number(static_cast<int>(v * 100.0f + 0.5f)) + " %";
    });
    m_dryWet->setFixedSize(76, 76);
    connect(m_dryWet, &ClientCompKnob::valueChanged,
            this, &StripTubePanel::applyDryWet);
    left->addWidget(m_dryWet, 0, Qt::AlignHCenter);

    m_output = new ClientCompKnob;
    m_output->setLabel("Output");
    m_output->setCenterLabelMode(true);
    m_output->setRange(-24.0f, 12.0f);
    m_output->setDefault(0.0f);
    m_output->setValueFromNorm([](float n) { return -24.0f + n * 36.0f; });
    m_output->setNormFromValue([](float v) { return (v + 24.0f) / 36.0f; });
    m_output->setLabelFormat([](float v) {
        return QString::number(v, 'f', 2) + " dB";
    });
    m_output->setFixedSize(76, 76);
    connect(m_output, &ClientCompKnob::valueChanged,
            this, &StripTubePanel::applyOutput);
    left->addWidget(m_output, 0, Qt::AlignHCenter);

    m_drive = new ClientCompKnob;
    m_drive->setLabel("Drive");
    m_drive->setCenterLabelMode(true);
    m_drive->setRange(0.0f, 24.0f);
    m_drive->setDefault(0.0f);
    m_drive->setValueFromNorm([](float n) { return n * 24.0f; });
    m_drive->setNormFromValue([](float v) { return v / 24.0f; });
    m_drive->setLabelFormat([](float v) {
        return QString::number(v, 'f', 2) + " dB";
    });
    m_drive->setFixedSize(76, 76);
    connect(m_drive, &ClientCompKnob::valueChanged,
            this, &StripTubePanel::applyDrive);
    left->addWidget(m_drive, 0, Qt::AlignHCenter);

    left->addStretch();
    body->addLayout(left, 0);

    // ── Centre column: curve + model buttons + Tone/Bias ────────────
    auto* center = new QVBoxLayout;
    center->setSpacing(6);

    m_curve = new ClientTubeCurveWidget;
    m_curve->setCompactMode(false);
    m_curve->setMinimumHeight(180);
    center->addWidget(m_curve, 1);

    // Model A / B / C selector now lives in the header row so it
    // doesn't eat vertical space from the curve.  See the header block
    // above for where the buttons are actually inserted.

    // Tone + Bias row
    {
        auto* row = new QHBoxLayout;
        row->setSpacing(12);
        row->addStretch();

        m_tone = new ClientCompKnob;
        m_tone->setLabel("Tone");
        m_tone->setCenterLabelMode(true);
        m_tone->setRange(-1.0f, 1.0f);
        m_tone->setDefault(0.0f);
        m_tone->setValueFromNorm([](float n) { return -1.0f + n * 2.0f; });
        m_tone->setNormFromValue([](float v) { return (v + 1.0f) / 2.0f; });
        m_tone->setLabelFormat([](float v) {
            return QString::number(v, 'f', 2);
        });
        m_tone->setFixedSize(76, 76);
        connect(m_tone, &ClientCompKnob::valueChanged,
                this, &StripTubePanel::applyTone);
        row->addWidget(m_tone, 0, Qt::AlignHCenter);

        // A / B / C model selector — narrow vertical stack sitting
        // between Tone and Bias.  Buttons live in m_modelGroup created
        // earlier so the change handler stays wired.
        {
            auto* modelCol = new QVBoxLayout;
            modelCol->setSpacing(3);
            auto addModelBtn = [&](const QString& label, int idx) {
                auto* btn = new QPushButton(label);
                btn->setCheckable(true);
                btn->setStyleSheet(kModelStyle);
                btn->setFixedSize(22, 20);
                m_modelGroup->addButton(btn, idx);
                modelCol->addWidget(btn);
                return btn;
            };
            m_modelA = addModelBtn("A", 0);
            m_modelB = addModelBtn("B", 1);
            m_modelC = addModelBtn("C", 2);
            row->addLayout(modelCol, 0);
        }

        m_bias = new ClientCompKnob;
        m_bias->setLabel("Bias");
        m_bias->setCenterLabelMode(true);
        m_bias->setRange(0.0f, 1.0f);
        m_bias->setDefault(0.0f);
        m_bias->setValueFromNorm([](float n) { return n; });
        m_bias->setNormFromValue([](float v) { return v; });
        m_bias->setLabelFormat([](float v) {
            return QString::number(static_cast<int>(v * 100.0f + 0.5f)) + " %";
        });
        m_bias->setFixedSize(76, 76);
        connect(m_bias, &ClientCompKnob::valueChanged,
                this, &StripTubePanel::applyBias);
        row->addWidget(m_bias, 0, Qt::AlignHCenter);

        row->addStretch();
        center->addLayout(row);
    }

    body->addLayout(center, 1);

    // ── Right column: Envelope / Attack / Release ───────────────────
    auto* right = new QVBoxLayout;
    right->setSpacing(10);

    m_envelope = new ClientCompKnob;
    m_envelope->setLabel("Envelope");
    m_envelope->setCenterLabelMode(true);
    m_envelope->setRange(-1.0f, 1.0f);
    m_envelope->setDefault(0.0f);
    m_envelope->setValueFromNorm([](float n) { return -1.0f + n * 2.0f; });
    m_envelope->setNormFromValue([](float v) { return (v + 1.0f) / 2.0f; });
    m_envelope->setLabelFormat([](float v) {
        const int pct = static_cast<int>(v * 100.0f + (v >= 0 ? 0.5f : -0.5f));
        return QString::number(pct) + " %";
    });
    m_envelope->setFixedSize(76, 76);
    connect(m_envelope, &ClientCompKnob::valueChanged,
            this, &StripTubePanel::applyEnvelope);
    right->addWidget(m_envelope, 0, Qt::AlignHCenter);

    m_attack = new ClientCompKnob;
    m_attack->setLabel("Attack");
    m_attack->setCenterLabelMode(true);
    m_attack->setRange(0.1f, 30.0f);
    m_attack->setDefault(5.0f);
    m_attack->setValueFromNorm([](float n) {
        return 0.1f * std::pow(300.0f, n);
    });
    m_attack->setNormFromValue([](float v) {
        return std::log(std::max(0.1f, v) / 0.1f) / std::log(300.0f);
    });
    m_attack->setLabelFormat([](float v) {
        return QString::number(v, 'f', v < 10.0f ? 2 : 1) + " ms";
    });
    m_attack->setFixedSize(76, 76);
    connect(m_attack, &ClientCompKnob::valueChanged,
            this, &StripTubePanel::applyAttack);
    right->addWidget(m_attack, 0, Qt::AlignHCenter);

    m_release = new ClientCompKnob;
    m_release->setLabel("Release");
    m_release->setCenterLabelMode(true);
    m_release->setRange(10.0f, 500.0f);
    m_release->setDefault(35.0f);
    m_release->setValueFromNorm([](float n) {
        return 10.0f * std::pow(50.0f, n);
    });
    m_release->setNormFromValue([](float v) {
        return std::log(std::max(10.0f, v) / 10.0f) / std::log(50.0f);
    });
    m_release->setLabelFormat([](float v) {
        return QString::number(v, 'f', v < 100.0f ? 2 : 1) + " ms";
    });
    m_release->setFixedSize(76, 76);
    connect(m_release, &ClientCompKnob::valueChanged,
            this, &StripTubePanel::applyRelease);
    right->addWidget(m_release, 0, Qt::AlignHCenter);

    right->addStretch();
    body->addLayout(right, 0);

    // ── Output level meter — far-right column, mirrors EQ editor ────
    // In the strip, the TX-side instance shortens the meter and tucks
    // a small RN2 toggle directly beneath it.  RX-side keeps the
    // default full-height meter (no button).  (#2813)
    m_outMeter = new ClientLevelMeter;

    m_rn2Btn = new QPushButton("RN2", this);
    m_rn2Btn->setCheckable(true);
    // Match the A/B/C model-button footprint so the toggle visually
    // belongs to the same family of compact panel buttons.  (#2813)
    m_rn2Btn->setFixedSize(22, 20);
    m_rn2Btn->setVisible(false);  // flipped on by showForTx()
    m_rn2Btn->setToolTip(tr(
        "Toggle RNNoise neural denoiser on the mic input.  Runs before "
        "any DSP chain stage so noise is suppressed before it can be "
        "amplified by gate / compressor / saturator.  Voice modes only — "
        "digital modes (RADE, DAX, RTTY, FT8, FDV, CW) bypass this stage.  "
        "Saved per Channel Strip profile, and suppressed by the strip's "
        "BYPASS button alongside every other voice stage."));
    // Reuse the A/B/C model-button stylesheet so all four compact
    // buttons in this panel share one visual idiom.  (#2813)
    m_rn2Btn->setStyleSheet(kModelStyle);
    connect(m_rn2Btn, &QPushButton::toggled, this, [this](bool on) {
        if (m_audio) m_audio->setRn2TxEnabled(on);
        // Direct setter call is the single source of truth — engine
        // emits rn2TxEnabledChanged for any cross-widget observer.
    });

    // Meter stretches to fill all available vertical space; RN2 button
    // sits flush at the bottom, horizontally centered.  Stretch factor 1
    // on the meter, 0 on the button so the meter absorbs all surplus
    // height when the panel grows.  (#2813)
    auto* meterCol = new QVBoxLayout;
    meterCol->setContentsMargins(0, 0, 0, 0);
    meterCol->setSpacing(4);
    meterCol->addWidget(m_outMeter, 1);
    meterCol->addWidget(m_rn2Btn, 0, Qt::AlignHCenter);
    body->addLayout(meterCol);

    root->addLayout(body);

    if (m_audio && tube()) {
        m_curve->setTube(tube());
    }

    syncControlsFromEngine();

    m_syncTimer = new QTimer(this);
    m_syncTimer->setInterval(33);
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
    // RN2 toggle is TX-only.  Meter keeps its default Expanding policy
    // so it absorbs all column height except the 20 px the RN2 button
    // plus the 4 px gap occupy at the bottom.  (#2813)
    if (m_rn2Btn) {
        m_rn2Btn->setVisible(true);
        if (m_audio) {
            QSignalBlocker block(m_rn2Btn);
            m_rn2Btn->setChecked(m_audio->rn2TxEnabled());
        }
    }
    syncControlsFromEngine();
    restoreGeometryFromSettings();
    show();
    raise();
    activateWindow();
    if (m_syncTimer) m_syncTimer->start();
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
    // RX side has its own RN2 toggle elsewhere (AetherDspWidget /
    // ClientRxChainWidget).  Hide our copy — meter keeps its default
    // Expanding policy so it fills the full column.  (#2813)
    if (m_rn2Btn) m_rn2Btn->setVisible(false);
    syncControlsFromEngine();
    restoreGeometryFromSettings();
    show();
    raise();
    activateWindow();
    if (m_syncTimer) m_syncTimer->start();
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
    { QSignalBlocker b(m_dryWet);   m_dryWet->setValue(t->dryWet()); }
    { QSignalBlocker b(m_envelope); m_envelope->setValue(t->envelopeAmount()); }
    { QSignalBlocker b(m_attack);   m_attack->setValue(t->attackMs()); }
    { QSignalBlocker b(m_release);  m_release->setValue(t->releaseMs()); }

    if (m_outMeter) m_outMeter->setPeakDb(t->outputPeakDb());

    m_restoring = false;
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

void StripTubePanel::applyAttack(float ms)
{
    if (m_restoring || !m_audio) return;
    tube()->setAttackMs(ms);
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
{ QWidget::showEvent(ev); }
void StripTubePanel::hideEvent(QHideEvent* ev)
{ saveGeometryToSettings(); QWidget::hideEvent(ev); }

} // namespace AetherSDR
