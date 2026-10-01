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
// and the daxPcmReady feed is connected on runningChanged(true) and torn
// down on runningChanged(false). The engine itself ignores PCM whose
// channel differs from the bound slice's live daxChannel(), and PCM whose
// slice id differs from the bound slice on the seam-native feed.

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

    // Whether this radio has a DAX plane at all. Resolved at call time for the
    // same reason as the provider above — no backend exists at construction.
    // Gates start()'s "no DAX channel assigned" warning, which is a correct
    // diagnosis on a Flex and a misleading one on a backend that demodulates
    // in-process and feeds feedRxSliceAudio() instead.
    m_clockEngine->setDaxAvailabilityProvider(
        [this] { return m_radioModel.hasDaxStreams(); });

    const auto bindAudio = [this] {
        disconnect(m_clockDaxConn);
        disconnect(m_clockSliceAudioConn);
        m_clockDaxConn = {};
        m_clockSliceAudioConn = {};
        if (!m_clockEngine->isRunning()) {
            return;
        }
        const quint64 generation = m_clockEngine->inputGeneration();
        if (auto* ps = m_radioModel.panStream()) {
            m_clockDaxConn = connect(
                ps, &PanadapterStream::daxPcmReady, m_clockEngine,
                [engine = m_clockEngine, generation](int channel, const PcmFrame& frame) {
                    engine->feedRxAudio(channel, frame, generation);
                }, Qt::QueuedConnection);
        }
        m_clockSliceAudioConn = connect(
            &m_radioModel, &RadioModel::backendSliceAudioFrameReady, m_clockEngine,
            [engine = m_clockEngine, generation](int sliceId, const PcmFrame& frame) {
                engine->feedRxSliceAudio(sliceId, frame, generation);
            }, Qt::QueuedConnection);
    };
    // Disconnect alone cannot cancel posted Qt events. Each production callback
    // carries the run/selection generation, checked inside the engine at receipt.
    connect(m_clockEngine, &AetherClockEngine::sourceGenerationChanged,
            this, [bindAudio](quint64) { bindAudio(); });
    connect(&m_radioModel, &RadioModel::connectionStateChanged, m_clockEngine,
            [engine = m_clockEngine](bool connected) {
                if (!connected) {
                    engine->stop();
                }
            });
    connect(&m_radioModel, &RadioModel::backendRebuilt,
            m_clockEngine, &AetherClockEngine::stop);
    connect(m_clockEngine, &AetherClockEngine::runningChanged,
            this, [this, bindAudio](bool running) {
                // The engine is the slice-binding authority; mirror it into
                // the model so `get clock` reports the bound slice.
                m_clockModel->setSliceId(running ? m_clockEngine->boundSliceId()
                                                 : -1);
                bindAudio();
            });

    if (m_appletPanel) {
        if (auto* applet = m_appletPanel->aetherClockApplet()) {
            applet->attach(m_clockEngine, m_clockModel);
            // DAX chooser follows the radio's slice capacity (#4854 review).
            applet->setMaxDaxChannels(m_radioModel.maxSlices());
            connect(&m_radioModel, &RadioModel::infoChanged, applet,
                    [this, applet] { applet->setMaxDaxChannels(m_radioModel.maxSlices()); });
            connect(&m_radioModel, &RadioModel::connectionStateChanged, applet,
                    [this, applet](bool) { applet->setMaxDaxChannels(m_radioModel.maxSlices()); });
        }
    }
}

} // namespace AetherSDR
