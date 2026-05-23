#include "ClientTubeApplet.h"
#include "ClientCompKnob.h"
#include "ClientTubeCurveWidget.h"
#include "core/AudioEngine.h"
#include "core/ClientTube.h"

#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>

namespace MasterSDR {

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

ClientTubeApplet::ClientTubeApplet(Side side, QWidget* parent)
    : QWidget(parent)
    , m_side(side)
{
    buildUI();
    hide();
}

ClientTube* ClientTubeApplet::tube() const
{
    if (!m_audio) return nullptr;
    return m_side == Side::Rx ? m_audio->clientTubeRx()
                              : m_audio->clientTubeTx();
}

void ClientTubeApplet::saveTubeSettings() const
{
    if (!m_audio) return;
    if (m_side == Side::Rx) m_audio->saveClientTubeRxSettings();
    else                    m_audio->saveClientTubeSettings();
}

void ClientTubeApplet::buildUI()
{
    setStyleSheet("QWidget { background: transparent; }");

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(4, 4, 4, 4);
    outer->setSpacing(4);

    m_curve = new ClientTubeCurveWidget;
    m_curve->setCompactMode(true);
    m_curve->setMinimumHeight(90);
    outer->addWidget(m_curve, 1);

    // Enable / Edit buttons removed — CHAIN widget handles bypass
    // (single-click) and editor-open (double-click).

    // Five-knob tuning row — Drive, Tone, Bias, Output, Mix.  Mappings
    // mirror ClientTubeEditor.
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

        m_drive = makeKnob("Drive");
        m_drive->setRange(0.0f, 24.0f);
        m_drive->setDefault(0.0f);
        m_drive->setValueFromNorm([](float n) { return n * 24.0f; });
        m_drive->setNormFromValue([](float v) { return v / 24.0f; });
        m_drive->setLabelFormat([](float v) {
            return QString::number(v, 'f', 1) + " dB";
        });
        row->addWidget(m_drive);

        m_tone = makeKnob("Tone");
        m_tone->setRange(-1.0f, 1.0f);
        m_tone->setDefault(0.0f);
        m_tone->setValueFromNorm([](float n) { return -1.0f + n * 2.0f; });
        m_tone->setNormFromValue([](float v) { return (v + 1.0f) / 2.0f; });
        m_tone->setLabelFormat([](float v) {
            return QString::number(v, 'f', 2);
        });
        row->addWidget(m_tone);

        m_bias = makeKnob("Bias");
        m_bias->setRange(0.0f, 1.0f);
        m_bias->setDefault(0.0f);
        m_bias->setValueFromNorm([](float n) { return n; });
        m_bias->setNormFromValue([](float v) { return v; });
        m_bias->setLabelFormat([](float v) {
            return QString::number(static_cast<int>(v * 100.0f + 0.5f)) + " %";
        });
        row->addWidget(m_bias);

        m_output = makeKnob("Output");
        m_output->setRange(-24.0f, 12.0f);
        m_output->setDefault(0.0f);
        m_output->setValueFromNorm([](float n) { return -24.0f + n * 36.0f; });
        m_output->setNormFromValue([](float v) { return (v + 24.0f) / 36.0f; });
        m_output->setLabelFormat([](float v) {
            return QString::number(v, 'f', 1) + " dB";
        });
        row->addWidget(m_output);

        m_mix = makeKnob("Mix");
        m_mix->setRange(0.0f, 1.0f);
        m_mix->setDefault(1.0f);
        m_mix->setValueFromNorm([](float n) { return n; });
        m_mix->setNormFromValue([](float v) { return v; });
        m_mix->setLabelFormat([](float v) {
            return QString::number(static_cast<int>(v * 100.0f + 0.5f)) + " %";
        });
        row->addWidget(m_mix);

        outer->addLayout(row);
    }

    auto wire = [this](ClientCompKnob* k, auto setter) {
        connect(k, &ClientCompKnob::valueChanged, this, [this, setter](float v) {
            if (!m_audio || !tube()) return;
            (tube()->*setter)(v);
            saveTubeSettings();
        });
    };
    wire(m_drive,  &ClientTube::setDriveDb);
    wire(m_tone,   &ClientTube::setTone);
    wire(m_bias,   &ClientTube::setBiasAmount);
    wire(m_output, &ClientTube::setOutputGainDb);
    wire(m_mix,    &ClientTube::setDryWet);

    // 30 Hz sync timer — mirrors parameter changes made in the floating
    // editor (or elsewhere) back onto the applet knobs.
    m_syncTimer = new QTimer(this);
    m_syncTimer->setInterval(33);
    connect(m_syncTimer, &QTimer::timeout,
            this, &ClientTubeApplet::syncEnableFromEngine);
}

void ClientTubeApplet::setAudioEngine(AudioEngine* engine)
{
    m_audio = engine;
    if (!m_audio) return;
    m_curve->setTube(tube());
    syncEnableFromEngine();
    if (m_syncTimer) m_syncTimer->start();
}

void ClientTubeApplet::syncEnableFromEngine()
{
    if (m_curve) m_curve->update();

    // Bypass dim — render the whole tile at reduced opacity when the
    // stage is bypassed, matching the dim effect on the EQ curve.
    const bool dspEnabled = (m_audio && tube()) ? tube()->isEnabled() : true;
    auto* eff = qobject_cast<QGraphicsOpacityEffect*>(graphicsEffect());
    if (!eff) {
        eff = new QGraphicsOpacityEffect(this);
        setGraphicsEffect(eff);
    }
    eff->setOpacity(dspEnabled ? 1.0 : 0.55);

    if (!m_audio || !tube()) return;
    ClientTube* t = tube();
    if (m_drive)  { QSignalBlocker b(m_drive);  m_drive->setValue(t->driveDb()); }
    if (m_tone)   { QSignalBlocker b(m_tone);   m_tone->setValue(t->tone()); }
    if (m_bias)   { QSignalBlocker b(m_bias);   m_bias->setValue(t->biasAmount()); }
    if (m_output) { QSignalBlocker b(m_output); m_output->setValue(t->outputGainDb()); }
    if (m_mix)    { QSignalBlocker b(m_mix);    m_mix->setValue(t->dryWet()); }
}

void ClientTubeApplet::refreshEnableFromEngine()
{
    syncEnableFromEngine();
}

void ClientTubeApplet::onEnableToggled(bool on)
{
    if (!m_audio) return;
    ClientTube* t = tube();
    if (!t) return;
    t->setEnabled(on);
    saveTubeSettings();
    if (m_curve) m_curve->update();
}

} // namespace MasterSDR
