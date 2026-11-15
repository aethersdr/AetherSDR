#include "ClientRxDspApplet.h"
#include "AetherDspWidget.h"
#include "core/AudioEngine.h"
#include "core/ThemeManager.h"

#include <QVBoxLayout>

namespace AetherSDR {

ClientRxDspApplet::ClientRxDspApplet(QWidget* parent) : QWidget(parent)
{
    theme::setContainer(this, QStringLiteral("applet/rxdsp"));
    setStyleSheet("QWidget { background: transparent; }");
    // Cap horizontal width so the embedded AetherDspWidget can't push the
    // PooDoo container wider than its 280 px slot — sliders + tab bar
    // shrink to fit instead of clipping.
    setMaximumWidth(250);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    hide();  // hidden until setAudioEngine wires it up + side filter shows it
}

void ClientRxDspApplet::setAudioEngine(AudioEngine* engine)
{
    m_audio = engine;
    if (!m_audio || m_widget) return;
    m_widget = new AetherDspWidget(m_audio, this);
    m_widget->setCompactMode(true);
    if (auto* lay = qobject_cast<QVBoxLayout*>(layout()))
        lay->addWidget(m_widget);
}

} // namespace AetherSDR
