#include "ClientCfcApplet.h"

#include "ClientCfcCurveWidget.h"
#include "ClientCompKnob.h"
#include "core/AudioEngine.h"
#include "core/ClientCfc.h"

#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <cmath>

namespace AetherSDR {

ClientCfcApplet::ClientCfcApplet(QWidget* parent)
    : QWidget(parent)
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(6, 4, 6, 4);
    outer->setSpacing(4);

    m_curve = new ClientCfcCurveWidget;
    m_curve->setReadOnly(true);
    m_curve->setFixedHeight(90);
    outer->addWidget(m_curve);

    auto* row = new QHBoxLayout;
    row->setSpacing(8);
    const auto makeKnob = [](const QString& label, float lo, float hi) {
        auto* k = new ClientCompKnob;
        k->setLabel(label);
        k->setCenterLabelMode(true);
        k->setFixedSize(56, 56);
        k->setRange(lo, hi);
        k->setDefault(0.0f);
        k->setValueFromNorm([lo, hi](float n) { return lo + n * (hi - lo); });
        k->setNormFromValue([lo, hi](float v) { return (v - lo) / (hi - lo); });
        k->setLabelFormat([](float v) { return QString::number(v, 'f', 1) + " dB"; });
        return k;
    };
    m_precomp = makeKnob(tr("Pre-comp"), 0.0f, ClientCfc::kMaxPrecompDb);
    m_postGain = makeKnob(tr("Post"), -ClientCfc::kMaxPostGainDb, ClientCfc::kMaxPostGainDb);
    row->addStretch();
    row->addWidget(m_precomp);
    row->addWidget(m_postGain);
    m_fftLabel = new QLabel;
    row->addWidget(m_fftLabel);
    row->addStretch();
    outer->addLayout(row);

    const auto wire = [this](ClientCompKnob* knob, void (ClientCfc::*setter)(float)) {
        connect(knob, &ClientCompKnob::valueChanged, this, [this, setter](float v) {
            // Local object operands: see VfoWidget::buildTabContent (#6724).
            if (auto* const cfc = m_audio ? m_audio->clientCfcTx() : nullptr) {
                (cfc->*setter)(v);
                m_audio->saveClientCfcSettings();
            }
        });
    };
    wire(m_precomp, &ClientCfc::setPrecompDb);
    wire(m_postGain, &ClientCfc::setPostGainDb);
}

void ClientCfcApplet::setAudioEngine(AudioEngine* engine)
{
    m_audio = engine;
    if (!m_audio) {
        return;
    }
    syncFromEngine();
    connect(m_audio, &AudioEngine::clientCfcStateChanged, this, &ClientCfcApplet::syncFromEngine);
}

void ClientCfcApplet::refreshEnableFromEngine()
{
    syncFromEngine();
}

void ClientCfcApplet::syncFromEngine()
{
    auto* const cfc = m_audio ? m_audio->clientCfcTx() : nullptr;
    auto* effect = qobject_cast<QGraphicsOpacityEffect*>(graphicsEffect());
    if (!effect) {
        effect = new QGraphicsOpacityEffect(this);
        setGraphicsEffect(effect);
    }
    effect->setOpacity(!cfc || cfc->isEnabled() ? 1.0 : 0.55);
    if (!cfc) {
        return;
    }
    m_curve->setBands(cfc->bands());
    m_curve->setPostEqActive(cfc->postEqEnabled());
    {
        const QSignalBlocker b1(m_precomp);
        const QSignalBlocker b2(m_postGain);
        m_precomp->setValue(cfc->precompDb());
        m_postGain->setValue(cfc->postGainDb());
    }
    const QString reason =
        cfc->postEqEnabled() ? QString{} : tr("Post gain applies only with Post-EQ on");
    m_postGain->setEnabled(cfc->postEqEnabled());
    m_postGain->setToolTip(reason);
    m_postGain->setAccessibleDescription(reason);
    m_fftLabel->setText(tr("FFT %1\n%2 ms")
                            .arg(cfc->fftSize())
                            .arg(std::lround(cfc->fftSize() * 1000.0 / 48000.0)));
}

}  // namespace AetherSDR
