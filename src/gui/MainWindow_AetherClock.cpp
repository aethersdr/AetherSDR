// MainWindow_AetherClock.cpp — AetherClock wiring for MainWindow.
//
// Constructs the AetherClock engine + model pair and connects them to the
// rest of the app:
//
//   • AetherClockEngine (core) — decodes WWV/WWVB from the bound slice's
//     DAX RX audio; owns the DAX-hold lifecycle through the injected
//     provider below (never a private stream registration).
//   • AetherClockModel — first-class Q_PROPERTY mirror consumed by the
//     strip applet and the automation bridge's `get clock` verb.
//   • AetherClockApplet (strip) — receives the engine action surface +
//     model via attach(); slice binding rides AppletPanel::setSlice.
//
// The pan stream is backend-owned and does not exist until a radio session
// is up, so nothing here touches it at construction time: the DAX-hold
// provider resolves panStream() at call time (the engine only drives it
// while started, which requires a live slice and therefore a live backend),
// and the daxAudioReady feed is connected on runningChanged(true) and torn
// down on runningChanged(false). The engine itself ignores PCM whose
// channel differs from the bound slice's live daxChannel().

#include "MainWindow.h"

#include "AppletPanel.h"
#include "AetherClockApplet.h"
#include "core/AetherClockEngine.h"
#include "models/AetherClockModel.h"
#include "models/RadioModel.h"  // brings the pan stream seam (tracked baseline);
                                // no direct vendor include above the seam (EB3)

namespace AetherSDR {

void MainWindow::setupAetherClock()
{
    m_clockEngine = new AetherClockEngine(this);
    m_clockModel = new AetherClockModel(this);
    m_clockModel->attachEngine(m_clockEngine);

    // DAX hold via the central per-channel consumer registry (#3305
    // pattern). Resolved at call time — see the file comment.
    m_clockEngine->setDaxChannelProvider(
        [this](int ch) {
            if (auto* ps = m_radioModel.panStream())
                ps->acquireDaxChannel(ch, PanadapterStream::DaxConsumer::Clock);
        },
        [this](int ch) {
            if (auto* ps = m_radioModel.panStream())
                ps->releaseDaxChannel(ch, PanadapterStream::DaxConsumer::Clock);
        });

    connect(m_clockEngine, &AetherClockEngine::runningChanged,
            this, [this](bool running) {
                // The engine is the slice-binding authority; mirror it into
                // the model so `get clock` reports the bound slice.
                m_clockModel->setSliceId(running ? m_clockEngine->boundSliceId()
                                                 : -1);
                if (running) {
                    auto* ps = m_radioModel.panStream();
                    if (ps && !m_clockDaxConn)
                        m_clockDaxConn = connect(
                            ps, &PanadapterStream::daxAudioReady,
                            m_clockEngine, &AetherClockEngine::feedRxAudio,
                            Qt::QueuedConnection);
                } else if (m_clockDaxConn) {
                    disconnect(m_clockDaxConn);
                    m_clockDaxConn = {};
                }
            });

    if (m_appletPanel) {
        if (auto* applet = m_appletPanel->aetherClockApplet())
            applet->attach(m_clockEngine, m_clockModel);
    }
}

} // namespace AetherSDR
