#include "ClientEqApplet.h"
#include "ClientEqCurveWidget.h"
#include "core/AudioEngine.h"
#include "core/ClientEq.h"

#include <QVBoxLayout>

namespace MasterSDR {

ClientEqApplet::ClientEqApplet(Path path, QWidget* parent)
    : QWidget(parent)
    , m_currentPath(path)
{
    buildUI();
    hide();  // hidden until toggled on from the button tray
}

void ClientEqApplet::buildUI()
{
    setStyleSheet("QWidget { background: transparent; }");

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(4, 4, 4, 4);
    outer->setSpacing(4);

    // Curve / analyzer.  Rx/Tx selector lived above this in the prior
    // shared-applet design; with one instance per side, the tile is
    // pure curve + analyzer and the side comes from m_currentPath.
    m_curve = new ClientEqCurveWidget;
    m_curve->setMinimumHeight(110);
    outer->addWidget(m_curve, 1);
}

void ClientEqApplet::setAudioEngine(AudioEngine* engine)
{
    m_audio = engine;
    if (!m_audio || !m_curve) return;
    m_curve->setEq(m_currentPath == Path::Rx
                       ? m_audio->clientEqRx()
                       : m_audio->clientEqTx());
    syncEnableFromEngine();
}

void ClientEqApplet::syncEnableFromEngine()
{
    if (m_curve) m_curve->update();
}

void ClientEqApplet::refreshEnableFromEngine()
{
    syncEnableFromEngine();
}

void ClientEqApplet::setTxFilterCutoffs(int lowHz, int highHz)
{
    if (!m_curve || m_currentPath != Path::Tx) return;
    m_curve->setFilterCutoffs(lowHz, highHz);
}

void ClientEqApplet::setRxFilterCutoffs(int audioLowHz, int audioHighHz)
{
    if (!m_curve || m_currentPath != Path::Rx) return;
    m_curve->setFilterCutoffs(audioLowHz, audioHighHz);
}

} // namespace MasterSDR
