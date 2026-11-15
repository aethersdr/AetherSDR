#include "ClientLevelerApplet.h"

#include "ClientCompKnob.h"
#include "ClientLevelerHistoryWidget.h"
#include "StripLevelerPanel.h"
#include "core/AudioEngine.h"
#include "core/ClientLeveler.h"

#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

namespace AetherSDR {

ClientLevelerApplet::ClientLevelerApplet(QWidget* parent)
    : QWidget(parent)
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(6, 4, 6, 4);
    outer->setSpacing(4);

    m_history = new ClientLevelerHistoryWidget;
    m_history->setFixedHeight(70);
    outer->addWidget(m_history);

    auto* row = new QHBoxLayout;
    row->addStretch();
    m_maxGain = LevelerKnobs::makeMaxGain(56);
    m_decay = LevelerKnobs::makeDecay(56);
    row->addWidget(m_maxGain);
    row->addWidget(m_decay);
    row->addStretch();
    outer->addLayout(row);

    const auto wire = [this](ClientCompKnob* knob, void (ClientLeveler::*setter)(float)) {
        connect(knob, &ClientCompKnob::valueChanged, this, [this, setter](float v) {
            // Local object operands: see VfoWidget::buildTabContent (#6724).
            if (auto* const lev = m_audio ? m_audio->clientLevelerTx() : nullptr) {
                (lev->*setter)(v);
                m_audio->saveClientLevelerSettings();
            }
        });
    };
    wire(m_maxGain, &ClientLeveler::setMaxGainDb);
    wire(m_decay, &ClientLeveler::setDecayMs);

    m_poll = new QTimer(this);
    m_poll->setInterval(33);
    connect(m_poll, &QTimer::timeout, this, [this]() {
        auto* const lev = m_audio ? m_audio->clientLevelerTx() : nullptr;
        if (lev && lev->isEnabled()) {
            m_history->push(lev->gainDb());
        }
    });
}

void ClientLevelerApplet::setAudioEngine(AudioEngine* engine)
{
    m_audio = engine;
    if (!m_audio) {
        return;
    }
    syncFromEngine();
    connect(m_audio, &AudioEngine::clientLevelerStateChanged, this,
            &ClientLevelerApplet::syncFromEngine);
}

void ClientLevelerApplet::refreshEnableFromEngine()
{
    syncFromEngine();
}

void ClientLevelerApplet::syncFromEngine()
{
    auto* const lev = m_audio ? m_audio->clientLevelerTx() : nullptr;
    auto* effect = qobject_cast<QGraphicsOpacityEffect*>(graphicsEffect());
    if (!effect) {
        effect = new QGraphicsOpacityEffect(this);
        setGraphicsEffect(effect);
    }
    effect->setOpacity(!lev || lev->isEnabled() ? 1.0 : 0.55);
    if (!lev) {
        return;
    }
    const QSignalBlocker b1(m_maxGain);
    const QSignalBlocker b2(m_decay);
    m_maxGain->setValue(lev->maxGainDb());
    m_decay->setValue(lev->decayMs());
    m_history->setMaxGainDb(lev->maxGainDb());
    m_history->setActive(lev->isEnabled());
}

void ClientLevelerApplet::showEvent(QShowEvent* ev)
{
    QWidget::showEvent(ev);
    m_poll->start();
}

void ClientLevelerApplet::hideEvent(QHideEvent* ev)
{
    QWidget::hideEvent(ev);
    m_poll->stop();
}

}  // namespace AetherSDR
